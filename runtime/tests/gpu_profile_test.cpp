#include <cmath>
#include <cstdio>
#include <string>

#include "../gpu/gpu_profile.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool Near(double value, double expected)
{
    return std::fabs(value - expected) < 1.0e-9;
}

size_t Index(mojorecomp::gpu::GpuPassCategory category)
{
    return static_cast<size_t>(category);
}

bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    using mojorecomp::gpu::AccumulateGpuProfileFrame;
    using mojorecomp::gpu::GpuPassCategory;
    using mojorecomp::gpu::GpuProfileAccumulator;
    using mojorecomp::gpu::GpuProfileFrame;
    using mojorecomp::gpu::kGpuPassCategoryCount;

    // Each segment is charged to the category that starts it; a category can
    // appear more than once in a frame.
    GpuProfileFrame frame;
    frame.categories = {GpuPassCategory::Other, GpuPassCategory::Scene,
                        GpuPassCategory::Resolve, GpuPassCategory::Scene,
                        GpuPassCategory::Present};
    frame.timestamps = {100, 150, 400, 420, 520, 530};
    frame.counts.draws = 900;
    frame.counts.resolves = 3;
    std::array<double, kGpuPassCategoryCount> categoryNs{};
    double frameNs = 0.0;
    if (!AccumulateGpuProfileFrame(frame, 64, 2.0, categoryNs, frameNs))
        return Fail("a well-formed frame must accumulate");
    if (!Near(categoryNs[Index(GpuPassCategory::Other)], 100.0) ||
        !Near(categoryNs[Index(GpuPassCategory::Scene)], 700.0) ||
        !Near(categoryNs[Index(GpuPassCategory::Resolve)], 40.0) ||
        !Near(categoryNs[Index(GpuPassCategory::Present)], 20.0) || !Near(frameNs, 860.0))
        return Fail("segments must be charged to their opening category times the period");

    // timestampValidBits below 64: the counter wraps inside the frame.
    GpuProfileFrame wrapped;
    wrapped.categories = {GpuPassCategory::Scene};
    wrapped.timestamps = {0xFFFFFFF0ull, 0x10ull};
    if (!AccumulateGpuProfileFrame(wrapped, 32, 1.0, categoryNs, frameNs) ||
        !Near(categoryNs[Index(GpuPassCategory::Scene)], 32.0) || !Near(frameNs, 32.0))
        return Fail("a 32-bit timestamp wrap must give the masked difference");

    GpuProfileFrame mismatched;
    mismatched.categories = {GpuPassCategory::Scene};
    mismatched.timestamps = {1, 2, 3};
    if (AccumulateGpuProfileFrame(mismatched, 64, 1.0, categoryNs, frameNs))
        return Fail("timestamps must be exactly one more than categories");
    GpuProfileFrame badCategory;
    badCategory.categories = {GpuPassCategory::Count};
    badCategory.timestamps = {1, 2};
    if (AccumulateGpuProfileFrame(badCategory, 64, 1.0, categoryNs, frameNs))
        return Fail("an out-of-range category must be rejected");
    if (AccumulateGpuProfileFrame(frame, 0, 1.0, categoryNs, frameNs))
        return Fail("a queue without valid timestamp bits must be rejected");

    // Two good frames (one flagged as overflowing) and one malformed frame.
    GpuProfileAccumulator accumulator(3);
    accumulator.AddFrame(frame, 64, 1.0e6);
    if (accumulator.ReportDue())
        return Fail("the report must wait for the configured frame count");
    GpuProfileFrame overflowing = frame;
    overflowing.overflow = true;
    accumulator.AddFrame(overflowing, 64, 1.0e6);
    accumulator.AddFrame(mismatched, 64, 1.0e6);
    if (!accumulator.ReportDue() || accumulator.Frames() != 3)
        return Fail("three added frames must make the report due");
    const std::string line = accumulator.FormatAndReset();
    // Periods of 1 ms per tick: scene = 350 ticks = 350 ms per good frame.
    if (!Contains(line, "[gpu-profile] frames=3 gpu=430.000ms max=430.000ms |") ||
        !Contains(line, " scene=350.000") || !Contains(line, " resolve=20.000") ||
        !Contains(line, " present=10.000") || !Contains(line, " fsr=0.000") ||
        !Contains(line, "draws=900.0") || !Contains(line, "resolves=3.0") ||
        !Contains(line, "marks=5.0") || !Contains(line, "overflow=1 malformed=1"))
    {
        std::fprintf(stderr, "%s\n", line.c_str());
        return Fail("the report must average over the well-formed frames only");
    }
    if (accumulator.Frames() != 0 || accumulator.ReportDue())
        return Fail("FormatAndReset must start a new window");

    // EDRAM transfers are split: into color, the depth pass into depth, and
    // the stencil clear plus bit-plane passes of that depth transfer.
    GpuProfileFrame transfers;
    transfers.categories = {GpuPassCategory::Transfer, GpuPassCategory::TransferDepth,
                            GpuPassCategory::TransferStencil, GpuPassCategory::Scene};
    transfers.timestamps = {0, 5, 7, 25, 26};
    transfers.counts.transfers = 2;
    transfers.counts.depthTransfers = 1;
    GpuProfileAccumulator split(1);
    split.AddFrame(transfers, 64, 1.0e6);
    const std::string splitLine = split.FormatAndReset();
    if (!Contains(splitLine, " transfer-color=5.000") ||
        !Contains(splitLine, " transfer-depth=2.000") ||
        !Contains(splitLine, " transfer-stencil=18.000") ||
        !Contains(splitLine, "transfers=2.0 depthTransfers=1.0"))
    {
        std::fprintf(stderr, "%s\n", splitLine.c_str());
        return Fail("transfer segments must be reported per kind");
    }

    std::puts("PASS: GPU profile segments, timestamp wrap, malformed frames and report averages.");
    return 0;
}
