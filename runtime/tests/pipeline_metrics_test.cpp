#include <cstdio>
#include <string>

#include "../host/pipeline_metrics.h"

namespace {

int Fail(const char* message, const std::string& line = {})
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    if (!line.empty())
        std::fprintf(stderr, "  %s\n", line.c_str());
    return 1;
}

bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    using namespace mojorecomp::host;

    struct BucketCase
    {
        uint32_t us;
        uint32_t bucket;
    };
    constexpr BucketCase kBuckets[] = {
        {0, 0}, {12499, 0}, {12500, 1}, {16667, 1}, {20832, 1}, {20833, 2},
        {25000, 2}, {29167, 3}, {33333, 3}, {37500, 4}, {41667, 4}, {45833, 5},
        {50000, 5}, {54167, 6}, {200000, 6},
    };
    for (const auto& c : kBuckets)
        if (TitleGateIntervalBucket(c.us) != c.bucket)
            return Fail("title gate interval is not bucketed by whole vblank ticks");

    PipelineCounters previous{};
    ReadPipelineCounters(previous);
    previous.wallNs = 1'000'000'000ull;
    previous.frames = 100;

    NoteTitleFrameGateRelease(16667);
    NoteTitleFrameGateRelease(16900);
    NoteTitleFrameGateRelease(16500);
    NoteTitleFrameGateRelease(33333);
    NoteGuestCpWait(2'000'000);
    NoteGuestCpWait(2'000'000);
    NotePm4Stall(1'000'000, true);
    NotePm4Stall(500'000, false);
    // Phase 3: a swap-handshake stall that a vblank happened to end counts as
    // vblank; the others keep the class of the memory they polled.
    NotePm4Stall(8'000'000, false, Pm4StallReason::SwapHandshake);
    NotePm4Stall(2'000'000, true, Pm4StallReason::SwapHandshake);
    NotePm4Stall(400'000, false, Pm4StallReason::GpuWrittenMemory);
    uint64_t byActivity[kPm4ActivityCount]{};
    byActivity[static_cast<uint32_t>(Pm4Activity::StallSwap)] = 3'000'000;
    byActivity[static_cast<uint32_t>(Pm4Activity::Parsing)] = 1'000'000;
    NoteGuestCpWaitBreakdown(byActivity, 4000);
    NotePm4FrameLagWait(2'000'000);
    NotePm4FrameLagWait(2'000'000);
    NotePm4PumpTimeout(1'000'000);
    NotePm4PumpTimeout(15'000'000);
    NoteRingBacklog(1024, 16384);
    NoteRingBacklog(0, 16384);
    for (int i = 0; i < 6; ++i)
        NoteGpuSignal(true);
    NoteGpuSignal(false);
    NoteOutstandingPresents(1);
    NoteOutstandingPresents(1);
    NoteOutstandingPresents(0);
    NoteOutstandingPresents(1);
    if (SetPm4Activity(Pm4Activity::Backpressure) != Pm4Activity::Idle ||
        CurrentPm4Activity() != Pm4Activity::Backpressure ||
        SetPm4Activity(Pm4Activity::Idle) != Pm4Activity::Backpressure)
        return Fail("PM4 activity is not a restorable single state");
    const uint64_t vblank = GuestVblankSequence();
    NoteGuestVblank();
    if (GuestVblankSequence() != vblank + 1)
        return Fail("guest vblank sequence did not advance");

    PipelineCounters current{};
    ReadPipelineCounters(current);
    current.wallNs = previous.wallNs + 100'000'000ull; // 4 frames in 100 ms
    current.frames = previous.frames + 4;
    current.guestMainCpuNs = 90'000'000ull;            // 90% of the window
    previous.guestMainCpuNs = 0;
    current.pm4ExecuteNs = 40'000'000ull;
    current.pm4DrawSinkNs = 20'000'000ull;
    current.executorBackpressureNs = 4'000'000ull;
    current.executorActiveNs = 48'000'000ull;
    current.snapshotCacheHits = 900;
    current.snapshotCacheMisses = 100;
    current.snapshotCopiedBytes = 4ull * 1024ull * 1024ull;
    current.snapshotWriteFaults = 40;
    current.snapshotProtectCalls = 8;
    current.snapshotVolatileCaptures = 12;
    current.snapshotCommandCaptures = 20;

    const std::string line = FormatPipelineSummary(previous, current);
    // Totals per 4 frames: stall 1.0+0.5+8.0+2.0+0.4 = 11.9 ms in 5 episodes,
    // two of them released by a vblank; 4 ms of frame-lag waits come off pm4 busy.
    const char* expected[] = {
        "[pipeline] frames=4 fps=40.00 frame=25.00ms busiest=guest(90%)",
        "guest cpu=22.50ms(90%) cpWait=1.00ms/0.5x",
        "pm4 busy=8.00ms sink=5.00ms backpressure=1.00ms stall=2.98ms/1.2x vblankReleased=0.5x",
        "vulkan worker=12.00ms",
        "snap hit=90.0% miss=25.0 volatile=3.0 cmd=5.0 copied=1.00MiB faults=10.0 protects=2.0",
        "| waits cpBy[idle=0.00 parse=0.25 swap=0.75 gpu=0.00 other=0.00 bp=0.00 lag=0.00] "
        "polls=1000 stallBy[swap=2.00 gpu=0.10 vblank=0.75 other=0.12] lag=1.00ms/0.5x "
        "idleWait=8.000ms/0.5x ring avg=",
        " max=4.0KiB of 64KiB signals cpTime=1.5 deferred=0.2 inflight=0.75 | ",
        "gate releases=4 ticks: <1=0% 1=75% 1.5=0% 2=25% 2.5=0% 3=0% >3=0%",
    };
    for (const char* needle : expected)
        if (!Contains(line, needle))
            return Fail(needle, line);

    // Without a sampled guest thread the busiest stage is chosen among the
    // host stages, and the guest column says so instead of printing zero.
    current.guestMainCpuNs = 0;
    const std::string hostOnly = FormatPipelineSummary(previous, current);
    if (!Contains(hostOnly, "busiest=vulkan(48%)") || !Contains(hostOnly, "guest cpu=n/a"))
        return Fail("host-only summary picked the wrong stage", hostOnly);

    // The ring maximum restarts with every sample; totals do not.
    PipelineCounters next{};
    ReadPipelineCounters(next);
    if (next.ringBacklogMaxDwords != 0 || next.ringDwords != 16384 ||
        next.cpTimeSignals != current.cpTimeSignals)
        return Fail("ring maximum is not per window or a total was reset");

    std::puts("PASS: pipeline counters name the busiest stage and vblank-tick quantization.");
    return 0;
}
