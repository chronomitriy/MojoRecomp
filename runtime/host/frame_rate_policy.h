#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace mojorecomp::host {

enum class FrameRateMode : uint8_t
{
    Compatibility30,
    HighFrameRate60,
    // Opt-in title cadence above 60 Hz (120/144/165/uncapped). It is held at
    // exactly the HighFrameRate60 contract until the title's own 1/60 s
    // scheduler floor has been lifted; see EffectiveTitleFrameRatePolicy().
    HighFrameRateUnlocked,
};

struct FrameRatePolicy
{
    FrameRateMode mode = FrameRateMode::Compatibility30;
    uint32_t simulationHz = 30;
    uint32_t presentationHz = 30;
    uint32_t displayHz = 60;
    uint32_t guestVblankHz = 60;
    bool requiresTitlePatches = false;
    // HighFrameRateUnlocked only: presentationHz is kUnlockedMaxHz, a safety
    // ceiling rather than a target the player chose.
    bool uncapped = false;
};

// Requested rates above 60 Hz. "uncapped" still runs the title gate at
// kUnlockedMaxHz so loading screens and menus that render in well under a
// millisecond can never hand the title's update a near-zero step.
inline constexpr uint32_t kUnlockedMinHz = 61;
inline constexpr uint32_t kUnlockedMaxHz = 300;
inline constexpr uint32_t kUnlockedDefaultHz = 120;

// The title loop dispatches its frame/update virtual only when the measured
// elapsed time in r28 is strictly greater than this value (see the
// MojoRecompHfrFrameGate hook in config/CrashOfTheTitans.toml).
inline constexpr uint32_t kTitleFrameGateThresholdUs = 33333;

// The title's mode-1 scheduler refuses steps shorter than 1/60 s. Rates above
// 60 Hz are impossible until that floor is lowered by MojoRecompHfrSchedulerFloor.
inline constexpr double kTitleSchedulerFloorSeconds = 1.0 / 60.0;

struct TitleFrameGatePhase
{
    uint32_t carryUs = 0;
};

constexpr FrameRatePolicy UnlockedPolicy(uint32_t hz, bool uncapped = false) noexcept
{
    return {FrameRateMode::HighFrameRateUnlocked, hz, hz, 60, 60, true, uncapped};
}

constexpr FrameRatePolicy PolicyFor(FrameRateMode mode) noexcept
{
    switch (mode)
    {
        case FrameRateMode::HighFrameRateUnlocked:
            return UnlockedPolicy(kUnlockedDefaultHz);
        case FrameRateMode::HighFrameRate60:
            return {FrameRateMode::HighFrameRate60, 60, 60, 60, 60, true};
        case FrameRateMode::Compatibility30:
        default:
            return {FrameRateMode::Compatibility30, 30, 30, 60, 60, false};
    }
}

// Returns the requested unlocked rate, or 0 when the setting is not an unlocked
// value. Accepted: "uncapped" and plain decimal integers 61..300 (the launcher
// offers 120, 144 and 165; other integers are for direct runtime launches, for
// example 162 to stay inside a 165 Hz VRR window).
constexpr uint32_t UnlockedRateHzForSetting(std::string_view value) noexcept
{
    if (value == "uncapped")
        return kUnlockedMaxHz;
    if (value.empty() || value.size() > 3 || value[0] == '0')
        return 0;
    uint32_t hz = 0;
    for (const char c : value)
    {
        if (c < '0' || c > '9')
            return 0;
        hz = hz * 10u + uint32_t(c - '0');
    }
    return hz >= kUnlockedMinHz && hz <= kUnlockedMaxHz ? hz : 0;
}

constexpr FrameRateMode FrameRateModeForSetting(std::string_view value) noexcept
{
    if (UnlockedRateHzForSetting(value))
        return FrameRateMode::HighFrameRateUnlocked;
    return value == "60" ? FrameRateMode::HighFrameRate60
                         : FrameRateMode::Compatibility30;
}

// "30", "60", unknown and empty values resolve exactly as PolicyFor(
// FrameRateModeForSetting(value)) always did; only unlocked values add a rate.
constexpr FrameRatePolicy PolicyForSetting(std::string_view value) noexcept
{
    if (const uint32_t hz = UnlockedRateHzForSetting(value))
        return UnlockedPolicy(hz, value == "uncapped");
    return PolicyFor(FrameRateModeForSetting(value));
}

constexpr const char* FrameRateModeName(FrameRateMode mode) noexcept
{
    switch (mode)
    {
        case FrameRateMode::HighFrameRate60: return "hfr60";
        case FrameRateMode::HighFrameRateUnlocked: return "hfr-unlocked";
        case FrameRateMode::Compatibility30:
        default: return "compat30";
    }
}

// HFR remains opt-in while the title-side patch is validated.  This is a
// process-wide policy chosen once at startup so every subsystem (title hook,
// vblank, diagnostics and presentation) observes the same mode.  Unset/unknown
// values retain the proven 30 FPS compatibility path.
inline FrameRatePolicy ActiveFrameRatePolicy() noexcept
{
    static const FrameRatePolicy policy = [] {
        const char* setting = std::getenv("MOJORECOMP_FRAME_RATE");
        return PolicyForSetting(setting ? setting : "");
    }();
    return policy;
}

// The unlocked mode only means something once the title's own 1/60 s scheduler
// floor has been lowered. Releasing the outer title gate faster than that floor
// would, depending on how the floor is written, stall the update, keep it at
// 60 Hz or run the game too fast. Until MojoRecompHfrSchedulerFloor has proven
// that it rewrote the floor, an unlocked request is therefore *exactly* the
// HighFrameRate60 policy, so every hook keeps the validated 60 FPS behavior.
constexpr FrameRatePolicy EffectiveTitleFrameRatePolicy(FrameRatePolicy requested,
                                                        bool schedulerFloorLifted) noexcept
{
    if (requested.mode == FrameRateMode::HighFrameRateUnlocked && !schedulerFloorLifted)
        return PolicyFor(FrameRateMode::HighFrameRate60);
    return requested;
}

// Process-wide latch set once by the scheduler-floor hook.
inline std::atomic<bool>& TitleSchedulerFloorLifted() noexcept
{
    static std::atomic<bool> lifted{false};
    return lifted;
}

inline FrameRatePolicy EffectiveActiveFrameRatePolicy() noexcept
{
    return EffectiveTitleFrameRatePolicy(
        ActiveFrameRatePolicy(), TitleSchedulerFloorLifted().load(std::memory_order_acquire));
}

// Only a register that really holds the title's 1/60 s floor is rewritten. A
// hook placed on the wrong instruction therefore neither changes guest state
// nor unlocks the outer gate.
constexpr bool IsTitleSchedulerFloorValue(double seconds) noexcept
{
    return seconds > kTitleSchedulerFloorSeconds - 1.0e-6 &&
           seconds < kTitleSchedulerFloorSeconds + 1.0e-6;
}

// Unlocked floor: 80% of the requested gate period. A step released by the
// outer gate is at least one full period long on the title's clock, so it can
// never be refused (or clamped up) by a floor sitting at the period itself
// when both clocks are a few microseconds apart. The floor still rejects
// pathological near-zero steps. Every other mode returns the guest value.
constexpr double TitleSchedulerFloorSeconds(double guestFloorSeconds,
                                            FrameRatePolicy requested) noexcept
{
    if (requested.mode != FrameRateMode::HighFrameRateUnlocked ||
        !requested.requiresTitlePatches || !requested.presentationHz ||
        !IsTitleSchedulerFloorValue(guestFloorSeconds))
        return guestFloorSeconds;

    const double floor = 0.8 / double(requested.presentationHz);
    return floor < guestFloorSeconds ? floor : guestFloorSeconds;
}

// Above 60 Hz the progress, vblank-wait, async-poll and GPU-poll hooks keep the
// exact HighFrameRate60 contract. Only the outer frame gate and the scheduler
// floor know the requested rate.
constexpr FrameRatePolicy HighFrameRate60TitleContract(FrameRatePolicy policy) noexcept
{
    return policy.mode == FrameRateMode::HighFrameRateUnlocked && policy.requiresTitlePatches
        ? PolicyFor(FrameRateMode::HighFrameRate60)
        : policy;
}

// Pre-rendered video keeps its authored title cadence. The display and guest
// vblank clocks remain 60 Hz; only the title-side HFR patches are suspended
// while the command stream confirms that native-cadence video is being drawn.
// Keeping this as a pure policy transform makes the transition independently
// testable and avoids teaching the generic scheduler about any specific movie.
constexpr FrameRatePolicy TitlePolicyForNativeCadenceContent(
    FrameRatePolicy active, bool nativeCadenceContent) noexcept
{
    if (nativeCadenceContent && active.mode == FrameRateMode::HighFrameRate60 &&
        active.requiresTitlePatches)
        return PolicyFor(FrameRateMode::Compatibility30);
    if (nativeCadenceContent && active.mode == FrameRateMode::HighFrameRateUnlocked &&
        active.requiresTitlePatches)
        return PolicyFor(FrameRateMode::Compatibility30);
    return active;
}

// Unlocked frame gate. The 60 Hz path can scale r28 by exactly 2 because
// 16667 * 2 lands just above the guest's 33333 us threshold. For 120/144/165
// and other rates a pure scale lands on the wrong side of that threshold at the
// target period (144 Hz: 6944 * 144 / 30 = 33331), so the result is pinned to
// the correct side of the guest comparison: released exactly when the
// phase-corrected elapsed time reaches the target period, never earlier.
constexpr uint32_t TitleFrameGateElapsedUsUnlocked(uint32_t elapsedUs,
                                                   uint32_t targetHz,
                                                   TitleFrameGatePhase& phase) noexcept
{
    if (!targetHz)
    {
        phase.carryUs = 0;
        return elapsedUs;
    }

    constexpr uint64_t kMicrosPerSecond = 1'000'000ull;
    const uint32_t targetUs = static_cast<uint32_t>(
        (kMicrosPerSecond + targetHz / 2u) / targetHz);
    const uint64_t effectiveUs = uint64_t(elapsedUs) + phase.carryUs;
    const bool release = effectiveUs >= targetUs;
    if (release)
    {
        // Same phase rule as the 60 Hz gate: keep sub-period overshoot, drop
        // the phase after a missed whole period so a hitch never bursts.
        phase.carryUs = effectiveUs < uint64_t(targetUs) * 2u
            ? static_cast<uint32_t>(effectiveUs - targetUs)
            : 0u;
    }

    constexpr auto baseline = PolicyFor(FrameRateMode::Compatibility30);
    uint64_t scaled = effectiveUs * targetHz / baseline.presentationHz;
    if (release && scaled <= kTitleFrameGateThresholdUs)
        scaled = kTitleFrameGateThresholdUs + 1u;
    if (!release && scaled > kTitleFrameGateThresholdUs)
        scaled = kTitleFrameGateThresholdUs;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

constexpr uint32_t TitleFrameGateElapsedUs(uint32_t elapsedUs,
                                           FrameRatePolicy policy) noexcept
{
    if (policy.mode == FrameRateMode::HighFrameRateUnlocked)
    {
        TitleFrameGatePhase noPhase{};
        return policy.requiresTitlePatches
            ? TitleFrameGateElapsedUsUnlocked(elapsedUs, policy.presentationHz, noPhase)
            : elapsedUs;
    }

    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return elapsedUs;

    constexpr auto baseline = PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = uint64_t(elapsedUs) * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

// Preserve the exact long-term HFR cadence when the host scheduler resumes the
// title a little after the 16.667 ms boundary.  The title already passes the
// unmodified real delta to its update; this state affects only the gate test.
// Carry at most the sub-frame overshoot into the next gate, and deliberately
// drop phase after a missed whole frame so a hitch never causes catch-up bursts.
constexpr uint32_t TitleFrameGateElapsedUsPhased(uint32_t elapsedUs,
                                                 FrameRatePolicy policy,
                                                 TitleFrameGatePhase& phase) noexcept
{
    if (policy.mode == FrameRateMode::HighFrameRateUnlocked)
    {
        if (!policy.requiresTitlePatches)
        {
            phase.carryUs = 0;
            return elapsedUs;
        }
        return TitleFrameGateElapsedUsUnlocked(elapsedUs, policy.presentationHz, phase);
    }

    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches || !policy.presentationHz)
    {
        phase.carryUs = 0;
        return elapsedUs;
    }

    constexpr uint64_t kMicrosPerSecond = 1'000'000ull;
    const uint32_t targetUs = static_cast<uint32_t>(
        (kMicrosPerSecond + policy.presentationHz / 2u) / policy.presentationHz);
    const uint64_t effectiveUs = uint64_t(elapsedUs) + phase.carryUs;

    if (effectiveUs >= targetUs)
    {
        if (effectiveUs < uint64_t(targetUs) * 2u)
            phase.carryUs = static_cast<uint32_t>(effectiveUs - targetUs);
        else
            phase.carryUs = 0;
    }

    constexpr auto baseline = PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = effectiveUs * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

// The title's mode-1 progress synchronizer intentionally stores one tick ahead
// after consuming exactly one graphics-progress tick. With a 60 Hz graphics
// interrupt publisher, that makes the next call wait for two ticks and therefore
// reduces this route to ~30 Hz. The hook is placed only on that exact look-ahead
// path, immediately after the guest's +1 and before the store. In HFR mode we
// cancel only that look-ahead; compatibility mode preserves the original value.
constexpr uint32_t TitleProgressCadenceCounter(uint32_t lookaheadCounter,
                                               FrameRatePolicy policy) noexcept
{
    policy = HighFrameRate60TitleContract(policy);
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return lookaheadCounter;

    return lookaheadCounter - 1u;
}

// The mode-1 scheduler already enforces its own 1/60 minimum step. Waiting for
// a *future* graphics-progress/vblank tick after that scheduler gate serializes
// two independent 60 Hz clocks and lowers the effective cadence. HFR therefore
// consumes the currently published progress value without blocking for a newer
// tick; compatibility mode retains the original wait loop exactly.
constexpr bool TitleProgressWaitBypass(FrameRatePolicy policy) noexcept
{
    policy = HighFrameRate60TitleContract(policy);
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches;
}

// One title-side async state machine spins inside a single update while its
// worker reports status 4 (pending). On the original 30 FPS path that blocking
// poll is part of the observed cadence. Once the outer scheduler is released at
// 60 Hz, keeping the same busy-wait stalls the main thread for an entire
// 30-ish-Hz worker period and collapses presentation back to ~25-30 FPS.
//
// HFR mode therefore turns only the still-pending case into a cooperative poll:
// the state is left unchanged and the next title update polls again. Completed,
// failed and transitional states retain the original control flow. The 30 FPS
// compatibility policy is a strict no-op.
constexpr bool TitleAsyncPollShouldYield(uint32_t status,
                                         FrameRatePolicy policy) noexcept
{
    policy = HighFrameRate60TitleContract(policy);
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches && status == 4u;
}

// MojoRecompGpuPollBackoff: HFR modes restore the db16cyc pause instead of the
// optional scheduler yield. Unlocked rates use the same 60 Hz contract.
constexpr bool TitleGpuPollCooperative(FrameRatePolicy policy) noexcept
{
    policy = HighFrameRate60TitleContract(policy);
    return policy.mode == FrameRateMode::HighFrameRate60 && policy.requiresTitlePatches;
}

constexpr std::chrono::microseconds PeriodForHzRoundedUs(uint32_t hz) noexcept
{
    return hz == 0
        ? std::chrono::microseconds::zero()
        : std::chrono::microseconds((1'000'000ull + hz / 2u) / hz);
}

} // namespace mojorecomp::host
