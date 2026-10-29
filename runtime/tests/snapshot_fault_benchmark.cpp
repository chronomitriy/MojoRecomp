// Manual Windows microbenchmark (not a CTest): what one rewrite of a tracked
// page costs with GuestMemorySnapshot's write protection, compared with simply
// copying the page. Run it on the target PC; the numbers depend on the CPU,
// Windows build and security mitigations. Synthetic data only.
//
// Usage: MojoRecompSnapshotFaultBenchmark [pages] [rounds]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../gpu/guest_memory_snapshot.h"

int main(int argc, char** argv)
{
    using namespace mojorecomp::gpu;
    const uint32_t pages = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 64;
    const uint32_t rounds = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 200;

    constexpr size_t physicalBytes = 0x1000000;
    constexpr uintptr_t views[] = {0xA0000000ull, 0xC0000000ull, 0xE0000000ull};
    HANDLE process = GetCurrentProcess();
    auto* base = static_cast<uint8_t*>(VirtualAlloc2(
        process, nullptr, 1ull << 32, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
        PAGE_NOACCESS, nullptr, 0));
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE | SEC_COMMIT, 0, physicalBytes, nullptr);
    if (!base || !mapping)
        return 1;
    for (uintptr_t view : views)
        if (!VirtualFree(base + view, physicalBytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !MapViewOfFile3(mapping, process, base + view, 0, physicalBytes,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            return 1;
    CloseHandle(mapping);

    constexpr uint32_t first = 0xA0010000u;
    using Clock = std::chrono::steady_clock;
    double protectNs = 0.0;
    double faultNs = 0.0;
    for (uint32_t round = 0; round < rounds; ++round)
    {
        // PM4 side: capture every page, which re-protects the dirty ones.
        const auto captureStart = Clock::now();
        for (uint32_t page = 0; page < pages; ++page)
        {
            GuestMemorySnapshot snapshot;
            snapshot.Capture(base, first + page * 4096u, 256);
        }
        protectNs += std::chrono::duration<double, std::nano>(Clock::now() - captureStart).count();

        // Guest side: rewrite one dword per page, taking one fault each.
        const auto writeStart = Clock::now();
        for (uint32_t page = 0; page < pages; ++page)
            base[first + page * 4096u] = static_cast<uint8_t>(round);
        faultNs += std::chrono::duration<double, std::nano>(Clock::now() - writeStart).count();
    }

    std::vector<uint8_t> scratch(4096);
    const auto copyStart = Clock::now();
    for (uint32_t round = 0; round < rounds; ++round)
        for (uint32_t page = 0; page < pages; ++page)
            std::memcpy(scratch.data(), base + first + page * 4096u, 256);
    const double copyNs =
        std::chrono::duration<double, std::nano>(Clock::now() - copyStart).count();

    const double cycles = double(pages) * rounds;
    std::printf("pages=%u rounds=%u faults=%llu protects=%llu\n", pages, rounds,
                static_cast<unsigned long long>(GuestSnapshotWriteFaultCount()),
                static_cast<unsigned long long>(GuestSnapshotProtectCallCount()));
    std::printf("guest write fault: %.2f us/page   PM4 recapture+protect: %.2f us/page   "
                "plain 256 B copy: %.3f us/page\n",
                faultNs / cycles / 1000.0, protectNs / cycles / 1000.0,
                copyNs / cycles / 1000.0);
    GuestSnapshotCacheShutdown();
    return 0;
}
