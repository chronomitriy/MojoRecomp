// Windows-only: exercises the real write-protection tracking of
// GuestMemorySnapshot (vectored write faults on aliased physical views) for
// ranges inside command memory, with the default policy. Synthetic data only.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../gpu/guest_memory_snapshot.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

struct Counters
{
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t faults = 0;
    uint64_t protects = 0;
    uint64_t commandCaptures = 0;
};

Counters Read()
{
    using namespace mojorecomp::gpu;
    return {GuestSnapshotCacheHitCount(), GuestSnapshotCacheMissCount(),
            GuestSnapshotWriteFaultCount(), GuestSnapshotProtectCallCount(),
            GuestSnapshotCommandCaptureCount()};
}

} // namespace

int main()
{
    using namespace mojorecomp::gpu;

    constexpr size_t physicalBytes = 0x100000;
    constexpr uintptr_t physicalViews[] = {0xA0000000ull, 0xC0000000ull, 0xE0000000ull};
    HANDLE process = GetCurrentProcess();
    auto* base = static_cast<uint8_t*>(VirtualAlloc2(
        process, nullptr, 1ull << 32, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS, nullptr, 0));
    if (!base)
        return Fail("could not reserve guest address space");
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE | SEC_COMMIT, 0, physicalBytes, nullptr);
    if (!mapping)
        return Fail("could not create shared physical backing");
    for (uintptr_t view : physicalViews)
    {
        if (!VirtualFree(base + view, physicalBytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !MapViewOfFile3(mapping, process, base + view, 0, physicalBytes,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            return Fail("could not map the physical alias views");
    }
    CloseHandle(mapping);

    auto capture = [&](uint32_t address, uint32_t bytes, uint8_t expect) {
        GuestMemorySnapshot snapshot;
        if (!snapshot.Capture(base, address, bytes))
            return false;
        const uint8_t* data = snapshot.Resolve(address, bytes);
        return data && data[0] == expect && data[bytes - 1] == expect;
    };
    auto fill = [&](uint32_t address, uint32_t bytes, uint8_t value) {
        for (uint32_t i = 0; i < bytes; ++i)
            base[address + i] = value;
    };

    // Four pages of "command buffer": a resolve rectangle and a BeginVertices
    // batch live inside it and are rewritten on every lap.
    constexpr uint32_t commandAddress = 0xA0010000u;
    constexpr uint32_t commandBytes = 0x4000;
    constexpr uint32_t rectAddress = commandAddress + 0x100;
    constexpr uint32_t rectBytes = 24;
    constexpr uint32_t batchAddress = commandAddress + 0x1F00; // spans two pages
    constexpr uint32_t batchBytes = 0x400;
    GuestSnapshotNoteCommandMemory(commandAddress, commandBytes);

    const Counters start = Read();
    for (uint8_t lap = 1; lap <= 4; ++lap)
    {
        fill(commandAddress, commandBytes, lap);
        if (!capture(rectAddress, rectBytes, lap) || !capture(batchAddress, batchBytes, lap) ||
            !capture(rectAddress, rectBytes, lap))
            return Fail("a command-memory capture returned stale bytes");
    }
    Counters now = Read();
    if (now.faults != start.faults || now.protects != start.protects)
        return Fail("command memory was write-protected or faulted");
    if (now.commandCaptures != start.commandCaptures + 12 || now.hits != start.hits ||
        now.misses != start.misses)
        return Fail("command-memory captures did not bypass the protected cache");

    // An ordinary range next to command memory keeps the protected cache.
    constexpr uint32_t staticAddress = 0xA0020000u;
    constexpr uint32_t staticBytes = 256;
    fill(staticAddress, staticBytes, 7);
    if (!capture(staticAddress, staticBytes, 7) || !capture(staticAddress, staticBytes, 7))
        return Fail("ordinary capture returned wrong bytes");
    const Counters ordinary = Read();
    if (ordinary.misses != now.misses + 1 || ordinary.hits != now.hits + 1 ||
        ordinary.protects != now.protects + 1 || ordinary.commandCaptures != now.commandCaptures)
        return Fail("an ordinary range was not protected once and then served from cache");
    fill(staticAddress, staticBytes, 8);
    if (Read().faults != ordinary.faults + 1 || !capture(staticAddress, staticBytes, 8))
        return Fail("ordinary tracking lost its write fault or served stale bytes");

    // A range above the size limit that reaches into command memory keeps the
    // protected path (all its pages protected, so it may be cached).
    constexpr uint32_t largeAddress = 0xA0000000u;
    constexpr uint32_t largeBytes = 0x50000; // 320 KiB, crosses the command pages
    fill(largeAddress, 16, 9);
    fill(largeAddress + largeBytes - 16, 16, 9);
    fill(commandAddress, commandBytes, 9);
    now = Read();
    if (!capture(largeAddress, largeBytes, 9))
        return Fail("large capture returned wrong bytes");
    const Counters large = Read();
    if (large.commandCaptures != now.commandCaptures || large.protects <= now.protects)
        return Fail("a large range was copied without protection");
    // The next lap faults once on the command pages the large range protected;
    // small command captures then stay unprotected again.
    fill(commandAddress, commandBytes, 10);
    if (!capture(rectAddress, rectBytes, 10) || !capture(batchAddress, batchBytes, 10))
        return Fail("command capture after a large range returned stale bytes");
    const Counters after = Read();
    if (after.faults <= large.faults || after.protects != large.protects ||
        after.commandCaptures != large.commandCaptures + 2)
        return Fail("command pages were re-protected after a large range");

    GuestSnapshotCacheShutdown();
    std::puts("PASS: command-memory snapshots are copied fresh and never write-protected.");
    return 0;
}
