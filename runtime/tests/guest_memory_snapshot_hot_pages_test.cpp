// Windows-only: exercises the real write-protection tracking of
// GuestMemorySnapshot (vectored write faults on aliased physical views) with
// the opt-in hot-page policy enabled. Synthetic data only.

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
    uint64_t volatileCaptures = 0;
};

Counters Read()
{
    using namespace mojorecomp::gpu;
    return {GuestSnapshotCacheHitCount(), GuestSnapshotCacheMissCount(),
            GuestSnapshotWriteFaultCount(), GuestSnapshotProtectCallCount(),
            GuestSnapshotVolatileCaptureCount()};
}

} // namespace

int main()
{
    using namespace mojorecomp::gpu;
    if (_putenv_s("MOJORECOMP_SNAPSHOT_HOT_PAGES", "1") != 0)
        return Fail("could not enable the hot-page policy");

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

    constexpr uint32_t dynamicAddress = 0xA0002000u; // one page, rewritten "every frame"
    constexpr uint32_t dynamicBytes = 256;
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

    fill(dynamicAddress, dynamicBytes, 1);
    const Counters start = Read();
    if (!capture(dynamicAddress, dynamicBytes, 1) || !capture(dynamicAddress, dynamicBytes, 1))
        return Fail("initial captures returned wrong bytes");
    Counters now = Read();
    if (now.misses != start.misses + 1 || now.hits != start.hits + 1 ||
        now.protects != start.protects + 1)
        return Fail("a clean range was not protected once and then served from cache");

    // First rewrite: one fault, then a normal protected recapture.
    fill(dynamicAddress, dynamicBytes, 2);
    if (Read().faults != now.faults + 1)
        return Fail("a write to a tracked page did not fault exactly once");
    if (!capture(dynamicAddress, dynamicBytes, 2))
        return Fail("recapture after the first write returned stale bytes");
    now = Read();
    if (now.volatileCaptures != start.volatileCaptures)
        return Fail("a page that faulted once was treated as hot");

    // Second rewrite in the same epoch marks the page hot: it stays writable.
    fill(dynamicAddress, dynamicBytes, 3);
    const Counters hot = Read();
    if (hot.faults != now.faults + 1)
        return Fail("second write did not fault");
    if (!capture(dynamicAddress, dynamicBytes, 3))
        return Fail("hot-page capture returned stale bytes");
    fill(dynamicAddress, dynamicBytes, 4);
    if (!capture(dynamicAddress, dynamicBytes, 4))
        return Fail("second hot-page capture returned stale bytes");
    now = Read();
    if (now.faults != hot.faults || now.protects != hot.protects ||
        now.volatileCaptures != hot.volatileCaptures + 2)
        return Fail("a hot page was re-protected or kept faulting");

    // A large range over the hot page keeps the protected cache and re-tracks it.
    constexpr uint32_t largeAddress = 0xA0000000u;
    constexpr uint32_t largeBytes = 0x50000; // 320 KiB, above the volatile limit
    fill(largeAddress, 16, 4);
    fill(largeAddress + largeBytes - 16, 16, 4);
    {
        GuestMemorySnapshot snapshot;
        if (!snapshot.Capture(base, largeAddress, largeBytes))
            return Fail("large capture failed");
        const uint8_t* data = snapshot.Resolve(dynamicAddress, dynamicBytes);
        if (!data || data[0] != 4)
            return Fail("large capture missed the current hot-page bytes");
    }
    const Counters large = Read();
    if (large.volatileCaptures != now.volatileCaptures || large.protects <= now.protects)
        return Fail("a large range was copied without protection");
    // The small range's cached copy predates the hot writes, so the first
    // capture refreshes it through the protected path and the next one hits.
    if (!capture(dynamicAddress, dynamicBytes, 4) || !capture(dynamicAddress, dynamicBytes, 4))
        return Fail("re-tracked hot page returned stale bytes");
    now = Read();
    if (now.misses != large.misses + 1 || now.hits != large.hits + 1 ||
        now.volatileCaptures != large.volatileCaptures)
        return Fail("a re-tracked hot page was not refreshed once and then cached");

    // Once the hold expires, a page that still faults only rarely is protected.
    for (int i = 0; i < 300; ++i)
        GuestSnapshotAdvanceEpoch();
    fill(dynamicAddress, dynamicBytes, 5);
    const Counters expired = Read();
    if (!capture(dynamicAddress, dynamicBytes, 5))
        return Fail("capture after the hold returned stale bytes");
    now = Read();
    if (now.volatileCaptures != expired.volatileCaptures || now.protects <= expired.protects)
        return Fail("an isolated fault long after the hold made the page hot again");

    GuestSnapshotCacheShutdown();
    std::puts("PASS: hot-page snapshots stop fault storms without serving stale bytes.");
    return 0;
}
