#include "gpu_profile.h"

#include <algorithm>
#include <cstdio>

namespace mojorecomp::gpu {

const char* ToString(GpuPassCategory category)
{
    switch (category)
    {
        case GpuPassCategory::Other: return "other";
        case GpuPassCategory::Scene: return "scene";
        case GpuPassCategory::FrontPass: return "front";
        case GpuPassCategory::Resolve: return "resolve";
        case GpuPassCategory::Transfer: return "transfer-color";
        case GpuPassCategory::TransferDepth: return "transfer-depth";
        case GpuPassCategory::TransferStencil: return "transfer-stencil";
        case GpuPassCategory::Clear: return "clear";
        case GpuPassCategory::Copy: return "copy";
        case GpuPassCategory::Post: return "post";
        case GpuPassCategory::Fsr: return "fsr";
        case GpuPassCategory::Present: return "present";
        default: return "?";
    }
}

bool AccumulateGpuProfileFrame(const GpuProfileFrame& frame, uint32_t validBits,
                               double periodNs,
                               std::array<double, kGpuPassCategoryCount>& categoryNs,
                               double& frameNs)
{
    categoryNs.fill(0.0);
    frameNs = 0.0;
    if (frame.categories.empty() ||
        frame.timestamps.size() != frame.categories.size() + 1 || !validBits)
        return false;
    const uint64_t mask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);
    for (size_t i = 0; i < frame.categories.size(); ++i)
    {
        const size_t category = static_cast<size_t>(frame.categories[i]);
        if (category >= kGpuPassCategoryCount)
            return false;
        const uint64_t ticks = (frame.timestamps[i + 1] - frame.timestamps[i]) & mask;
        const double ns = double(ticks) * periodNs;
        categoryNs[category] += ns;
        frameNs += ns;
    }
    return true;
}

void GpuProfileAccumulator::AddFrame(const GpuProfileFrame& frame, uint32_t validBits,
                                     double periodNs)
{
    std::array<double, kGpuPassCategoryCount> categoryNs{};
    double frameNs = 0.0;
    ++frames_;
    if (!AccumulateGpuProfileFrame(frame, validBits, periodNs, categoryNs, frameNs))
    {
        ++malformed_;
        return;
    }
    for (size_t i = 0; i < kGpuPassCategoryCount; ++i)
        categoryNs_[i] += categoryNs[i];
    frameNs_ += frameNs;
    maxFrameNs_ = std::max(maxFrameNs_, frameNs);
    if (frame.overflow)
        ++overflowFrames_;
    marks_ += frame.categories.size();
    renderPasses_ += frame.counts.renderPasses;
    resolves_ += frame.counts.resolves;
    transfers_ += frame.counts.transfers;
    depthTransfers_ += frame.counts.depthTransfers;
    aliasRedirects_ += frame.counts.aliasRedirects;
    clears_ += frame.counts.clears;
    copies_ += frame.counts.copies;
    draws_ += frame.counts.draws;
    frontPassDraws_ += frame.counts.frontPassDraws;
}

std::string GpuProfileAccumulator::FormatAndReset()
{
    const uint32_t timed = frames_ > malformed_ ? frames_ - malformed_ : 0;
    const double perFrame = timed ? 1.0 / double(timed) : 0.0;
    auto ms = [&](double ns) { return ns * perFrame / 1.0e6; };
    auto avg = [&](uint64_t count) { return double(count) * perFrame; };

    char line[1024];
    int at = std::snprintf(line, sizeof(line), "[gpu-profile] frames=%u gpu=%.3fms max=%.3fms |",
                           frames_, ms(frameNs_), maxFrameNs_ / 1.0e6);
    for (size_t i = 0; i < kGpuPassCategoryCount && at > 0 && size_t(at) < sizeof(line); ++i)
        at += std::snprintf(line + at, sizeof(line) - size_t(at), " %s=%.3f",
                            ToString(static_cast<GpuPassCategory>(i)), ms(categoryNs_[i]));
    if (at > 0 && size_t(at) < sizeof(line))
        std::snprintf(line + at, sizeof(line) - size_t(at),
                      " | per frame: renderPasses=%.1f resolves=%.1f transfers=%.1f depthTransfers=%.1f "
                      "aliasRedirects=%.1f clears=%.1f "
                      "copies=%.1f draws=%.1f frontDraws=%.2f marks=%.1f overflow=%u malformed=%u",
                      avg(renderPasses_), avg(resolves_), avg(transfers_), avg(depthTransfers_),
                      avg(aliasRedirects_), avg(clears_),
                      avg(copies_), avg(draws_), avg(frontPassDraws_), avg(marks_),
                      overflowFrames_, malformed_);

    const uint32_t reportFrames = reportFrames_;
    *this = GpuProfileAccumulator(reportFrames);
    return line;
}

} // namespace mojorecomp::gpu
