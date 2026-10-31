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

// Why a PM4 WAIT_REG_MEM stall episode ended. The polled target is classified
// when the episode starts; an episode released by a guest vblank counts as
// Vblank whatever it polled.
enum class Pm4StallReason : uint8_t
{
    // A scratch-register write-back slot: D3D's swap and CPU-callback
    // handshake (SCRATCH_REG0/1, docs/HFR-PHASE2.md section 4.2).
    SwapHandshake,
    // Other memory an earlier GPU store wrote: fences from EVENT_WRITE_SHD,
    // MEM_WRITE and similar packets.
    GpuWrittenMemory,
    Vblank,
    // Registers and memory that only the CPU writes.
    Other,
};
inline constexpr uint32_t kPm4StallReasonCount = 4;
inline constexpr const char* kPm4StallReasonNames[kPm4StallReasonCount] = {
    "swap", "gpu", "vblank", "other"};

// What the PM4 thread is doing. The guest samples it while it waits for the CP
// read pointer, so its cpWait can be attributed to whatever holds the PM4 thread.
enum class Pm4Activity : uint8_t
{
    Idle,         // waiting for the guest to publish more commands
    Parsing,      // executing commands, including draw snapshots and the ISR
    StallSwap,    // WAIT_REG_MEM on a scratch write-back slot
    StallGpu,     // WAIT_REG_MEM on other GPU-written memory
    StallOther,   // any other WAIT_REG_MEM
    Backpressure, // the Vulkan executor queue is full
    FrameLag,     // holding a swap until the Vulkan thread finishes an earlier one
};
inline constexpr uint32_t kPm4ActivityCount = 7;
inline constexpr const char* kPm4ActivityNames[kPm4ActivityCount] = {
    "idle", "parse", "swap", "gpu", "other", "bp", "lag"};

struct PipelineCounters
{
    uint64_t wallNs = 0;
    uint64_t frames = 0;

    // Guest main thread: kernel+user CPU time and time spent spinning in the
    // title's CP read-pointer poll while waiting for the PM4 thread.
    uint64_t guestMainCpuNs = 0;
    uint64_t guestCpWaitNs = 0;
    uint64_t guestCpWaitEpisodes = 0;
    // The same wait split by what the PM4 thread was doing at each poll, and
    // the number of polls.
    uint64_t guestCpWaitByPm4Ns[kPm4ActivityCount]{};
    uint64_t guestCpPolls = 0;

    // PM4 thread: time inside Pm4_Execute (includes the draw sink and any
    // executor backpressure), the draw-sink share of it, and WAIT_REG_MEM
    // stalls. A stall counts as vblank-released when a guest vblank was
    // delivered between its last failing poll and the poll that passed.
    uint64_t pm4ExecuteNs = 0;
    uint64_t pm4DrawSinkNs = 0;
    uint64_t pm4StallNs = 0;
    uint64_t pm4StallEpisodes = 0;
    uint64_t pm4StallVblankReleased = 0;
    uint64_t pm4StallReasonNs[kPm4StallReasonCount]{};
    // Swaps held until the Vulkan thread finished an earlier present
    // (MOJORECOMP_PM4_FRAMES_AHEAD), excluded from pm4 busy like backpressure.
    uint64_t pm4FrameLagNs = 0;
    uint64_t pm4FrameLagWaits = 0;
    // PM4 pump waits that ran to their timeout and their total duration. The
    // average shows the host timer granularity the pump actually gets.
    uint64_t pm4PumpTimeouts = 0;
    uint64_t pm4PumpTimeoutNs = 0;

    // Ring dwords the guest has published but the PM4 thread has not consumed:
    // time integral, largest value since the previous sample (reset by
    // ReadPipelineCounters), and the ring size.
    uint64_t ringBacklogDwordNs = 0;
    uint64_t ringBacklogMaxDwords = 0;
    uint64_t ringDwords = 0;

    // GPU stores and CP interrupts that took effect when the PM4 thread reached
    // them (CP time) or only when the Vulkan thread did (deferred).
    uint64_t cpTimeSignals = 0;
    uint64_t deferredSignals = 0;
    // Presents enqueued but not yet completed by the Vulkan thread, summed over
    // swaps: per frame it is how many frames the Vulkan thread lags behind.
    uint64_t outstandingPresents = 0;

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
    uint64_t snapshotCommandCaptures = 0;

    uint64_t titleGateReleases = 0;
    uint64_t titleGateIntervals[kTitleGateIntervalBuckets]{};
};

// Recording side. All functions are thread-safe and lock-free.
void NoteGuestCpWait(uint64_t ns) noexcept;
// One finished cpWait episode: time per PM4 activity and the number of polls.
void NoteGuestCpWaitBreakdown(const uint64_t (&nsByActivity)[kPm4ActivityCount],
                              uint64_t polls) noexcept;
void NotePm4Stall(uint64_t ns, bool releasedByVblank) noexcept;
void NotePm4Stall(uint64_t ns, bool releasedByVblank, Pm4StallReason target) noexcept;
void NotePm4FrameLagWait(uint64_t ns) noexcept;
void NotePm4PumpTimeout(uint64_t ns) noexcept;
void NoteRingBacklog(uint32_t backlogDwords, uint32_t ringDwords) noexcept;
void NoteGpuSignal(bool cpTime) noexcept;
void NoteOutstandingPresents(uint32_t presents) noexcept;

// Written by the PM4 thread only; returns the previous activity so a scoped
// wait can restore it.
Pm4Activity SetPm4Activity(Pm4Activity activity) noexcept;
Pm4Activity CurrentPm4Activity() noexcept;
void NoteGuestVblank() noexcept;
uint64_t GuestVblankSequence() noexcept;
void NoteTitleFrameGateRelease(uint32_t intervalUs) noexcept;

// Registers the host thread that runs the guest entry point so its CPU time can
// be sampled from another thread. Returns 0 where the platform cannot sample.
void SetGuestMainThread(uint32_t hostThreadId) noexcept;
uint32_t GuestMainThreadId() noexcept;
uint64_t GuestMainThreadCpuNs() noexcept;

// Fills the fields owned by this module (guest wait, PM4 waits, ring, signals,
// gate). The ring backlog maximum restarts after every call.
void ReadPipelineCounters(PipelineCounters& out) noexcept;

// One-line per-frame summary of the difference between two samples.
std::string FormatPipelineSummary(const PipelineCounters& previous,
                                  const PipelineCounters& current);

} // namespace mojorecomp::host
