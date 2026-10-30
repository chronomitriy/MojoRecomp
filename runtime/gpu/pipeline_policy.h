#pragma once

// Phase 3 pipeline switches (docs/HFR-PHASE3.md). Each one is read once from
// the environment, and each applies only while the title runs an HFR policy:
// the 30 FPS mode and native-cadence (Bink) video keep the phase 2 behaviour.

#include <cstdint>
#include <cstdlib>

#include "../host/frame_rate_policy.h"
#include "pm4.h"

namespace mojorecomp::gpu {

inline bool PipelineEnvSwitch(const char* name, bool defaultValue) noexcept
{
    const char* value = std::getenv(name);
    if (!value || !*value)
        return defaultValue;
    return value[0] != '0';
}

// GPU memory writes and command-processor interrupts take effect when the PM4
// thread reaches them instead of when the Vulkan thread does.
// MOJORECOMP_CP_TIME_SIGNALS=0 restores the phase 2 ordering.
inline bool CpTimeSignalsRequested() noexcept
{
    static const bool requested = PipelineEnvSwitch("MOJORECOMP_CP_TIME_SIGNALS", true);
    return requested;
}

// The PM4 pump is woken by the guest's CP poll, by deferred stores and by the
// guest vblank, and its idle poll uses a high-resolution timer.
// MOJORECOMP_PM4_PROMPT_WAKE=0 restores the condition-variable poll.
inline bool PromptPm4WakeRequested() noexcept
{
    static const bool requested = PipelineEnvSwitch("MOJORECOMP_PM4_PROMPT_WAKE", true);
    return requested;
}

// With CP-time signals, how many presents the Vulkan thread may still owe when
// the PM4 thread enqueues the next one (MOJORECOMP_PM4_FRAMES_AHEAD, 1 to 3).
inline uint32_t Pm4FramesAhead() noexcept
{
    static const uint32_t frames = [] {
        const char* value = std::getenv("MOJORECOMP_PM4_FRAMES_AHEAD");
        if (!value || !*value)
            return 1u;
        const unsigned long parsed = std::strtoul(value, nullptr, 10);
        return parsed >= 1 && parsed <= 3 ? static_cast<uint32_t>(parsed) : 1u;
    }();
    return frames;
}

// The process runs an HFR mode (fixed at startup; Bink video does not change it).
inline bool HfrModeRequested() noexcept
{
    return host::CommandProcessorTimeSignals(host::EffectiveActiveFrameRatePolicy());
}

// The title policy right now: an HFR mode, and no Bink video on screen.
inline bool HfrPipelinePolicyActive() noexcept
{
    return host::CommandProcessorTimeSignals(host::TitlePolicyForNativeCadenceContent(
        host::EffectiveActiveFrameRatePolicy(), Pm4_BinkVideoCadenceActive()));
}

} // namespace mojorecomp::gpu
