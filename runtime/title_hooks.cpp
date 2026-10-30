// COT-specific hooks for native context switching and XMA buffer filling. Guest
// code remains responsible for saving and restoring its register state.

#include "ppc_recomp_shared.h"

#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <immintrin.h>

#include "runtime_state.h"
#include "cpu/guest_fiber.h"
#include "gpu/pipeline_policy.h"
#include "gpu/pm4.h"
#include "host/frame_rate_policy.h"
#include "host/pipeline_metrics.h"
#include "kernel/guestcall.h"
#include "title_hfr_discovery.h"

namespace {

struct DecoderFillState {
    uint32_t lastProgress = 0xFFFFFFFFu;
    uint32_t stallRetries = 0;
};

thread_local DecoderFillState g_decoderFillState;

// Guest addresses of the HFR hook sites in config/CrashOfTheTitans.toml, used
// only to label MOJORECOMP_HFR_DISCOVERY output.
constexpr uint32_t kHfrFrameGateSite = 0x821FD15Cu;
constexpr uint32_t kHfrProgressWaitSite = 0x8238F02Cu;
constexpr uint32_t kHfrProgressCadenceSite = 0x8238F05Cu;

mojorecomp::host::FrameRatePolicy ActiveTitleFrameRatePolicy()
{
    // An unlocked request is the 60 FPS policy until the scheduler floor hook
    // has lowered the title's 1/60 s minimum step. 30/60 pass through unchanged.
    return mojorecomp::host::TitlePolicyForNativeCadenceContent(
        mojorecomp::host::EffectiveActiveFrameRatePolicy(), Pm4_BinkVideoCadenceActive());
}

// One-time notice when an unlocked rate is requested but the scheduler-floor
// hook has not run, so the title necessarily stays at 60 Hz. The frame gate
// only runs while a state transition loads; the floor hook runs from the first
// Game::Tick, so a long loading screen alone can print this.
void NoteUnlockedRateHeldAt60()
{
    const auto requested = mojorecomp::host::ActiveFrameRatePolicy();
    if (requested.mode != mojorecomp::host::FrameRateMode::HighFrameRateUnlocked ||
        mojorecomp::host::TitleSchedulerFloorLifted().load(std::memory_order_acquire))
        return;
    static thread_local uint32_t releases = 0;
    if (++releases != 900u) // ~15 s at 60 Hz
        return;
    std::fprintf(stderr,
        "[hfr] unlocked rate %u Hz%s requested, but MojoRecompHfrSchedulerFloor has not run "
        "yet: the title stays at 60 Hz. It runs from the first Game::Tick update after "
        "loading; if it never does, check the 0x821FC1C8 hook in "
        "config/CrashOfTheTitans.toml (docs/HFR-PHASE2.md).\n",
        requested.presentationHz, requested.uncapped ? " (uncapped)" : "");
}

bool NativeFibersEnabled() {
    const char* value = std::getenv("MOJORECOMP_NATIVE_FIBERS");
    // Crash of the Titans relies on these context-switch hooks during normal
    // frontend/game boot. Keeping native fibers opt-in leaves the guest alive
    // but stalls it before the real asset-loading path (black presentation with
    // only the tiny fallback shader set). Native fibers are therefore the
    // production default; setting MOJORECOMP_NATIVE_FIBERS=0 is retained only for
    // explicit regression/diagnostic comparisons with the old fallback path.
    return !value || !*value || value[0] != '0';
}

bool ProgressDiagnosticsEnabled() {
    const char* value = std::getenv("MOJORECOMP_PROGRESS_DIAGNOSTICS");
    return value && value[0] == '1';
}

// A/B switch for MojoRecompHfrPresentImmediate: keep the title's own
// vblank-synchronised present interval in the HFR modes.
bool HfrGuestVsyncRequested() {
    static const bool requested = [] {
        const char* value = std::getenv("MOJORECOMP_HFR_GUEST_VSYNC");
        return value && value[0] == '1';
    }();
    return requested;
}

uint32_t GpuPollYieldInterval() {
    static const uint32_t interval = [] {
        const char* value = std::getenv("MOJORECOMP_GPU_POLL_YIELD");
        if (!value || !*value || value[0] == '0')
            return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value)
            return 1u;
        return static_cast<uint32_t>(parsed ? parsed : 1u);
    }();
    return interval;
}
uint32_t GpuPollDelayUs() {
    static const uint32_t delayUs = [] {
        const char* value = std::getenv("MOJORECOMP_GPU_POLL_DELAY_US");
        if (!value || !*value || value[0] == '0')
            return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value)
            return 10u;
        return static_cast<uint32_t>(parsed);
    }();
    return delayUs;
}

void GpuPollBusyDelay(uint32_t microseconds) {
    if (!microseconds)
        return;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(microseconds);
    do {
        // Keep the host core in a polite spin instead of repeatedly re-reading
        // the CP writeback cache line. This mirrors Xenon's db16cyc intent while
        // allowing the PM4 worker to publish progress without cache-line ping-pong.
        for (int i = 0; i < 32; ++i)
            _mm_pause();
    } while (std::chrono::steady_clock::now() < deadline);
}

uint32_t GuestLoadU32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, g_mojoGuestBase + address, sizeof(value));
    return _byteswap_ulong(value);
}

uint8_t GuestLoadU8(uint32_t address) {
    return *(g_mojoGuestBase + address);
}

} // namespace

void MojoRecompHfrFrameGate(PPCRegister& elapsedUs) {
    static thread_local mojorecomp::host::TitleFrameGatePhase gatePhase;
    const uint32_t measuredUs = elapsedUs.u32;
    elapsedUs.u32 = mojorecomp::host::TitleFrameGateElapsedUsPhased(
        measuredUs, ActiveTitleFrameRatePolicy(), gatePhase);
    // The guest dispatches its update when r28 exceeds the threshold, so the
    // measured value at that point is the title's frame-to-frame interval.
    if (elapsedUs.u32 > mojorecomp::host::kTitleFrameGateThresholdUs)
    {
        mojorecomp::host::NoteTitleFrameGateRelease(measuredUs);
        NoteUnlockedRateHeldAt60();
    }
    if (MojoRecompHfrDiscoveryEnabled())
        MojoRecompHfrDiscoveryObserve("frame-gate", kHfrFrameGateSite);
}

// Lowers the title's 1/60 s scheduler floor for the unlocked frame rates. The
// floor is the `lfs f13` at 0x821FC1C8 in Game::Tick (0x821FC198), the
// gameplay update the title loop calls on every pass; see the hook block in
// config/CrashOfTheTitans.toml. This one-register form is what
// MOJORECOMP_HFR_DISCOVERY prints for other candidates. 30/60 FPS never touch
// the register, and only a register that really holds 1/60 s is rewritten or
// unlocks the outer gate.
void MojoRecompHfrSchedulerFloor(PPCRegister& floor) {
    const auto requested = mojorecomp::host::ActiveFrameRatePolicy();
    if (requested.mode != mojorecomp::host::FrameRateMode::HighFrameRateUnlocked)
        return;
    if (!mojorecomp::host::IsTitleSchedulerFloorValue(floor.f64)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            std::fprintf(stderr,
                "[hfr] MojoRecompHfrSchedulerFloor saw %.9g instead of 1/60 s; "
                "the hook is not on the scheduler floor and stays inactive\n", floor.f64);
        return;
    }
    floor.f64 = mojorecomp::host::TitleSchedulerFloorSeconds(floor.f64, requested);
    if (!mojorecomp::host::TitleSchedulerFloorLifted().exchange(true, std::memory_order_acq_rel))
        std::fprintf(stderr,
            "[hfr] scheduler floor lifted: 16.667 ms -> %.3f ms; title gate now %u Hz%s\n",
            floor.f64 * 1000.0, requested.presentationHz,
            requested.uncapped ? " (uncapped ceiling)" : "");
}

// Form used at the verified Game::Tick site: f13 holds the floor just loaded
// and f0 the accumulated step that `fcmpu cr6,f0,f13; blt` compares with it.
// Gameplay updates are dispatched here rather than through the r28 frame gate
// (which only runs during asynchronous state transitions), so this also feeds
// the [pipeline] gate interval histogram in every mode.
void MojoRecompHfrSchedulerFloor(PPCRegister& floor, PPCRegister& accumulated) {
    const bool genuineFloor = mojorecomp::host::IsTitleSchedulerFloorValue(floor.f64);
    MojoRecompHfrSchedulerFloor(floor);
    // The guest skips the update only when the sum is below the floor.
    if (!genuineFloor || !(accumulated.f64 >= floor.f64))
        return;
    const double intervalUs = accumulated.f64 < 60.0 ? accumulated.f64 * 1.0e6 : 60.0e6;
    mojorecomp::host::NoteTitleFrameGateRelease(static_cast<uint32_t>(intervalUs + 0.5));
}

void MojoRecompHfrProgressCadence(PPCRegister& lookaheadCounter) {
    lookaheadCounter.u32 = mojorecomp::host::TitleProgressCadenceCounter(
        lookaheadCounter.u32, ActiveTitleFrameRatePolicy());
    if (MojoRecompHfrDiscoveryEnabled())
        MojoRecompHfrDiscoveryObserve("progress-cadence", kHfrProgressCadenceSite);
}

bool MojoRecompHfrSkipProgressWait() {
    if (MojoRecompHfrDiscoveryEnabled())
        MojoRecompHfrDiscoveryObserve("progress-wait", kHfrProgressWaitSite);
    return mojorecomp::host::TitleProgressWaitBypass(
        ActiveTitleFrameRatePolicy());
}

// Placed on the `b` that leads the mode-1 "one vblank elapsed" branch to
// `li r29,1` (present interval ONE). Returning true jumps past it, so r29 keeps
// the D3DPRESENT_INTERVAL_IMMEDIATE value the function loaded on entry.
bool MojoRecompHfrPresentImmediate() {
    if (!mojorecomp::host::TitlePresentImmediate(ActiveTitleFrameRatePolicy()))
        return false;
    const bool guestVsync = HfrGuestVsyncRequested();
    static std::atomic<bool> announced{false};
    if (!announced.load(std::memory_order_relaxed) && !announced.exchange(true))
        std::fprintf(stderr, guestVsync
            ? "[hfr] present interval: title default (MOJORECOMP_HFR_GUEST_VSYNC=1); "
              "on-time frames wait for the next guest vblank\n"
            : "[hfr] present interval: immediate; D3D no longer holds on-time frames "
              "for the next guest vblank\n");
    return !guestVsync;
}

bool MojoRecompHfrYieldAsyncPoll(PPCRegister& status) {
    return mojorecomp::host::TitleAsyncPollShouldYield(
        status.u32, ActiveTitleFrameRatePolicy());
}

void MojoRecompBeginContextSwitch(PPCRegister& target, PPCRegister& pcr) {
    if (NativeFibersEnabled())
        mojorecomp::fiber::Begin(*g_ppcContext, target.u32, pcr.u32);
}

void MojoRecompProgressProbe(PPCRegister& stateObject) {
    if (!ProgressDiagnosticsEnabled() || !g_mojoGuestBase || !stateObject.u32)
        return;

    const uint32_t p = stateObject.u32;
    const uint32_t phase = GuestLoadU32(p + 1652);
    const uint32_t mode = GuestLoadU32(p + 1660);
    const uint32_t published = GuestLoadU32(p + 1664);
    const uint32_t lastCounter = GuestLoadU32(p + 1668);
    const uint32_t renderer = GuestLoadU32(p + 16);
    const uint32_t drawSerial = renderer ? GuestLoadU32(renderer + 16408) : 0;

    struct Snapshot {
        uint32_t phase = 0xFFFFFFFFu;
        uint32_t mode = 0xFFFFFFFFu;
        uint32_t published = 0xFFFFFFFFu;
        uint32_t lastCounter = 0xFFFFFFFFu;
        uint32_t renderer = 0xFFFFFFFFu;
        uint32_t drawSerial = 0xFFFFFFFFu;
        uint64_t calls = 0;
    };
    static thread_local Snapshot last;
    ++last.calls;

    const bool changed = phase != last.phase || mode != last.mode ||
                         published != last.published || lastCounter != last.lastCounter ||
                         renderer != last.renderer || drawSerial != last.drawSerial;
    if (changed || last.calls <= 16 || (last.calls % 120u) == 0) {
        std::fprintf(stderr,
            "[progress] update=%llu obj=%08X phase=%u mode=%u published=%u counter=%u "
            "renderer=%08X drawSerial=%u\n",
            static_cast<unsigned long long>(last.calls), p, phase, mode, published,
            lastCounter, renderer, drawSerial);
    }

    last.phase = phase;
    last.mode = mode;
    last.published = published;
    last.lastCounter = lastCounter;
    last.renderer = renderer;
    last.drawSerial = drawSerial;
}

void MojoRecompGpuPollBackoff(PPCRegister& pollObject, PPCRegister& owner) {
    if (!g_mojoGuestBase || !pollObject.u32 || !owner.u32)
        return;

    const uint32_t object = pollObject.u32;
    const uint32_t state = owner.u32;
    const uint32_t statusFlags = GuestLoadU8(state + 10813);
    const uint32_t currentPtrSlot = GuestLoadU32(state + 10768);
    const uint32_t currentPtr = currentPtrSlot ? GuestLoadU32(currentPtrSlot) : 0;
    const uint32_t observedPtr = GuestLoadU32(object + 8);
    const uint32_t startTick = GuestLoadU32(object + 12);

    // The original Xenon routine executes eight db16cyc instructions before
    // polling the CP read pointer. XenonRecomp currently emits those as comments,
    // which turns this into a full-speed host spin loop. When explicitly enabled,
    // periodically yield only while the CP pointer has not advanced. Guest-visible
    // state and the routine's return value remain entirely controlled by the
    // translated code after this hook returns.
    const uint32_t yieldInterval = GpuPollYieldInterval();
    const uint32_t delayUs = GpuPollDelayUs();
    const bool waitingOnCp = !(statusFlags & 0x2u) && currentPtrSlot && currentPtr == observedPtr;

    // Account the wall time this thread spends waiting for the PM4 thread to
    // advance the CP read pointer, split by what the PM4 thread was doing at
    // each poll. Clock reads happen only while waiting.
    struct CpWaitEpisode {
        bool active = false;
        uint32_t object = 0;
        uint32_t observed = 0;
        std::chrono::steady_clock::time_point start{};
        std::chrono::steady_clock::time_point last{};
        std::chrono::steady_clock::time_point lastWake{};
        uint64_t byPm4Activity[mojorecomp::host::kPm4ActivityCount]{};
        uint64_t polls = 0;
    };
    static thread_local CpWaitEpisode cpWait;
    const bool hfrCooperativePoll =
        mojorecomp::host::TitleGpuPollCooperative(ActiveTitleFrameRatePolicy());
    if (waitingOnCp || cpWait.active) {
        const auto now = std::chrono::steady_clock::now();
        const bool sameEpisode = cpWait.active && cpWait.object == object &&
                                 cpWait.observed == observedPtr;
        auto attribute = [&](std::chrono::steady_clock::time_point until) {
            const uint32_t activity =
                static_cast<uint32_t>(mojorecomp::host::CurrentPm4Activity());
            if (activity < mojorecomp::host::kPm4ActivityCount && until > cpWait.last)
                cpWait.byPm4Activity[activity] += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        until - cpWait.last).count());
        };
        if (cpWait.active && (!waitingOnCp || !sameEpisode)) {
            // The poll loop normally calls this hook once more after the
            // pointer moves. If it left another way, end at the last poll.
            const bool endsNow = !waitingOnCp &&
                                 now - cpWait.last < std::chrono::milliseconds(1);
            const auto end = endsNow ? now : cpWait.last;
            if (endsNow)
                attribute(now);
            mojorecomp::host::NoteGuestCpWait(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - cpWait.start).count()));
            mojorecomp::host::NoteGuestCpWaitBreakdown(cpWait.byPm4Activity, cpWait.polls);
            cpWait.active = false;
        }
        if (waitingOnCp && !cpWait.active) {
            cpWait = {};
            cpWait.active = true;
            cpWait.object = object;
            cpWait.observed = observedPtr;
            cpWait.start = now;
            cpWait.last = now;
            cpWait.polls = 1;
        } else if (waitingOnCp) {
            attribute(now);
            cpWait.last = now;
            ++cpWait.polls;
        }
        // Phase 3: the guest is blocked on the PM4 thread, so do not leave the
        // pump asleep until its next poll timeout. Repeat every 50 us in case
        // the pump went back to sleep without reaching the guest's pointer.
        if (waitingOnCp && hfrCooperativePoll && mojorecomp::gpu::PromptPm4WakeRequested() &&
            (cpWait.polls == 1 || now - cpWait.lastWake >= std::chrono::microseconds(50))) {
            cpWait.lastWake = now;
            Pm4_NotifyWorkAvailable();
        }
    }

    // Xenon's db16cyc backoff becomes a no-op in the recompilation. In HFR,
    // restore that short CPU pause only while the CP pointer is unchanged. A
    // host scheduler yield is much stronger than db16cyc and measurably lowers
    // the title cadence, so HFR deliberately avoids yielding here.
    if (hfrCooperativePoll && waitingOnCp) {
        for (int i = 0; i < 8; ++i)
            _mm_pause();
    }
    else if (delayUs && waitingOnCp)
        GpuPollBusyDelay(delayUs);
    if (!hfrCooperativePoll && yieldInterval && waitingOnCp) {
        struct BackoffState {
            uint32_t object = 0;
            uint32_t observed = 0;
            uint64_t unchangedPolls = 0;
        };
        static thread_local BackoffState backoff;
        if (backoff.object != object || backoff.observed != observedPtr) {
            backoff.object = object;
            backoff.observed = observedPtr;
            backoff.unchangedPolls = 1;
        } else {
            ++backoff.unchangedPolls;
        }
        if ((backoff.unchangedPolls % yieldInterval) == 0)
            std::this_thread::yield();
    }

    if (!ProgressDiagnosticsEnabled())
        return;

    static thread_local uint64_t calls = 0;
    ++calls;
    if (calls <= 16 || (calls % 256u) == 0 || currentPtr != observedPtr) {
        std::fprintf(stderr,
            "[progress] gpu-poll=%llu poll=%08X owner=%08X flags=%02X "
            "observed=%08X current=%08X startTick=%u\n",
            static_cast<unsigned long long>(calls), object, state, statusFlags,
            observedPtr, currentPtr, startTick);
    }
}

void MojoRecompCommitContextSwitch(PPCRegister& sp, PPCRegister& thread,
                                 PPCRegister& allocationBase, PPCRegister& stackBase,
                                 PPCRegister& stackLimit) {
    if (NativeFibersEnabled())
        mojorecomp::fiber::Commit(*g_ppcContext, sp.u32, thread.u32, allocationBase.u32,
                                stackBase.u32, stackLimit.u32);
}

void MojoRecompDecoderFillBegin() {
    g_decoderFillState.lastProgress = 0xFFFFFFFFu;
    g_decoderFillState.stallRetries = 0;
}

void MojoRecompDecoderFillPrepare(PPCRegister& destination, PPCRegister& sampleCount) {
    if (!g_mojoGuestBase || !destination.u32 || sampleCount.u32 > 0x00800000u)
        return;

    std::memset(g_mojoGuestBase + destination.u32, 0,
                static_cast<std::size_t>(sampleCount.u32) * sizeof(uint16_t));
}

bool MojoRecompDecoderFillStalled(PPCRegister& progress, PPCRegister& sampleCount) {
    if (progress.u32 >= sampleCount.u32)
        return false;

    auto& state = g_decoderFillState;
    if (progress.u32 != state.lastProgress) {
        state.lastProgress = progress.u32;
        state.stallRetries = 0;
        return false;
    }

    if (++state.stallRetries < 256)
        return false;

    return true;
}
