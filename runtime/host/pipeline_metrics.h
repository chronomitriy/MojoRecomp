#pragma once

// Low-overhead counters for the guest -> PM4 -> Vulkan pipeline.
//
// Every frame passes through three host threads: the guest main thread writes
// PM4 into the ring, the PM4 thread parses it and snapshots the guest memory
// each draw reads, and the Vulkan executor thread records and presents. The
// frame rate is bounded by the busiest stage and by the waits that couple them.
// These counters are monotonically increasing totals; FormatPipelineSummary
// turns two samples into one per-frame line so a single runtime.log can show
// which stage limits a scene.

#include <cstdint>
#include <string>

namespace mojorecomp::host {

// Dispatch intervals of the title's frame gate, in 60 Hz guest-vblank ticks.
// Buckets are centred on whole ticks so frame times quantized to vblank show up
// as mass in "1", "2" and "3" with little in "1.5"/"2.5".
inline constexpr uint32_t kTitleGateIntervalBuckets = 7;
inline constexpr const char* kTitleGateIntervalBucketNames[kTitleGateIntervalBuckets] = {
    "<1", "1", "1.5", "2", "2.5", "3", ">3"};

constexpr uint32_t TitleGateIntervalBucket(uint32_t intervalUs) noexcept
{
    // Quarter-tick boundaries: 0.75, 1.25, 1.75, 2.25, 2.75, 3.25 ticks of
    // 16666.67 us, rounded to the nearest microsecond.
    if (intervalUs < 12500u) return 0;
    if (intervalUs < 20833u) return 1;
    if (intervalUs < 29167u) return 2;
    if (intervalUs < 37500u) return 3;
    if (intervalUs < 45833u) return 4;
    if (intervalUs < 54167u) return 5;
    return 6;
}

struct PipelineCounters
{
    uint64_t wallNs = 0;
    uint64_t frames = 0;

    // Guest main thread: kernel+user CPU time and time spent spinning in the
    // title's CP read-pointer poll while waiting for the PM4 thread.
    uint64_t guestMainCpuNs = 0;
    uint64_t guestCpWaitNs = 0;
    uint64_t guestCpWaitEpisodes = 0;

    // PM4 thread: time inside Pm4_Execute (includes the draw sink and any
    // executor backpressure), the draw-sink share of it, and WAIT_REG_MEM
    // stalls. A stall counts as vblank-released when a guest vblank was
    // delivered between its last failing poll and the poll that passed.
    uint64_t pm4ExecuteNs = 0;
    uint64_t pm4DrawSinkNs = 0;
    uint64_t pm4StallNs = 0;
    uint64_t pm4StallEpisodes = 0;
    uint64_t pm4StallVblankReleased = 0;

    // Vulkan executor thread.
    uint64_t executorActiveNs = 0;
    uint64_t executorBackpressureNs = 0;

    // Per-draw guest-memory snapshots built on the PM4 thread.
    uint64_t snapshotCacheHits = 0;
    uint64_t snapshotCacheMisses = 0;
    uint64_t snapshotCopiedBytes = 0;
    uint64_t snapshotWriteFaults = 0;
    uint64_t snapshotProtectCalls = 0;
    uint64_t snapshotVolatileCaptures = 0;

    uint64_t titleGateReleases = 0;
    uint64_t titleGateIntervals[kTitleGateIntervalBuckets]{};
};

// Recording side. All functions are thread-safe and lock-free.
void NoteGuestCpWait(uint64_t ns) noexcept;
void NotePm4Stall(uint64_t ns, bool releasedByVblank) noexcept;
void NoteGuestVblank() noexcept;
uint64_t GuestVblankSequence() noexcept;
void NoteTitleFrameGateRelease(uint32_t intervalUs) noexcept;

// Registers the host thread that runs the guest entry point so its CPU time can
// be sampled from another thread. Returns 0 where the platform cannot sample.
void SetGuestMainThread(uint32_t hostThreadId) noexcept;
uint64_t GuestMainThreadCpuNs() noexcept;

// Fills the fields owned by this module (guest wait, PM4 stall, gate).
void ReadPipelineCounters(PipelineCounters& out) noexcept;

// One-line per-frame summary of the difference between two samples.
std::string FormatPipelineSummary(const PipelineCounters& previous,
                                  const PipelineCounters& current);

} // namespace mojorecomp::host
