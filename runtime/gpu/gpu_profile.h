#pragma once

// GPU pass timing (MOJORECOMP_GPU_PROFILE=1). The presenter writes a Vulkan
// timestamp whenever the kind of work it records changes; the time between two
// timestamps is charged to the category that was active after the first one.
// This file holds the portable part: categories, per-frame accumulation and the
// [gpu-profile] log line.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mojorecomp::gpu {

enum class GpuPassCategory : uint8_t
{
    // Barriers and other work outside the categories below.
    Other,
    // Guest draws into the EDRAM render targets.
    Scene,
    // The title's full-screen front pass (motion blur, by pixel-shader hash).
    FrontPass,
    // EDRAM resolves into textures.
    Resolve,
    // EDRAM ownership transfers between surface formats: into color targets,
    // the depth pass of a transfer into a depth target, and that transfer's
    // stencil clear and eight bit-plane passes (used when the device lacks
    // VK_EXT_shader_stencil_export).
    Transfer,
    TransferDepth,
    TransferStencil,
    Clear,
    // Texture uploads and their mip generation.
    Copy,
    // Presentation compose blit and FXAA.
    Post,
    // FSR 1 EASU and RCAS.
    Fsr,
    // Final presentation blit and the debug overlay.
    Present,
    Count,
};

inline constexpr size_t kGpuPassCategoryCount = static_cast<size_t>(GpuPassCategory::Count);

const char* ToString(GpuPassCategory category);

struct GpuProfileCounts
{
    uint32_t renderPasses = 0;
    uint32_t resolves = 0;
    uint32_t transfers = 0;
    // Transfers into a depth target (a subset of transfers).
    uint32_t depthTransfers = 0;
    // Draws through an MSAA alias drawn into the surface owning its tiles.
    uint32_t aliasRedirects = 0;
    uint32_t clears = 0;
    uint32_t copies = 0;
    uint32_t draws = 0;
    uint32_t frontPassDraws = 0;
};

// One recorded frame: the category of each timestamp in order, and the raw
// timestamps (one more than categories: the last one closes the frame).
struct GpuProfileFrame
{
    std::vector<GpuPassCategory> categories;
    std::vector<uint64_t> timestamps;
    GpuProfileCounts counts;
    // Marks were dropped because the frame ran out of queries.
    bool overflow = false;
};

// Sums a frame's segments per category, in nanoseconds. `validBits` is the
// queue's timestampValidBits (wrap-around), `periodNs` its timestampPeriod.
// Returns false for a malformed frame.
bool AccumulateGpuProfileFrame(const GpuProfileFrame& frame, uint32_t validBits,
                               double periodNs,
                               std::array<double, kGpuPassCategoryCount>& categoryNs,
                               double& frameNs);

class GpuProfileAccumulator
{
public:
    explicit GpuProfileAccumulator(uint32_t reportFrames = 120) : reportFrames_(reportFrames) {}

    void AddFrame(const GpuProfileFrame& frame, uint32_t validBits, double periodNs);
    bool ReportDue() const { return frames_ >= reportFrames_; }
    uint32_t Frames() const { return frames_; }
    // "[gpu-profile] ..." with per-frame averages in milliseconds; resets.
    std::string FormatAndReset();

private:
    uint32_t reportFrames_ = 120;
    uint32_t frames_ = 0;
    uint32_t malformed_ = 0;
    uint32_t overflowFrames_ = 0;
    double frameNs_ = 0.0;
    double maxFrameNs_ = 0.0;
    std::array<double, kGpuPassCategoryCount> categoryNs_{};
    uint64_t marks_ = 0;
    uint64_t renderPasses_ = 0;
    uint64_t resolves_ = 0;
    uint64_t transfers_ = 0;
    uint64_t depthTransfers_ = 0;
    uint64_t aliasRedirects_ = 0;
    uint64_t clears_ = 0;
    uint64_t copies_ = 0;
    uint64_t draws_ = 0;
    uint64_t frontPassDraws_ = 0;
};

} // namespace mojorecomp::gpu
