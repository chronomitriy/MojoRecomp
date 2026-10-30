#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

#include "../host/frame_rate_policy.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

using mojorecomp::host::FrameRateMode;
using mojorecomp::host::FrameRatePolicy;
using mojorecomp::host::TitleFrameGatePhase;

// Verbatim behaviour of the 30/60 FPS policy as shipped in COT runtime 0.2.0
// (b5450a7). Every new mode must leave these results unchanged for the
// Compatibility30 and HighFrameRate60 policies.
namespace reference {

constexpr FrameRatePolicy PolicyFor(FrameRateMode mode) noexcept
{
    switch (mode)
    {
        case FrameRateMode::HighFrameRate60:
            return {FrameRateMode::HighFrameRate60, 60, 60, 60, 60, true};
        case FrameRateMode::Compatibility30:
        default:
            return {FrameRateMode::Compatibility30, 30, 30, 60, 60, false};
    }
}

constexpr FrameRateMode FrameRateModeForSetting(std::string_view value) noexcept
{
    return value == "60" ? FrameRateMode::HighFrameRate60
                         : FrameRateMode::Compatibility30;
}

constexpr FrameRatePolicy TitlePolicyForNativeCadenceContent(
    FrameRatePolicy active, bool nativeCadenceContent) noexcept
{
    if (nativeCadenceContent && active.mode == FrameRateMode::HighFrameRate60 &&
        active.requiresTitlePatches)
        return reference::PolicyFor(FrameRateMode::Compatibility30);
    return active;
}

constexpr uint32_t TitleFrameGateElapsedUs(uint32_t elapsedUs,
                                           FrameRatePolicy policy) noexcept
{
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return elapsedUs;

    constexpr auto baseline = reference::PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = uint64_t(elapsedUs) * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

constexpr uint32_t TitleFrameGateElapsedUsPhased(uint32_t elapsedUs,
                                                 FrameRatePolicy policy,
                                                 TitleFrameGatePhase& phase) noexcept
{
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

    constexpr auto baseline = reference::PolicyFor(FrameRateMode::Compatibility30);
    if (!baseline.presentationHz || policy.presentationHz <= baseline.presentationHz)
        return elapsedUs;

    const uint64_t scaled = effectiveUs * policy.presentationHz /
                            baseline.presentationHz;
    return scaled > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(scaled);
}

constexpr uint32_t TitleProgressCadenceCounter(uint32_t lookaheadCounter,
                                               FrameRatePolicy policy) noexcept
{
    if (policy.mode != FrameRateMode::HighFrameRate60 ||
        !policy.requiresTitlePatches)
        return lookaheadCounter;
    return lookaheadCounter - 1u;
}

constexpr bool TitleProgressWaitBypass(FrameRatePolicy policy) noexcept
{
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches;
}

constexpr bool TitleAsyncPollShouldYield(uint32_t status, FrameRatePolicy policy) noexcept
{
    return policy.mode == FrameRateMode::HighFrameRate60 &&
           policy.requiresTitlePatches && status == 4u;
}

} // namespace reference

bool SamePolicy(const FrameRatePolicy& a, const FrameRatePolicy& b)
{
    return a.mode == b.mode && a.simulationHz == b.simulationHz &&
           a.presentationHz == b.presentationHz && a.displayHz == b.displayHz &&
           a.guestVblankHz == b.guestVblankHz &&
           a.requiresTitlePatches == b.requiresTitlePatches && a.uncapped == b.uncapped;
}

// Deterministic pseudo-random elapsed times around the interesting boundaries.
struct Lcg
{
    uint64_t state = 0x4D4F4A4F52454350ull;
    uint32_t Next()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    }
};

struct LoopResult
{
    std::vector<uint32_t> intervalsUs;
};

// Models the title loop: after each dispatched update the loop resumes once
// the update's work is done, then re-measures r28 every pollUs until the gate
// (the hooked comparison r28 > 33333) releases the next update.
template <typename Gate>
LoopResult SimulateTitleLoop(Gate&& gate, uint64_t durationUs, uint32_t pollUs,
                             uint32_t (*workUs)(uint32_t frame))
{
    LoopResult result;
    uint64_t now = 0;
    uint64_t lastDispatch = 0;
    uint32_t frame = 0;
    while (now < durationUs)
    {
        const uint64_t elapsed = now - lastDispatch;
        const uint32_t gated = gate(static_cast<uint32_t>(elapsed));
        if (gated > mojorecomp::host::kTitleFrameGateThresholdUs)
        {
            if (frame)
                result.intervalsUs.push_back(static_cast<uint32_t>(elapsed));
            lastDispatch = now;
            now += workUs(frame++);
        }
        else
        {
            now += pollUs;
        }
    }
    return result;
}

struct TickResult
{
    uint64_t updates = 0;
    double updatedSeconds = 0.0; // sum of the steps the updates received
    double shortestStep = 1.0e9;
};

// Models the gameplay path of the title loop (guest 0x821FCFB8) and Game::Tick
// (0x821FC198) as decompiled: every loop pass stores its timestamp and calls
// Tick with the real delta since the previous pass (a float, us * 1e-6f).
// Tick adds it to a float accumulator and returns while the sum is below the
// floor; the update receives the whole sum, clamped to 0.1 s, and resets it.
template <typename Work>
TickResult SimulateTitleTick(double floorSeconds, uint64_t durationUs, uint32_t passUs,
                             Work&& workUs)
{
    TickResult result;
    float accumulator = 0.0f;
    uint64_t now = 0;
    uint64_t lastPass = 0;
    uint32_t frame = 0;
    while (now < durationUs)
    {
        const float dt = float(now - lastPass) * 1.0e-6f;
        lastPass = now;
        accumulator = float(double(dt) + double(accumulator));
        if (double(accumulator) < floorSeconds)
        {
            now += passUs;
            continue;
        }
        const float step = accumulator > 0.1f ? 0.1f : accumulator;
        ++result.updates;
        result.updatedSeconds += step;
        result.shortestStep = std::min(result.shortestStep, double(step));
        accumulator = 0.0f;
        now += passUs + workUs(frame++);
    }
    return result;
}

struct PresentPathResult
{
    uint32_t heldFrames = 0; // XE_SWAP waited for a later guest vblank
    uint64_t heldUs = 0;
};

// Models the decompiled present path. Title (0x8238EFE8, mode 1 with the HFR
// progress hooks): interval ONE when exactly one vblank passed since the last
// present, immediate otherwise. D3D: the swap callback (0x82273A40, immediate
// threshold 0) flips now or queues the flip for a target vblank, and the
// command processor waits until the vblank handler (0x822738D0) performs it.
PresentPathResult SimulatePresentPath(bool presentImmediate, uint32_t frameUs,
                                      uint32_t cpLagUs, uint32_t frames)
{
    constexpr uint32_t kVblankUs = 16667;
    PresentPathResult result;
    uint32_t titleLastCounter = 0; // this+1668
    uint32_t lastTarget = 0;       // dev+16404
    uint64_t cpFreeAt = 0;
    for (uint32_t k = 1; k <= frames; ++k)
    {
        const uint64_t presentAt = uint64_t(k) * frameUs;
        const uint32_t counter = static_cast<uint32_t>(presentAt / kVblankUs);
        const uint32_t elapsedVblanks = counter - titleLastCounter;
        titleLastCounter = counter;
        const uint32_t interval = elapsedVblanks == 1 && !presentImmediate ? 1u : 0u;

        const uint64_t callbackAt = std::max<uint64_t>(presentAt + cpLagUs, cpFreeAt);
        const uint32_t vblanks = static_cast<uint32_t>(callbackAt / kVblankUs);
        uint32_t target = lastTarget + interval;
        if (target <= vblanks)
            target = interval ? vblanks + 1u : vblanks;
        lastTarget = target;
        uint64_t swapAt = callbackAt;
        if (target != vblanks)
        {
            swapAt = uint64_t(target) * kVblankUs;
            ++result.heldFrames;
            result.heldUs += swapAt - callbackAt;
        }
        cpFreeAt = swapAt;
    }
    return result;
}

} // namespace

int main()
{
    using mojorecomp::host::ActiveFrameRatePolicy;
    using mojorecomp::host::EffectiveTitleFrameRatePolicy;
    using mojorecomp::host::FrameRateModeForSetting;
    using mojorecomp::host::FrameRateModeName;
    using mojorecomp::host::HighFrameRate60TitleContract;
    using mojorecomp::host::IsTitleSchedulerFloorValue;
    using mojorecomp::host::kTitleFrameGateThresholdUs;
    using mojorecomp::host::kUnlockedMaxHz;
    using mojorecomp::host::PeriodForHzRoundedUs;
    using mojorecomp::host::PolicyFor;
    using mojorecomp::host::PolicyForSetting;
    using mojorecomp::host::PolicyForSettings;
    using mojorecomp::host::TitleAsyncPollShouldYield;
    using mojorecomp::host::TitleFrameGateElapsedUs;
    using mojorecomp::host::TitleFrameGateElapsedUsPhased;
    using mojorecomp::host::TitleGpuPollCooperative;
    using mojorecomp::host::TitlePolicyForNativeCadenceContent;
    using mojorecomp::host::TitlePresentImmediate;
    using mojorecomp::host::TitleProgressCadenceCounter;
    using mojorecomp::host::TitleProgressWaitBypass;
    using mojorecomp::host::TitleSchedulerFloorSeconds;
    using mojorecomp::host::UnlockedRateHzForSetting;

    const auto active = ActiveFrameRatePolicy();
    if (active.mode != FrameRateMode::Compatibility30 ||
        active.simulationHz != 30 || active.presentationHz != 30 ||
        active.displayHz != 60 || active.guestVblankHz != 60 ||
        active.requiresTitlePatches)
        return Fail("active policy is not the validated 30 FPS baseline");

    if (PeriodForHzRoundedUs(active.simulationHz) != std::chrono::microseconds(33333))
        return Fail("30 Hz simulation cadence changed");
    if (PeriodForHzRoundedUs(active.guestVblankHz) != std::chrono::microseconds(16667))
        return Fail("60 Hz guest vblank cadence changed");

    if (FrameRateModeForSetting("60") != FrameRateMode::HighFrameRate60 ||
        FrameRateModeForSetting("30") != FrameRateMode::Compatibility30 ||
        FrameRateModeForSetting("") != FrameRateMode::Compatibility30 ||
        FrameRateModeForSetting("invalid") != FrameRateMode::Compatibility30)
        return Fail("frame-rate setting parser lost safe fallback semantics");

    const auto high = PolicyFor(FrameRateMode::HighFrameRate60);
    if (high.simulationHz != 60 || high.presentationHz != 60 ||
        high.displayHz != 60 || high.guestVblankHz != 60 ||
        !high.requiresTitlePatches)
        return Fail("60 FPS policy lost the title-patch safety contract");

    const auto video = TitlePolicyForNativeCadenceContent(high, true);
    if (video.mode != FrameRateMode::Compatibility30 ||
        video.simulationHz != 30 || video.presentationHz != 30 ||
        video.displayHz != 60 || video.guestVblankHz != 60 ||
        video.requiresTitlePatches)
        return Fail("native-cadence content did not suspend only the HFR title policy");
    if (TitlePolicyForNativeCadenceContent(high, false).mode !=
            FrameRateMode::HighFrameRate60 ||
        TitlePolicyForNativeCadenceContent(active, true).mode !=
            FrameRateMode::Compatibility30)
        return Fail("native-cadence policy transform changed non-video behavior");

    if (TitleFrameGateElapsedUs(33333u, active) != 33333u)
        return Fail("30 FPS compatibility mode unexpectedly changes the title frame gate");
    if (TitleFrameGateElapsedUs(16666u, high) != 33332u ||
        TitleFrameGateElapsedUs(16667u, high) != 33334u)
        return Fail("60 FPS title frame gate no longer maps the 16.667 ms boundary exactly");

    TitleFrameGatePhase phase{};
    if (TitleFrameGateElapsedUsPhased(17000u, high, phase) != 34000u ||
        phase.carryUs != 333u)
        return Fail("HFR phase gate did not preserve a small scheduling overshoot");
    if (TitleFrameGateElapsedUsPhased(16333u, high, phase) != 33332u ||
        phase.carryUs != 333u)
        return Fail("HFR phase gate released one microsecond too early");
    if (TitleFrameGateElapsedUsPhased(16334u, high, phase) != 33334u ||
        phase.carryUs != 0u)
        return Fail("HFR phase gate did not repay the previous overshoot exactly");
    phase.carryUs = 500u;
    (void)TitleFrameGateElapsedUsPhased(40000u, high, phase);
    if (phase.carryUs != 0u)
        return Fail("HFR phase gate carried debt across a missed whole frame");
    phase.carryUs = 123u;
    if (TitleFrameGateElapsedUsPhased(33333u, active, phase) != 33333u ||
        phase.carryUs != 0u)
        return Fail("30 FPS compatibility mode retained HFR phase state");

    if (TitleProgressCadenceCounter(101u, active) != 101u)
        return Fail("30 FPS compatibility mode unexpectedly changes progress cadence");
    if (TitleProgressCadenceCounter(101u, high) != 100u ||
        TitleProgressCadenceCounter(0u, high) != 0xFFFFFFFFu)
        return Fail("60 FPS progress cadence no longer cancels exactly one look-ahead tick");
    if (TitleProgressWaitBypass(active) || !TitleProgressWaitBypass(high))
        return Fail("progress wait bypass lost its HFR-only safety contract");
    if (TitlePresentImmediate(active) || !TitlePresentImmediate(high) ||
        TitlePresentImmediate(video))
        return Fail("immediate present interval lost its HFR-only safety contract");

    if (TitleAsyncPollShouldYield(4u, active))
        return Fail("30 FPS compatibility mode unexpectedly yields the async poll");
    if (!TitleAsyncPollShouldYield(4u, high) ||
        TitleAsyncPollShouldYield(0u, high) ||
        TitleAsyncPollShouldYield(1u, high) ||
        TitleAsyncPollShouldYield(3u, high))
        return Fail("60 FPS async poll yield no longer targets only pending status 4");

    // --- The 30 and 60 FPS paths are unchanged from 0.2.0. ---------------
    {
        const std::string_view settings[] = {
            "", "30", "60", "invalid", "0", "59", "60 ", " 60", "060", "6O", "-60"};
        for (std::string_view setting : settings)
        {
            if (UnlockedRateHzForSetting(setting))
                return Fail("a 0.2.0 setting value was reinterpreted as an unlocked rate");
            if (!SamePolicy(PolicyForSettings(setting, ""), PolicyForSetting(setting)))
                return Fail("an empty frame-rate target changed a 0.2.0 setting value");
            if (FrameRateModeForSetting(setting) != reference::FrameRateModeForSetting(setting) ||
                !SamePolicy(PolicyForSetting(setting),
                            reference::PolicyFor(reference::FrameRateModeForSetting(setting))))
                return Fail("a 0.2.0 setting value resolves to a different policy");
        }

        const FrameRatePolicy policies[] = {
            reference::PolicyFor(FrameRateMode::Compatibility30),
            reference::PolicyFor(FrameRateMode::HighFrameRate60),
            reference::TitlePolicyForNativeCadenceContent(
                reference::PolicyFor(FrameRateMode::HighFrameRate60), true),
        };
        for (const auto& policy : policies)
        {
            if (!SamePolicy(PolicyFor(policy.mode), reference::PolicyFor(policy.mode)) ||
                !SamePolicy(EffectiveTitleFrameRatePolicy(policy, false), policy) ||
                !SamePolicy(EffectiveTitleFrameRatePolicy(policy, true), policy) ||
                !SamePolicy(HighFrameRate60TitleContract(policy), policy))
                return Fail("unlocked-mode transforms are not identities for 30/60");
            for (bool video : {false, true})
                if (!SamePolicy(TitlePolicyForNativeCadenceContent(policy, video),
                                reference::TitlePolicyForNativeCadenceContent(policy, video)))
                    return Fail("native-cadence transform changed for 30/60");
            for (uint32_t elapsed = 0; elapsed <= 200000u; ++elapsed)
                if (TitleFrameGateElapsedUs(elapsed, policy) !=
                    reference::TitleFrameGateElapsedUs(elapsed, policy))
                    return Fail("30/60 frame gate differs from 0.2.0");
            for (uint32_t elapsed : {0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu})
                if (TitleFrameGateElapsedUs(elapsed, policy) !=
                    reference::TitleFrameGateElapsedUs(elapsed, policy))
                    return Fail("30/60 frame gate overflow handling differs from 0.2.0");

            TitleFrameGatePhase now{};
            TitleFrameGatePhase then{};
            Lcg rng;
            for (uint32_t i = 0; i < 1'000'000u; ++i)
            {
                const uint32_t elapsed = (i % 97u) == 0 ? rng.Next() : rng.Next() % 60000u;
                if (TitleFrameGateElapsedUsPhased(elapsed, policy, now) !=
                        reference::TitleFrameGateElapsedUsPhased(elapsed, policy, then) ||
                    now.carryUs != then.carryUs)
                    return Fail("30/60 phased frame gate differs from 0.2.0");
            }
            for (uint32_t counter : {0u, 1u, 101u, 0x7FFFFFFFu, 0xFFFFFFFFu})
                if (TitleProgressCadenceCounter(counter, policy) !=
                    reference::TitleProgressCadenceCounter(counter, policy))
                    return Fail("30/60 progress cadence differs from 0.2.0");
            if (TitleProgressWaitBypass(policy) != reference::TitleProgressWaitBypass(policy))
                return Fail("30/60 progress wait bypass differs from 0.2.0");
            // New in this branch, and only where the 0.2.0 HFR progress hooks apply.
            if (TitlePresentImmediate(policy) != reference::TitleProgressWaitBypass(policy))
                return Fail("immediate present interval reaches a non-HFR policy");
            for (uint32_t status = 0; status < 16u; ++status)
                if (TitleAsyncPollShouldYield(status, policy) !=
                    reference::TitleAsyncPollShouldYield(status, policy))
                    return Fail("30/60 async poll yield differs from 0.2.0");
            if (TitleGpuPollCooperative(policy) != reference::TitleProgressWaitBypass(policy))
                return Fail("30/60 GPU poll cooperation differs from 0.2.0");
            if (TitleSchedulerFloorSeconds(1.0 / 60.0, policy) != 1.0 / 60.0)
                return Fail("30/60 modes must never rewrite the scheduler floor");
        }
    }

    // --- Late frames are not quantized to vblank ticks by the HFR gate. ---
    {
        auto hfrGate = [phase = TitleFrameGatePhase{}](uint32_t elapsed) mutable {
            return TitleFrameGateElapsedUsPhased(
                elapsed, PolicyFor(FrameRateMode::HighFrameRate60), phase);
        };
        // 25 ms of work per update: a ~40 FPS scene. If the gate rounded late
        // updates up to whole 16.667 ms ticks, intervals would be 33.3 ms.
        const auto late = SimulateTitleLoop(hfrGate, 10'000'000ull, 100u,
                                            [](uint32_t) { return 25000u; });
        for (uint32_t interval : late.intervalsUs)
            if (interval != 25000u)
                return Fail("the HFR gate delayed a late update to a vblank tick");

        // Alternating 15/22 ms of work: each update is released as soon as it
        // is both late enough for the gate and finished; never a full tick later.
        auto hfrGate2 = [phase = TitleFrameGatePhase{}](uint32_t elapsed) mutable {
            return TitleFrameGateElapsedUsPhased(
                elapsed, PolicyFor(FrameRateMode::HighFrameRate60), phase);
        };
        const auto mixed = SimulateTitleLoop(hfrGate2, 10'000'000ull, 50u,
                                             [](uint32_t frame) {
                                                 return (frame & 1u) ? 22000u : 15000u;
                                             });
        for (uint32_t interval : mixed.intervalsUs)
            if (interval > 22000u + 50u)
                return Fail("the HFR gate added a whole-tick delay to a mixed-cost scene");

        // Fast work is held at exactly 60 Hz in the long run.
        auto hfrGate3 = [phase = TitleFrameGatePhase{}](uint32_t elapsed) mutable {
            return TitleFrameGateElapsedUsPhased(
                elapsed, PolicyFor(FrameRateMode::HighFrameRate60), phase);
        };
        const auto fast = SimulateTitleLoop(hfrGate3, 10'000'000ull, 37u,
                                            [](uint32_t) { return 9000u; });
        if (fast.intervalsUs.size() < 598u || fast.intervalsUs.size() > 600u)
            return Fail("the HFR gate drifted away from 60 Hz for fast scenes");
    }

    // --- The HFR present path no longer waits for guest vblanks. -----------
    {
        // A 40 FPS scene: with the title's own choice, the frames presented
        // exactly one vblank after the previous one get interval ONE, and the
        // command processor holds their XE_SWAP for the next vblank.
        const bool hfrImmediate = TitlePresentImmediate(high);
        const auto titleDefault = SimulatePresentPath(false, 25000u, 3000u, 600u);
        const auto hooked = SimulatePresentPath(hfrImmediate, 25000u, 3000u, 600u);
        if (titleDefault.heldFrames < 100u || titleDefault.heldUs < 100u * 1000u)
            return Fail("present-path model no longer shows the vblank hold it documents");
        if (hooked.heldFrames != 0u || hooked.heldUs != 0u)
            return Fail("an HFR present was still held for a guest vblank");
        // At a steady 60 every frame took interval ONE; none is held now.
        if (SimulatePresentPath(false, 16667u, 3000u, 600u).heldFrames < 590u ||
            SimulatePresentPath(hfrImmediate, 16667u, 3000u, 600u).heldFrames != 0u)
            return Fail("steady 60 FPS presents are still held for a guest vblank");
    }

    // --- Unlocked rates above 60 Hz. ---------------------------------------
    {
        struct SettingCase
        {
            std::string_view value;
            uint32_t hz;
            bool uncapped;
        };
        constexpr SettingCase kSettings[] = {
            {"120", 120, false}, {"144", 144, false}, {"165", 165, false},
            {"61", 61, false},   {"162", 162, false}, {"300", 300, false},
            {"uncapped", kUnlockedMaxHz, true},
        };
        for (const auto& c : kSettings)
        {
            const auto policy = PolicyForSetting(c.value);
            if (UnlockedRateHzForSetting(c.value) != c.hz ||
                FrameRateModeForSetting(c.value) != FrameRateMode::HighFrameRateUnlocked ||
                policy.mode != FrameRateMode::HighFrameRateUnlocked ||
                policy.simulationHz != c.hz || policy.presentationHz != c.hz ||
                policy.displayHz != 60 || policy.guestVblankHz != 60 ||
                !policy.requiresTitlePatches || policy.uncapped != c.uncapped)
                return Fail("unlocked setting did not produce its policy");
        }
        for (std::string_view rejected : {"301", "1000", "uncap", "Uncapped", "12O", "+120"})
            if (UnlockedRateHzForSetting(rejected) ||
                PolicyForSetting(rejected).mode != FrameRateMode::Compatibility30)
                return Fail("an invalid unlocked value did not fall back to 30 FPS");

        // Launcher form: 60 plus a target. A target never changes 30 or a
        // non-unlocked value, and an invalid target leaves the 60 FPS mode.
        if (!SamePolicy(PolicyForSettings("60", "144"), PolicyForSetting("144")) ||
            !SamePolicy(PolicyForSettings("60", "uncapped"), PolicyForSetting("uncapped")) ||
            !SamePolicy(PolicyForSettings("120", ""), PolicyForSetting("120")) ||
            !SamePolicy(PolicyForSettings("60", ""), PolicyFor(FrameRateMode::HighFrameRate60)) ||
            !SamePolicy(PolicyForSettings("60", "bogus"), PolicyFor(FrameRateMode::HighFrameRate60)) ||
            !SamePolicy(PolicyForSettings("60", "60"), PolicyFor(FrameRateMode::HighFrameRate60)) ||
            !SamePolicy(PolicyForSettings("30", "144"), PolicyFor(FrameRateMode::Compatibility30)) ||
            !SamePolicy(PolicyForSettings("", "144"), PolicyFor(FrameRateMode::Compatibility30)))
            return Fail("MOJORECOMP_FRAME_RATE_TARGET is not limited to the 60 FPS setting");

        const auto u144 = PolicyForSetting("144");
        // Until the scheduler floor is lifted an unlocked request is exactly 60.
        if (!SamePolicy(EffectiveTitleFrameRatePolicy(u144, false),
                        PolicyFor(FrameRateMode::HighFrameRate60)) ||
            !SamePolicy(EffectiveTitleFrameRatePolicy(u144, true), u144))
            return Fail("the scheduler-floor interlock does not hold unlocked rates at 60");

        // Progress, wait, async and GPU-poll hooks keep the 60 Hz contract.
        if (TitleProgressCadenceCounter(101u, u144) != 100u ||
            !TitleProgressWaitBypass(u144) || !TitleAsyncPollShouldYield(4u, u144) ||
            TitleAsyncPollShouldYield(3u, u144) || !TitleGpuPollCooperative(u144) ||
            !TitlePresentImmediate(u144) ||
            !TitlePresentImmediate(EffectiveTitleFrameRatePolicy(u144, false)) ||
            TitlePresentImmediate(TitlePolicyForNativeCadenceContent(u144, true)))
            return Fail("unlocked mode changed the 60 Hz progress-hook contract");
        if (!SamePolicy(TitlePolicyForNativeCadenceContent(u144, true),
                        PolicyFor(FrameRateMode::Compatibility30)))
            return Fail("unlocked mode does not suspend for pre-rendered video");

        // The gate releases exactly at the target period on the guest's fixed
        // r28 > 33333 comparison, for rates where a pure scale would not.
        for (const auto& c : kSettings)
        {
            const auto policy = PolicyForSetting(c.value);
            const uint32_t target = static_cast<uint32_t>((1'000'000u + c.hz / 2u) / c.hz);
            if (TitleFrameGateElapsedUs(target - 1u, policy) > kTitleFrameGateThresholdUs ||
                TitleFrameGateElapsedUs(target, policy) <= kTitleFrameGateThresholdUs)
                return Fail("unlocked gate does not release exactly at the target period");
        }

        // Long-run cadence for every unlocked rate, with coarse host polling.
        for (const auto& c : kSettings)
        {
            const auto policy = PolicyForSetting(c.value);
            auto gate = [policy, phase = TitleFrameGatePhase{}](uint32_t elapsed) mutable {
                return TitleFrameGateElapsedUsPhased(elapsed, policy, phase);
            };
            const auto run = SimulateTitleLoop(gate, 10'000'000ull, 41u,
                                               [](uint32_t) { return 1500u; });
            const double expected = 10.0 * c.hz;
            const double got = double(run.intervalsUs.size());
            if (got < expected - 2.0 || got > expected + 1.0)
                return Fail("unlocked gate cadence drifted from its target rate");
            const uint32_t target = static_cast<uint32_t>((1'000'000u + c.hz / 2u) / c.hz);
            for (uint32_t interval : run.intervalsUs)
                if (interval + 41u < target)
                    return Fail("unlocked gate released an update early");
        }

        // A hitch drops phase instead of bursting several catch-up updates.
        TitleFrameGatePhase hitch{};
        hitch.carryUs = 3000u;
        (void)TitleFrameGateElapsedUsPhased(50000u, u144, hitch);
        if (hitch.carryUs != 0u)
            return Fail("unlocked gate carried debt across a hitch");

        // The scheduler floor is rewritten only for a genuine 1/60 s value.
        const double floatSixtieth = double(1.0f / 60.0f);
        if (!IsTitleSchedulerFloorValue(floatSixtieth) ||
            !IsTitleSchedulerFloorValue(1.0 / 60.0) ||
            IsTitleSchedulerFloorValue(1.0 / 30.0) || IsTitleSchedulerFloorValue(0.0))
            return Fail("scheduler floor detection is too loose or too strict");
        const double floor144 = TitleSchedulerFloorSeconds(floatSixtieth, u144);
        if (floor144 < 1.0 / 144.0 - 1.0e-12 || floor144 > 1.0 / 144.0 + 1.0e-12)
            return Fail("unlocked scheduler floor is not the target period");
        if (TitleSchedulerFloorSeconds(0.5, u144) != 0.5)
            return Fail("a non-floor register value was rewritten");
        const double floorUncapped =
            TitleSchedulerFloorSeconds(floatSixtieth, PolicyForSetting("uncapped"));
        if (floorUncapped < 1.0 / kUnlockedMaxHz - 1.0e-12 ||
            floorUncapped > 1.0 / kUnlockedMaxHz + 1.0e-12)
            return Fail("uncapped floor does not keep the safety ceiling");

        // Game::Tick: the update rate follows the floor, and every second of
        // real time reaches the updates (the accumulator never drops a step).
        for (const auto& c : kSettings)
        {
            const auto policy = PolicyForSetting(c.value);
            const double floor = TitleSchedulerFloorSeconds(floatSixtieth, policy);
            for (uint32_t passUs : {40u, 400u, 1100u})
            {
                const auto run = SimulateTitleTick(floor, 10'000'000ull, passUs,
                                                   [](uint32_t) { return 1500u; });
                const double expected = 10.0 * c.hz;
                const double got = double(run.updates);
                // A pass that crosses the floor overshoots it by at most one pass.
                const double slowest = 10.0e6 / (1.0e6 / c.hz + passUs);
                if (got > expected + 1.0 || got < slowest - 2.0)
                    return Fail("Game::Tick with the unlocked floor misses its target rate");
                if (run.updatedSeconds < 10.0 - 0.1 - 1.0e-3 || run.updatedSeconds > 10.0 + 1.0e-3)
                    return Fail("Game::Tick with the unlocked floor lost or invented time");
                if (run.shortestStep < floor - 1.0e-9)
                    return Fail("Game::Tick dispatched a step below the floor");
            }
        }
        // The guest floor itself still caps the 60 FPS policy at 60 Hz.
        {
            const double floor60 = TitleSchedulerFloorSeconds(
                floatSixtieth, PolicyFor(FrameRateMode::HighFrameRate60));
            const auto run = SimulateTitleTick(floor60, 10'000'000ull, 40u,
                                               [](uint32_t) { return 1500u; });
            if (floor60 != floatSixtieth || run.updates > 600u || run.updates < 590u)
                return Fail("the 60 FPS policy no longer runs Game::Tick at 60 Hz");
        }

        if (std::string_view(FrameRateModeName(FrameRateMode::HighFrameRateUnlocked)) !=
                "hfr-unlocked" ||
            std::string_view(FrameRateModeName(FrameRateMode::HighFrameRate60)) != "hfr60")
            return Fail("frame-rate mode names changed");
    }

    std::puts("PASS: simulation, presentation and display/vblank rates are explicit and separate.");
    return 0;
}
