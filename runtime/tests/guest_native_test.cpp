// Differential test of the native libc hooks (runtime/guest_native.cpp) against
// the translated original routines, on generated inputs. It links the
// translated image (ppc/), so it builds only next to a recompiled title; it
// reads no game data at run time.
//
//   MojoRecompGuestNativeTest           differential checks
//   MojoRecompGuestNativeTest --bench   ns per call, original vs native

#include "ppc_recomp_shared.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "../guest_native.h"
#include "../runtime_state.h"

namespace {

using mojorecomp::guest_native::SetEnabledForTesting;

constexpr uint32_t kWindow = 0x40000000u; // virtual range, no physical aliases
constexpr uint32_t kWindowBytes = 0x40000u;
constexpr uint32_t kStack = 0x70000000u;
constexpr uint32_t kStackBytes = 0x10000u;
constexpr uint32_t kGuard = 256;

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool Commit(uint8_t* base, uint32_t address, uint32_t bytes)
{
#if defined(_WIN32)
    return VirtualAlloc(base + address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base + address, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

uint8_t* ReserveGuestSpace()
{
#if defined(_WIN32)
    auto* base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* mapping = mmap(nullptr, 1ull << 32, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    auto* base = mapping == MAP_FAILED ? nullptr : static_cast<uint8_t*>(mapping);
#endif
    if (!base || !Commit(base, kWindow, kWindowBytes) || !Commit(base, kStack, kStackBytes))
        return nullptr;
    return base;
}

struct Lcg
{
    uint64_t state = 0x4D4F4A4F4E415449ull;
    uint32_t Next()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    }
    uint32_t Below(uint32_t bound) { return bound ? Next() % bound : 0; }
};

struct Routine
{
    const char* name;
    PPCFunc* entry;
    bool copy;
};

const Routine kRoutines[] = {
    {"memcpy 0x823CEC90", sub_823CEC90, true},
    {"forward copy 0x823CF0E8", sub_823CF0E8, true},
    {"memset 0x823CF440", sub_823CF440, false},
};

uint32_t CallGuest(PPCFunc* entry, uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5)
{
    PPCContext ctx{};
    ctx.r1.u64 = kStack + kStackBytes - 0x200;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    entry(ctx, base);
    return ctx.r3.u32;
}

uint32_t RandomSize(Lcg& rng)
{
    switch (rng.Below(10))
    {
        case 0: case 1: case 2: case 3: return rng.Below(33);
        case 4: case 5: case 6: return 33 + rng.Below(480);
        case 7: case 8: return 513 + rng.Below(7680);
        default: return 8193 + rng.Below(65536);
    }
}

struct Case
{
    uint32_t dst = 0;
    uint32_t src = 0; // copies; memset: the r4 value
    uint32_t bytes = 0;
    bool overlapping = false;
};

Case RandomCase(Lcg& rng, bool copy)
{
    Case c;
    c.bytes = RandomSize(rng);
    const uint32_t span = kWindowBytes / 2 - 2 * kGuard;
    if (!copy)
    {
        c.dst = kWindow + kGuard + rng.Below(span - c.bytes);
        c.src = rng.Next(); // only the low byte is the fill value
        return c;
    }
    if (rng.Below(10) < 3)
    {
        // Overlap at the distances the guest routines step by: 1, 4, 8, 128.
        static const int32_t kDeltas[] = {0, 1, -1, 3, -3, 4, -4, 7, 8, -8, 9, 64, 127,
                                          128, -128, 129, 200, -200};
        const int32_t delta = kDeltas[rng.Below(sizeof(kDeltas) / sizeof(kDeltas[0]))];
        c.bytes = std::min<uint32_t>(c.bytes, span - 512);
        c.src = kWindow + kGuard + 256 + rng.Below(span - c.bytes - 512);
        c.dst = uint32_t(int64_t(c.src) + delta);
        c.overlapping = true;
        return c;
    }
    c.dst = kWindow + kGuard + rng.Below(span - c.bytes);
    c.src = kWindow + kWindowBytes / 2 + kGuard + rng.Below(span - c.bytes);
    if (rng.Below(2))
        std::swap(c.dst, c.src);
    return c;
}

int RunDifferential(uint8_t* base)
{
    using mojorecomp::guest_native::CopyNonOverlapping;
    using mojorecomp::guest_native::Fill;
    Lcg rng;
    std::vector<uint8_t> initial(kWindowBytes);
    std::vector<uint8_t> original(kWindowBytes);
    for (auto& byte : initial)
        byte = static_cast<uint8_t>(rng.Next());
    uint8_t* window = base + kWindow;
    for (const Routine& routine : kRoutines)
    {
        uint32_t nativeCases = 0;
        uint32_t fallbackCases = 0;
        for (uint32_t i = 0; i < 3000; ++i)
        {
            const Case c = RandomCase(rng, routine.copy);

            std::memcpy(window, initial.data(), kWindowBytes);
            SetEnabledForTesting(false);
            const uint32_t originalResult = CallGuest(routine.entry, base, c.dst, c.src, c.bytes);
            std::memcpy(original.data(), window, kWindowBytes);

            std::memcpy(window, initial.data(), kWindowBytes);
            SetEnabledForTesting(true);
            const uint32_t nativeResult = CallGuest(routine.entry, base, c.dst, c.src, c.bytes);

            if (originalResult != c.dst || nativeResult != c.dst)
                return Fail("a routine did not return its destination in r3");
            if (std::memcmp(original.data(), window, kWindowBytes) != 0)
            {
                std::fprintf(stderr, "%s: dst=%08X src/value=%08X bytes=%u overlapping=%d\n",
                             routine.name, c.dst, c.src, c.bytes, int(c.overlapping));
                return Fail("native and translated routines left different memory");
            }

            // The translated original itself must match the C library where
            // the result is defined, or the comparison above proves little.
            std::vector<uint8_t> expected = initial;
            const uint32_t dstOffset = c.dst - kWindow;
            if (!routine.copy)
                std::memset(expected.data() + dstOffset, uint8_t(c.src), c.bytes);
            else if (!c.overlapping)
                std::memcpy(expected.data() + dstOffset, initial.data() + (c.src - kWindow),
                            c.bytes);
            if ((!routine.copy || !c.overlapping) &&
                std::memcmp(expected.data(), original.data(), kWindowBytes) != 0)
                return Fail("the translated routine does not match memcpy/memset");

            // Prove the native path took these cases: repeating the job on the
            // result is idempotent here and must be accepted.
            if (!c.overlapping &&
                !(routine.copy ? CopyNonOverlapping(base, c.dst, c.src, c.bytes)
                               : Fill(base, c.dst, uint8_t(c.src), c.bytes)))
                return Fail("the native routine declined a case it should take");

            (c.overlapping ? fallbackCases : nativeCases) += 1;
        }
        if (!nativeCases || (routine.copy && !fallbackCases))
            return Fail("a routine was not exercised on both paths");
        std::printf("PASS: %s matches the translated original (%u native, %u overlapping "
                    "cases left to the original)\n",
                    routine.name, nativeCases, fallbackCases);
    }

    // Cases the native copy must decline without touching memory. None of these
    // addresses is committed, so an access would crash the test.
    if (CopyNonOverlapping(base, 0xC0001010u, 0xA0001000u, 64) ||   // same physical bytes
        CopyNonOverlapping(base, 0xE0002000u, 0xC0002000u, 16) ||   // identical alias
        CopyNonOverlapping(base, 0x9FFFFFF8u, 0x50000000u, 16) ||   // into the views
        CopyNonOverlapping(base, 0xBFFFFFF8u, 0x50000000u, 16) ||   // across two views
        CopyNonOverlapping(base, 0xFFFFFFF0u, 0x50000000u, 32) ||   // wraps 4 GiB
        Fill(base, 0xFFFFFF00u, 0, 0x200))
        return Fail("a native routine accepted a range it must leave to the original");
    if (!CopyNonOverlapping(base, kWindow, kWindow + 64, 0))
        return Fail("an empty copy was not completed natively");
    std::puts("PASS: alias, view-boundary and wrapping ranges stay on the original code.");
    return 0;
}

void RunBenchmark(uint8_t* base)
{
    std::printf("%-26s %8s %6s %14s %14s %8s\n", "routine", "bytes", "align",
                "original ns", "native ns", "speedup");
    for (const Routine& routine : kRoutines)
    {
        for (uint32_t bytes : {8u, 32u, 128u, 512u, 4096u, 65536u})
        {
            for (uint32_t misalign : {0u, 3u})
            {
                const uint32_t dst = kWindow + kGuard + misalign;
                const uint32_t src = routine.copy ? kWindow + kWindowBytes / 2 + kGuard : 0x5Au;
                const uint32_t calls = std::max(2000u, (64u << 20) / std::max(bytes, 1u) / 4u);
                double ns[2] = {};
                for (int native = 0; native < 2; ++native)
                {
                    SetEnabledForTesting(native != 0);
                    CallGuest(routine.entry, base, dst, src, bytes);
                    const auto start = std::chrono::steady_clock::now();
                    for (uint32_t i = 0; i < calls; ++i)
                        CallGuest(routine.entry, base, dst, src, bytes);
                    ns[native] = std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - start).count() / calls;
                }
                std::printf("%-26s %8u %6u %14.1f %14.1f %7.1fx\n", routine.name, bytes,
                            misalign, ns[0], ns[1], ns[0] / std::max(ns[1], 0.001));
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    uint8_t* base = ReserveGuestSpace();
    if (!base)
        return Fail("could not reserve the guest address space");
    g_mojoGuestBase = base;
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0)
    {
        RunBenchmark(base);
        return 0;
    }
    return RunDifferential(base);
}
