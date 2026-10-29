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

    const std::string line = FormatPipelineSummary(previous, current);
    const char* expected[] = {
        "[pipeline] frames=4 fps=40.00 frame=25.00ms busiest=guest(90%)",
        "guest cpu=22.50ms(90%) cpWait=1.00ms/0.5x",
        "pm4 busy=9.00ms sink=5.00ms backpressure=1.00ms stall=0.38ms/0.5x vblankReleased=0.2x",
        "vulkan worker=12.00ms",
        "snap hit=90.0% miss=25.0 volatile=3.0 copied=1.00MiB faults=10.0 protects=2.0",
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

    std::puts("PASS: pipeline counters name the busiest stage and vblank-tick quantization.");
    return 0;
}
