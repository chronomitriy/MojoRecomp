# HFR Unlimited: CPU cost and frame rates above 60

Branch: `hfr-unlimited` (based on `b5450a7` = upstream v1.1.0, COT runtime 0.2.0-alpha).

This is a personal, self-built experiment. Nothing here is an official MojoRecomp
release. All work was done without the game: code was read, compiled and
unit-tested in a cloud container. Nothing was run against Crash of the Titans.
[Section 6](#6-what-was-and-was-not-verified) lists exactly what was and was not verified.

> **Phase 2 update.** [HFR-PHASE2.md](HFR-PHASE2.md) was written with the game's executable.
> It locates and hooks the scheduler floor, explains and fixes the 40/20 FPS plateaus,
> enables part of the register localization experimentally and stops write-protecting
> command memory. Where this file says something can only be found with the game, the
> answer is now there; statements it corrects are marked "Phase 2:".
>
> **Phase 3 update.** [HFR-PHASE3.md](HFR-PHASE3.md) removes the swap handshake's wait for
> the Vulkan thread, wakes the PM4 pump promptly and speeds up the vertex path.
>
> **Phase 4 update.** [HFR-PHASE4.md](HFR-PHASE4.md) adds a motion blur toggle, FSR 1
> upscaling/sharpening, and GPU and guest CPU profilers, all off by default.

## Contents

1. [Summary](#1-summary)
2. [Phase 1: CPU cost](#2-phase-1-cpu-cost)
3. [Phase 2: above 60 FPS](#3-phase-2-above-60-fps)
4. [Build and test on Windows](#4-build-and-test-on-windows)
5. [What to check in the logs](#5-what-to-check-in-the-logs)
6. [What was and was not verified](#6-what-was-and-was-not-verified)
7. [Gameplay checklist](#7-gameplay-checklist)
8. [Environment variables](#8-environment-variables)

## 1. Summary

| Question | Answer |
| --- | --- |
| Does guest execution run serially with PM4 parsing and draw translation? | **No.** There are three stages on three threads: guest → PM4 parser (also snapshots guest memory per draw) → Vulkan executor. The guest can still wait on the later stages through the CP read pointer, GPU fences and interrupts. [2.1](#21-how-a-frame-flows-through-the-runtime) |
| Which stage limits your 37 FPS scenes? | **Not known from 0.2.0 logs.** "guest+PM4 26.65 ms" is the whole frame interval minus present time, not guest time. `tools\summarize-runtime-log.bat` works out the Vulkan thread's share from your **existing** log. The new `[pipeline]` line names the busiest stage directly. [2.2](#22-reading-your-020-numbers) |
| Per-draw CPU cost | **Fixed:** two linear shader-module searches, one linear pipeline search and one Win32 key-state syscall on every draw. Lookups dropped from 1.1–7.5 µs to 0.06 µs per draw in a cloud microbenchmark. **Opt-in:** hot-page snapshots, against write-protection fault storms. [2.3](#23-per-draw-cost-what-changed) [2.4](#24-snapshot-write-protection) |
| Does the HFR gate quantize late frames to 2–3 vblank ticks? | **No.** A unit test runs the real gate with 25 ms updates: each one is released at exactly 25 ms. The 40/20 FPS plateaus must come from another wait; the new counters identify which. [2.6](#26-are-late-frames-quantized-to-vblank-ticks) |
| Can the game run above 60 at correct speed? | **Phase 2: the address is found and hooked** ([HFR-PHASE2 3](HFR-PHASE2.md#3-the-scheduler-floor-e2)). Original answer: **only after one title-specific address is found on your PC.** The title's own scheduler refuses steps shorter than 1/60 s. The new 120/144/165/uncapped modes therefore stay at exactly 60 until a hook lowers that floor. `MOJORECOMP_HFR_DISCOVERY=1` locates it and prints the config block to add. [3](#3-phase-2-above-60-fps) |
| Install Runtime from ZIP? | **Not possible for a self-built runtime.** The launcher only accepts ZIPs signed with the official release key (`launcher/src-tauri/src/updates.rs:882`). Use `tools\run-custom-runtime.bat` instead. [4.5](#45-run-the-custom-runtime) |

Priority order for your PC:

1. Run `tools\summarize-runtime-log.bat` on your existing 0.2.0 log.
2. Build the branch and play the same scene in 60 mode. Compare `Vulkan perf` FPS and read `[pipeline]`.
3. A/B `MOJORECOMP_SNAPSHOT_HOT_PAGES=1`.
4. Only then do the above-60 discovery.

Above 60 only helps if the CPU-bound scenes get well past 60 first.

## 2. Phase 1: CPU cost

### 2.1 How a frame flows through the runtime

| Thread | Work | Code |
| --- | --- | --- |
| Guest main thread (plus other guest threads) | Recompiled game code. Writes PM4 command packets into the ring buffer. Spins in the title's CP-poll routine while the read pointer has not moved. | `title_hooks.cpp` `MojoRecompGpuPollBackoff` (guest `0x8226D984`) |
| `MojoRecomp PM4` | Parses the ring and indirect buffers. Evaluates `WAIT_REG_MEM` (a stall retries after at most 250 µs). On every draw, builds a `GuestMemorySnapshot` (index, vertex and texture ranges) and enqueues it. | `gpu/vd.cpp:335` `GraphicsProgressPump`, `gpu/pm4.cpp:921`, `gpu/renderer_probe.cpp:130` `OnDraw` |
| `MojoRecomp Vulkan` | `AsynchronousGpuExecutor` worker: translates draws (`VkPresenter_Draw`), resolves, presents. Two frames in flight on the GPU. | `gpu/gpu_executor.cpp:400`, `gpu/renderer_probe.cpp:416` (production default), `gpu/vk_presenter.cpp:2413` |
| Guest vblank pump | Delivers source-0 interrupts at a fixed 60 Hz, independent of rendering. | `gpu/vd.cpp:225` |

So the answer to "pipeline them" is that they are already pipelined. The couplings are:

- **CP read pointer.** The guest spins until the PM4 thread has consumed far enough, for ring space or a kick.
- **GPU-written memory and source-1 interrupts are FIFO-ordered behind the executor.** `EVENT_WRITE_SHD`/`MEM_WRITE` stores and `INTERRUPT` packets publish a batch immediately (`gpu_executor.cpp:204`). They take effect only when the Vulkan thread reaches them, and for interrupts after a handshake with the PM4 thread (`pm4.cpp:1306`). A guest fence wait therefore waits for Vulkan translation, not just parsing.
- **`WAIT_REG_MEM` on the PM4 thread.** It can block parsing until the guest, the vblank ISR or the executor writes the polled value.

Throughput is set by the slowest stage plus these waits. No mid-frame GPU sync was found: there is one `vkQueueSubmit` per frame, at present (`vk_presenter.cpp:12396`), and your GPU sits at 47%. So the GPU is not the limit.

### 2.2 Reading your 0.2.0 numbers

- `guest+PM4 26.65 ms` is computed as `frameMs - presentMs` on the Vulkan thread (`vk_presenter.cpp:14354`). It is the end-to-end frame interval, not time spent in the guest.
- `drawTotal 8.72 ms` (p90 16.24) is Vulkan-thread time. If that thread were the limit, its busy time would be close to 27 ms per frame. The cumulative `gpu-probe ... worker=…ms` values that 0.2.0 already logs every 60 frames show it, and `tools\summarize-runtime-log.bat` does the arithmetic (see [4.7](#47-phase-1-measurement-plan)).
- `drawOther 4.14 ms` was time spent before the attachment/texture/pipeline timers start. That is exactly where the two shader-module lookups and the per-draw `GetAsyncKeyState` were.
- The 0.2.0 logs cannot show the guest main thread's CPU time or the PM4 thread's share. The new `[pipeline]` line (below) shows both.

### 2.3 Per-draw cost: what changed

All of these run on the Vulkan thread and leave the lookup results identical.

| Cost per draw (0.2.0) | Change | Where |
| --- | --- | --- |
| `GetModule` scanned every translated shader module, twice per draw (VS + PS). | Hash index per stage; deque pointers are stable. | `vk_presenter.cpp:10017` |
| `GetPipeline` ran `std::find_if` over every pipeline, including all pipelines prewarmed from the on-disk manifest, so the cost grows with the hours played. | Hash index over the same key fields `SamePipelineKey` compares. | `vk_presenter.cpp:10251`, `PipelineKeyHash` |
| `AttributeLayoutHash(vs)` once per draw. | Computed once when the module's metadata is populated. The value is unchanged, so the pipeline manifest stays compatible. | `PopulateShaderModuleMetadata` |
| `TraceDrawFrame` called `GetAsyncKeyState(VK_F12)` (a win32k syscall) for every draw, resolve and dispatch, only to arm a debug trace. | The F12 trace key is polled once per presented frame. | `vk_presenter.cpp:917` `PollDrawTraceHotkey` |

Microbenchmark, `runtime/tests/draw_lookup_benchmark.cpp` (synthetic records with the presenter's layout; 2.1 GHz cloud Xeon):

| shader modules / pipelines | lookups per draw before | after | at 1300 draws/frame |
| --- | --- | --- | --- |
| 256 / 1024 | 1.12 µs | 0.05 µs | 1.46 → 0.06 ms |
| 512 / 2048 | 2.00 µs | 0.06 µs | 2.60 → 0.08 ms |
| 1024 / 4096 | 3.90 µs | 0.07 µs | 5.07 → 0.09 ms |
| 1536 / 8192 | 7.54 µs | 0.06 µs | 9.81 → 0.08 ms |

Your real counts are in `runtime.log` on the `Vulkan raster health: ... pipelines=N modules=M` line. A Ryzen 5 5600G runs this faster per element, but the scaling is the same. Run `MojoRecompDrawLookupBenchmark <modules> <pipelines>` on your PC for real numbers.

The GPU-poll syscall cost was not measured; a Windows syscall costs on the order of a microsecond with current mitigations. These fixes only raise FPS if the Vulkan thread is the limiting stage.

Remaining per-draw costs found but not changed (candidates once `[pipeline]` says the Vulkan thread limits):

- 4 KiB VS + 4 KiB PS constants copied per draw (`vk_presenter.cpp:11106`).
- A full index scan per indexed draw to find the maximum index (`:11600`).
- About 12 profiler clock reads per draw. These are on whenever a log folder is set (`vk_presenter.cpp:154`); `MOJORECOMP_DETAILED_CPU_PROFILE=0` turns them off, along with the `Vulkan CPU profile` line.

### 2.4 Snapshot write protection

The PM4 thread copies every range a draw reads into an immutable snapshot, so the Vulkan thread can render it later. To avoid re-copying unchanged data, it keeps a cache and write-protects the cached pages with `VirtualProtect` on all three physical alias views. A guest write to such a page takes an access violation into a vectored exception handler, which unprotects the page (`gpu/guest_memory_snapshot.cpp:138`). The next capture re-protects it (`:244`). Lowering a protection also costs a TLB shootdown that interrupts the other threads.

That is cheap for static meshes and textures. It is very expensive for data the guest rewrites every frame: dynamic vertices, UI and particles, and command memory if draw-UP vertex data lives there.

- **Measured in the cloud under Wine (not representative, Wine handles exceptions with Linux signals):** 11 µs per guest write fault and 8 µs per re-protect, against 0.01 µs to copy 256 bytes. Run `MojoRecompSnapshotFaultBenchmark` on Windows for real numbers.
- **New counters:** `[pipeline] ... snap ... faults=… protects=…` per frame. Multiply faults/frame by the benchmark's µs/fault to get guest time lost per frame.
- **Opt-in mitigation, `MOJORECOMP_SNAPSHOT_HOT_PAGES=1`:**
  - A page that faults twice within 8 swaps stays writable for 240 swaps.
  - Ranges of 256 KiB or less that touch it are copied fresh instead of cached.
  - Larger ranges (textures) keep the protected cache, which re-tracks the page.
  - A cached range is only ever served while all its pages are protected, so stale bytes cannot be returned.
  - Tested under Wine in `guest_memory_snapshot_hot_pages_test.cpp`.
  - It is off by default until it is measured on real hardware.
- **Phase 2:** command memory (Pure3D's immediate-mode vertices and D3D's resolve rectangles live there) is no longer write-protected at all, by default ([HFR-PHASE2 6](HFR-PHASE2.md#6-pm4snapshot-thread-command-memory)).

### 2.5 Guest-side candidates (documented, not changed)

- **Register localization is off.** `config/CrashOfTheTitans.toml:19824-19832` sets every `*_as_local` option to `false`. That keeps every PPC register in the `PPCContext` struct in memory, and every guest store forces reloads. XenonRecomp's README reports frame-time gains of "several milliseconds" from localization in Unleashed Recompiled. The maintained Xenon patch already adds a context-switch safety analysis for CR and non-volatile registers. The comment above those options says "localization stays aggressive", which does not match the values, so they were probably turned off later for a reason. **Experiment, local only:**
  - Set `ctr_as_local`, `xer_as_local` and `reserved_as_local` to `true`. The ABI never keeps these across calls, so this is the lowest-risk step.
  - Run `tools\recompile.bat` and `build-smoke.bat`, then play.
  - `cr_as_local`, `non_argument_as_local` and `non_volatile_as_local` are riskier: they depend on the context-switch analysis.
  - Expect crashes or subtle logic bugs if the analysis misses a path. Revert the lines if anything is off.
  - **Phase 2:** the low-risk step is committed as experimental, and the riskier options were analysed ([HFR-PHASE2 5](HFR-PHASE2.md#5-guest-cpu)).
- **Every indirect (virtual) call records a crash breadcrumb.** `tools/recompile.py:79` routes calls through `MojoRecompTraceIndirectCall`, which writes a thread-local record and checks a guarded static (`kernel/imports.cpp:73`). That is cheap per call, but virtual calls are frequent in this engine.

### 2.6 Are late frames quantized to vblank ticks?

The HFR60 gate does not quantize. If an update finishes late, the next gate check already sees `r28` ≥ 16.667 ms and releases immediately. The carry logic only keeps the sub-period overshoot, and drops it after a missed whole period (`host/frame_rate_policy.h`, `TitleFrameGateElapsedUsPhased`).

`tests/frame_rate_policy_test.cpp` simulates the title loop through the real gate:

- 25 ms of work per update: every interval is exactly 25 ms.
- Alternating 15/22 ms: each update is released as soon as it finishes.
- Fast work: holds 600 updates in 10 s.

The host side already presents as soon as a frame is ready: VSync off selects `IMMEDIATE`/`MAILBOX` (`vk_presenter.cpp:2263`). So 40/20 FPS plateaus would have to come from a **guest or PM4 wait tied to the 60 Hz vblank**. Candidates:

- a `WAIT_REG_MEM` on memory that the vblank ISR writes (a flip or "display done" flag). **Phase 2: this one**, D3D's present handshake with interval ONE ([HFR-PHASE2 4](HFR-PHASE2.md#4-the-4020-fps-plateaus));
- a guest event or fence signalled by the vblank ISR;
- the mode-1 progress path's reconciliation block.

Without the binary none of these can be confirmed. The new counters tell them apart:

- `[pipeline] gate ... ticks:` is the histogram of the title's frame-to-frame intervals, bucketed around whole 16.667 ms ticks. Mass in `1`/`2`/`3` with almost nothing in `1.5`/`2.5` means the title loop itself is quantized. A smooth spread means it is not, and the plateau is in presentation or measurement.
- `pm4 stall=…ms/…x vblankReleased=…x`: if most stall episodes are released by a vblank, the PM4 thread is waiting for the vblank ISR.
- `cpWait`: guest time spent waiting for the PM4 thread.
- `MOJORECOMP_WAIT_PROFILE=1` (already in 0.2.0) aggregates guest kernel waits by caller.

If vblank-released stalls do show up, the fix is a targeted title hook in the style of the existing `MojoRecompHfrSkipProgressWait`. Simulation speed is unaffected, because guest `dt` is real time. Delivering vblanks faster is **not** an option: the title counts vblanks for engine progress (`vd.cpp:125-128`).

### 2.7 The `[pipeline]` line

Every 120 guest swaps (`gpu/renderer_probe.cpp` `LogPipelineSummary`), all values per frame:

```
[pipeline] frames=120 fps=37.10 frame=26.95ms busiest=guest(91%) | guest cpu=24.50ms(91%) cpWait=0.20ms/1.0x |
  pm4 busy=9.10ms sink=6.00ms backpressure=0.10ms stall=0.40ms/2.0x vblankReleased=0.1x | vulkan worker=11.30ms |
  snap hit=92.0% miss=310.0 volatile=0.0 copied=1.20MiB faults=420.0 protects=150.0 |
  gate releases=120 ticks: <1=0% 1=10% 1.5=70% 2=20% 2.5=0% 3=0% >3=0%
```

(The real line is one line; the numbers are an example.)

- `busiest` is the stage with the largest busy share of wall time: `guest`, `pm4` or `vulkan`. Above about 85%, that stage is the limit.
- `guest cpu` is GetThreadTimes of the main guest thread. It includes the time spent handling snapshot write faults.
- `pm4 busy` is time inside `Pm4_Execute`, excluding executor backpressure. `sink` is the per-draw snapshot and enqueue part of it.
- If no stage is near 100%, the frame is limited by waits: see `cpWait`, `stall` and the tick histogram.

## 3. Phase 2: above 60 FPS

### 3.1 Evidence from the code

The guest's own timing was established by the original port and is encoded in comments next to the hooks. These are the parts that matter above 60:

| # | Fact | Source |
| --- | --- | --- |
| E1 | The title loop measures elapsed µs in `r28` and dispatches its update only when `r28 > 33333`. HFR60 doubles `r28` right before that compare. **Phase 2:** that gate only runs on loading and transition screens; gameplay calls `Game::Tick` every pass ([HFR-PHASE2 3.1](HFR-PHASE2.md#31-the-title-loop)). | `config/CrashOfTheTitans.toml:19887-19895` |
| E2 | "The downstream scheduler already uses a **1/60-second minimum** and receives the unmodified real delta in f1." | same, `:19888-19889`; `frame_rate_policy.h` |
| E3 | The mode-1 progress synchronizer paces frames by the 60 Hz graphics-interrupt counter. Its look-ahead turns that into 30 Hz; HFR cancels the look-ahead and the vblank wait. | `:19897-19915` |
| E4 | "Crash consumes source-0 as an engine progress tick". A 1 ms vblank made the game run about twice as fast. | `gpu/vd.cpp:125-128` |
| E5 | The XAudio render callback is "the cutscene audio/master clock". | `audio/xaudio.cpp:355` |
| E6 | Bink videos keep their authored cadence; HFR is suspended while Bink draws are seen. | `gpu/pm4.cpp:1065`, `frame_rate_policy.h` |
| E7 | The title is told the display runs at 60 Hz. | `gpu/vd.cpp:571` |
| E8 | An async streaming state machine busy-waits per update; HFR turns that into a poll. | `config/CrashOfTheTitans.toml:19918` |

Consequences:

- Releasing the outer gate above 60 Hz without touching E2 gives one of three outcomes, depending on how the floor is written. The update is skipped and time is lost (slow motion or a freeze); it is skipped with time accumulated (still 60 Hz); or `dt` is clamped up to 1/60 (the game runs too fast).
- E4 rules out raising the vblank rate.
- E5 and E6 keep cutscenes and videos on their own clocks, which is fine.

Unlike the Twinsanity decompilation, there is no source code here. A timing fix means hooking an exact instruction in the original binary, and the floor's address is not recorded anywhere in this repository.

### 3.2 Options and choice

- **(a) Raise the simulation rate.** The real thing: correct motion, no artifacts, the same approach the 60 FPS mode already uses. It needs the E2 floor lowered at its exact site, and that site can only be found with the game.
- **(b) Keep simulation at 60 and generate frames on the presentation side.** Xenos draws are not semantically tagged, so draw-level interpolation would need a frame of latency, matching of about 1,000 draws across frames and replaying the EDRAM/resolve chain. That's not feasible blind. Image-space optical flow produces HUD and disocclusion artifacts, and it can't be compiled or tuned here. An external tool such as Lossless Scaling (LSFG) does this today on a GTX 1080 Ti for borderless windows, at the cost of latency and artifacts. Use the 60 mode with it.
- **Chosen: (a) with a safety interlock.**
  - The new modes are complete and tested, but they behave **exactly like the 60 FPS mode** until the title's floor hook proves it has rewritten a genuine 1/60 s value.
  - That proof is a latch set only by `MojoRecompHfrSchedulerFloor`, and only when the register holds 1/60 to within 1e-6.
  - A wrong hook address is therefore inert: no slow motion, no double speed.

### 3.3 Design

- **Settings.** The launcher offers `120`, `144`, `165` and `uncapped`. The runtime also accepts any integer from 61 to 300 when run directly; for example, `162` keeps a 165 Hz G-Sync display inside its VRR range. `uncapped` uses a 300 Hz safety ceiling, so menus or loading screens can never hand the update a near-zero step. The launcher passes an unlocked choice as `MOJORECOMP_FRAME_RATE=60` plus `MOJORECOMP_FRAME_RATE_TARGET=<rate>`. A runtime that predates these modes therefore runs its 60 mode instead of falling back to 30.
- **30 and 60 are unchanged.** `tests/frame_rate_policy_test.cpp` carries a verbatim copy of the 0.2.0 policy. It compares every 30/60 result over 200,000 gate values and a million-step phased sequence, plus all the other hooks.
- **Gate.** It releases at exactly the target period on the guest's fixed `r28 > 33333` comparison. A pure scale lands on the wrong side at some rates: 144 Hz gives 6944 × 144 / 30 = 33331. The 60 Hz phase rules are kept: carry the sub-period overshoot, drop it after a whole missed period.
- **Floor.** 80% of the target period: 6.667 ms at 120 Hz, 5.556 ms at 144, 4.848 ms at 165. A step released by the gate is never refused or clamped up by a floor that sits on the period itself. Whichever form E2 takes (skip, accumulate or clamp), the game speed stays correct. **Phase 2:** E2 is an accumulator with no outer gate on the gameplay path, so the floor is now exactly the target period ([HFR-PHASE2 3.4](HFR-PHASE2.md#34-the-change)).
- **Unchanged above 60:**
  - the progress, vblank-wait, async-poll and GPU-poll hooks keep the 60 Hz contract;
  - the guest vblank stays 60 Hz (E4);
  - the display information stays 60 Hz (E7);
  - Bink suspends to the 30 FPS title policy, as in the 60 mode.

### 3.4 Systems at risk above 60

These could not be checked without the binary. Going from 30 to 60 already exercised dt-correctness, and you report the 60 mode playing correctly, so most systems are probably real-time or vblank based. Below 1/60 s steps, though, the title's own floor says the engine was never tested there.

- **Physics and collision:** smaller steps are usually more stable, but tuning, sleeping and contact thresholds may differ.
- **Per-update smoothing** (camera follow, UI easing): frame-rate dependent if written as `x += (target - x) * k`. At 120 the camera would feel snappier.
- **Frame-counted windows:** jump buffering, combo and parry timing, invulnerability frames. These would be shorter in real time.
- **Per-update spawners:** particles and debris would be denser.
- **Vblank-counted subsystems (E4):** they keep their real-time speed but still step at 60 Hz, which shows as judder in that subsystem only.
- **CPU load doubles.** At 2x resolution your CPU is not reaching 60 yet, so finish phase 1 first.

### 3.5 Finding the floor on your PC

**Phase 2:** found statically; the hook is in `config/CrashOfTheTitans.toml` (`0x821FC1C8`). The steps below remain useful as a cross-check ([HFR-PHASE2 7.7](HFR-PHASE2.md#77-optional-cross-check-with-the-discovery-tool)).

1. Run once with discovery enabled, in the 60 mode, from a level start (see [4.8](#48-unlocking-above-60)). The log gets:
   - `[hfr-discovery] scanned … KiB of guest code: N loads of 1/60, M loads of 60.0`, followed by up to 64 `image 1/60` loads with their function addresses;
   - for each HFR hook site (`frame-gate`, `progress-wait`, `progress-cadence`), the guest call chain and every 1/60 or 60.0 load inside the functions on that chain;
   - for each 1/60 load on a chain, a `toml|` block.
2. Pick the candidate that matches E2's description:
   - depth 1 or 2 on the `progress-wait` / `progress-cadence` chains, which is "the scheduler that called it";
   - its next use is an `fcmpu` against `f1` (the real delta), an `fsel` or an `fsub`.
   - Your local session (which can see the generated `ppc\*.cpp` and the binary) can confirm by reading that function.
3. Paste that block (without the `[hfr-discovery] toml| ` prefix) under the existing HFR hooks in `config\CrashOfTheTitans.toml`. Rebuild and run at 120. The log must say `[hfr] scheduler floor lifted: 16.667 ms -> 6.667 ms; title gate now 120 Hz`.
4. Interpret the result:
   - `MojoRecompHfrSchedulerFloor saw … instead of 1/60 s`: the hook is in the wrong place. It is inert; try the next candidate.
   - Floor lifted but FPS stays at 60: there is another limiter.
   - Game too fast: stop and report.

## 4. Build and test on Windows

### 4.1 Toolchain

These are the paths `setup.bat` and `build-smoke.bat` expect.

| Tool | Expected location / note |
| --- | --- |
| Git for Windows | on PATH |
| Visual Studio 2026 Community ("18") with **Desktop development with C++** and **C++ CMake tools for Windows** (provides Ninja) | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`. With VS 2022 instead, edit `VSVARS` in `setup.bat` and `build-smoke.bat` locally. |
| CMake 3.25 or later (standalone installer) | `C:\Program Files\CMake\bin\cmake.exe` |
| LLVM 22 for Windows x64 (the `clang+llvm-22.*-x86_64-pc-windows-msvc` archive) | extract so that `thirdparty\llvm22\bin\clang-cl.exe`, `thirdparty\llvm22\bin\lld-link.exe` and `thirdparty\llvm22\lib\clang\22\lib\windows\clang_rt.builtins-x86_64.lib` exist |
| Python 3 | `python` on PATH (used by `tools\recompile.bat` and the build) |
| Node.js and Rust | only if you also build the launcher (not needed to test) |

### 4.2 Get the source

```bat
git clone https://github.com/chronomitriy/MojoRecomp.git
```

```bat
cd MojoRecomp
```

```bat
git checkout hfr-unlimited
```

```bat
git submodule update --init --recursive
```

### 4.3 Provide your own default.xex

`config\CrashOfTheTitans.toml` reads `..\game\default.xex`, which is `game\default.xex` in the repository. The `game\` folder is git-ignored. Never commit it. `%USERPROFILE%\Games\MojoRecomp-Games\cot` below is the launcher's default library folder; use your own path if you moved the library.

```bat
mkdir game
```

```bat
copy "%USERPROFILE%\Games\MojoRecomp-Games\cot\default.xex" game\default.xex
```

### 4.4 Toolchain, recompile, build

```bat
setup.bat
```

```bat
tools\recompile.bat
```

```bat
build-smoke.bat
```

- `setup.bat` builds the patched XenonRecomp/XenosRecomp, FFmpeg, SDL and extract-xiso.
- `tools\recompile.bat` writes the translated C++ into `ppc\`. That folder is local and ignored.
- `build-smoke.bat` builds `runtime\build-smoke\cot-runtime.exe` (plus `mojorecomp-ffmpeg.dll` and `mojorecomp-lzx.dll`) and runs the smoke gate.
- `tools\analyze.bat` is not needed: the switch tables are already committed.

### 4.5 Run the custom runtime

The launcher's **Versions > Install Runtime from ZIP** verifies an ed25519 signature made with the official release key (`updates.rs:882`). It would reject a self-built ZIP. Overwriting the installed runtime's files would fail the launcher's hash check. Instead:

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
```

- It uses the same command line as the launcher (`--cpu --config … --game-root …`).
- It uses the same roots: settings and logs in `%LOCALAPPDATA%\MojoRecomp`, saves in the Saved Games known folder, read from the registry.
- It sets the same OBS-layer opt-out, uses the DXC from the XenosRecomp checkout, and writes the log to `%LOCALAPPDATA%\MojoRecomp\logs\cot\runtime.log`.
- The second argument is the frame rate. Display and graphics settings come from the launcher's `settings.toml`; change them in the launcher and press **Save**.
- Back up `%USERPROFILE%\Saved Games\MojoRecomp\cot` first. The custom runtime uses the same saves.
- To go back, just press **PLAY** in the launcher: the official runtime is untouched.
- The first run compiles pipelines into `runtime\build-smoke\cache\vulkan`, so expect some first-time stutter.

### 4.6 Optional: unit tests and benchmarks on Windows

Run these after `tools\recompile.bat`, because the CMake configure needs `ppc\`. First open a VS developer environment:

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
```

```bat
"C:\Program Files\CMake\bin\cmake.exe" -S runtime -B runtime\build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_C_COMPILER="%CD%\thirdparty\llvm22\bin\clang-cl.exe" -DCMAKE_CXX_COMPILER="%CD%\thirdparty\llvm22\bin\clang-cl.exe"
```

```bat
"C:\Program Files\CMake\bin\cmake.exe" --build runtime\build-tests --target MojoRecompFrameRatePolicyTest MojoRecompPipelineMetricsTest MojoRecompPpcConstantScanTest MojoRecompGuestMemorySnapshotTest MojoRecompGuestMemorySnapshotHotPagesTest MojoRecompDrawLookupBenchmark MojoRecompSnapshotFaultBenchmark
```

```bat
"C:\Program Files\CMake\bin\ctest.exe" --test-dir runtime\build-tests -R "FrameRatePolicy|PipelineMetrics|PpcConstantScan|GuestMemorySnapshot" --output-on-failure
```

```bat
runtime\build-tests\MojoRecompSnapshotFaultBenchmark.exe
```

```bat
runtime\build-tests\MojoRecompDrawLookupBenchmark.exe 1024 4096
```

For the benchmark, use the `pipelines=` and `modules=` counts from your `Vulkan raster health` log line.

### 4.7 Phase 1 measurement plan

1. Summarize your existing 0.2.0 log. If the Vulkan thread is busy for most of the frame, the per-draw fixes should show up directly. If it is mostly idle, the limit is upstream.

   ```bat
   tools\summarize-runtime-log.bat
   ```

2. Run the same scene and route twice, about 2 minutes each, in the 60 mode. Once with the official runtime from the launcher, once with `tools\run-custom-runtime.bat … 60`. Compare the `Vulkan perf` FPS with the summariser after each run, and read `busiest=` in the `[pipeline]` lines.
3. Repeat the custom run with hot pages. If `faults=` was large and the guest was the busiest stage, this should help.

   ```bat
   set MOJORECOMP_SNAPSHOT_HOT_PAGES=1
   ```

   ```bat
   tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
   ```

4. If the guest is still the limit, add these in a `cmd` window before running:

   ```bat
   set MOJORECOMP_GUEST_THREAD_TIME_PROFILE=1
   ```

   ```bat
   set MOJORECOMP_WAIT_PROFILE=1
   ```

   They log `[guest-cpu]` once a second and aggregate guest kernel waits by caller. Then try the register-localization experiment in [2.5](#25-guest-side-candidates-documented-not-changed).

### 4.8 Unlocking above 60

**Phase 2:** steps 1 and 2 are no longer needed; the floor hook is committed. Follow [HFR-PHASE2 7.6](HFR-PHASE2.md#76-run-2-above-60).

1. Run discovery from a level start. Play 20–30 seconds of gameplay, then quit.

   ```bat
   set MOJORECOMP_HFR_DISCOVERY=1
   ```

   ```bat
   tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
   ```

2. Pick the candidate (see [3.5](#35-finding-the-floor-on-your-pc)). Add its `[[midasm_hook]]` block to `config\CrashOfTheTitans.toml` after the `MojoRecompHfrYieldAsyncPoll` hooks, then rebuild:

   ```bat
   tools\recompile.bat
   ```

   ```bat
   build-smoke.bat
   ```

3. Test at 120 first, then 144, 165, `162` (for G-Sync headroom) and `uncapped`:

   ```bat
   set MOJORECOMP_HFR_DISCOVERY=
   ```

   ```bat
   tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 120
   ```

4. G-Sync: enable it in the NVIDIA Control Panel for windowed and full-screen mode, and use **Borderless fullscreen** in the launcher. Leave the launcher's VSync off and keep the cap a few FPS under 165, so `162` or `144`. With VSync off, frames at or above the refresh rate tear.

## 5. What to check in the logs

`%LOCALAPPDATA%\MojoRecomp\logs\cot\runtime.log`. Runtime lines from KLOG carry a `[kernel] ` prefix.

| Line | Meaning / expected |
| --- | --- |
| `cot-runtime: frame policy simulation=120Hz presentation=120Hz display=60Hz vblank=60Hz title_patches=1` | The requested policy. Display and vblank must always say 60. |
| `cot-runtime: unlocked frame rate requested=120Hz; the title runs the 60 FPS policy until …` | An unlocked rate was requested. |
| `[hfr] scheduler floor lifted: 16.667 ms -> 6.667 ms; title gate now 120 Hz` | The floor hook works; above-60 is active. |
| `[hfr] unlocked rate 120 Hz requested, but MojoRecompHfrSchedulerFloor has not run …` | No floor hook is configured; the title correctly stays at 60. |
| `[hfr] MojoRecompHfrSchedulerFloor saw … instead of 1/60 s …` | The hook is on the wrong instruction and stays inactive. |
| `graphics guest-vblank pump started (period=16.667 ms, independent of PM4)` | Must be 16.667 in every mode. |
| `graphics vblank perf: vblank=60.00 Hz …` (every second) | About 60.0, never 120 or more. |
| `Vulkan perf: X fps Y ms/frame …` (every 120 frames) | Presented FPS. |
| `Vulkan frame pacing: target=8.333 ms mean=… late(>1.5x)=a/b` | The target follows the effective rate (16.667 until the floor is lifted). |
| `[pipeline] …` (every 120 swaps) | The per-stage breakdown ([2.7](#27-the-pipeline-line)). |
| `PM4 Bink native cadence active/released at frame=…` | An FMV started or ended; the title policy drops to 30 during it. |
| `Vulkan raster health: … pipelines=N modules=M …` | Pipeline and shader counts for the lookup benchmark. |
| `[hfr-discovery] …` | Only with `MOJORECOMP_HFR_DISCOVERY=1` ([3.5](#35-finding-the-floor-on-your-pc)). |

`stutter.log` in the same folder logs one `[stutter] frame=… total=… cause=…` line per hitch, with the time split (pm4, draw, tex, pipe, resolve, fence, present). Compare how many hitches there are and their causes between 60 and the unlocked rates on the same route. A sustained rise in `pipe`/`newPipelines` after the first run would point at the pipeline index, which should not happen.

## 6. What was and was not verified

**Compiled and run in the cloud** (Linux x86-64, GCC 13 and Clang 18):

- `FrameRatePolicy`: 30/60 reference sweep, the late-frame quantization simulation, unlocked rates and the interlock.
- `PpcConstantScan`, using synthetic PPC encodings.
- `PipelineMetrics`, `GuestMemorySnapshot`, `GpuExecutor`.
- The other portable runtime tests, unchanged and passing: StutterProfiler, PresentationMetrics, HostTiming, RenderPolicy, TextureAbi, InputMerge, DebugOverlay, ShaderMetadata, RuntimeConfig, DialogueCatalog, RcfDiagnostics.

**Windows code run under Wine 9** (MinGW/Clang build):

- `GuestMemorySnapshot` with real vectored write faults on aliased views, and `GuestMemorySnapshotHotPages`.
- A smoke run of `MojoRecompSnapshotFaultBenchmark`.
- `tools\run-custom-runtime.bat` end to end, with a stand-in `cot-runtime.exe` that prints its arguments and environment.

**Compile-checked for Windows only** (Clang targeting MinGW, with stub PPC headers; not the real clang-cl/MSVC toolchain): `vk_presenter.cpp`, `pm4.cpp`, `vd.cpp`, `renderer_probe.cpp`, `guest_memory_snapshot.cpp`, `gpu_executor.cpp`, `draw_snapshot.cpp`, `title_hooks.cpp`, `title_hfr_discovery.cpp`, `boot_probe.cpp`, `debug_mode.cpp`, `pipeline_metrics.cpp`, `ppc_constant_scan.cpp`.

**Launcher:**

- `npm run check`: tsc and the 9 UI contract tests pass.
- The Vite build passes.
- `cargo test` on Linux: 99 pass. 3 storage and path-sanitisation tests fail, and they fail identically on the untouched v1.1.0 source (Windows path assumptions).

**Microbenchmark:** draw lookups on a 2.1 GHz cloud Xeon (table in [2.3](#23-per-draw-cost-what-changed)).

**Not done:**

- No PPC recompilation or link of `cot-runtime.exe`. Both need `default.xex`.
- No real Windows build.
- No run of the game. No FPS measurement of any fix.
- The discovery output and floor hook were not validated against the binary.
- No gameplay, audio or visual validation.
- Whether the floor exists in the form E2 describes, and whether the game is dt-correct below 1/60 s, are inferred from the existing hook comments, not verified.

## 7. Gameplay checklist

Compare every item against the 60 FPS mode on the same save. The fastest objective speed check is to film (or stopwatch) a fixed in-engine event in both modes and compare durations: a moving platform's cycle, a door or elevator, an in-engine cutscene. They must match within about 2%.

| Area | How | Pass |
| --- | --- | --- |
| Jump height and distance | Same ledge or gap, standing jump, running jump, double jump, slide-jump | Same reachability and arc as 60; no gap becomes easier or impossible |
| Movement speed | Run a straight corridor of known length | Same time as 60 |
| Titan combat timing | Mount a Titan, combos, block or parry windows, enemy telegraphs | Windows feel the same; no missed or doubled hits; enemy attacks not faster |
| Spin and combat (Crash) | Combo strings, stun meter, Mojo pickup magnet | Same timing and counts |
| Camera | Fast pans and follow | No new jitter; snappier than 60 is a known risk ([3.4](#34-systems-at-risk-above-60)) |
| Physics | Breakables, debris, rolling objects | No explosions, jitter or tunnelling |
| In-engine cutscenes | Episode intros | Lip sync and audio aligned for the whole scene |
| FMV (Bink) | Intro and episode videos | Plays at authored speed; log shows `Bink native cadence active` |
| Music and audio sync | Loops, footsteps and impacts | No drift, crackle or speed change |
| Menus and UI | Pause, episode select, settings, loading screens | Animations at normal speed; no lockups at `uncapped` |
| Level 1 | Full playthrough | Completable, no softlock |
| A late level | For example episodes 15–18 | Same as 60. Black screens there are a known 0.2.0 issue (upstream issue #11), not a regression |
| Long session | 20–30 minutes | No growth in `stutter.log`; FPS stable |

## 8. Environment variables

| Variable | Effect |
| --- | --- |
| `MOJORECOMP_FRAME_RATE` | `30` (default), `60`, `120`, `144`, `165`, `uncapped`, or 61–300 |
| `MOJORECOMP_FRAME_RATE_TARGET` | The launcher's form: honoured only together with `MOJORECOMP_FRAME_RATE=60` |
| `MOJORECOMP_HFR_DISCOVERY=1` | Floor discovery output ([3.5](#35-finding-the-floor-on-your-pc)) |
| `MOJORECOMP_SNAPSHOT_HOT_PAGES=1` | Opt-in hot-page snapshots ([2.4](#24-snapshot-write-protection)) |
| `MOJORECOMP_GUEST_THREAD_TIME_PROFILE=1` | `[guest-cpu]` once a second (existing) |
| `MOJORECOMP_WAIT_PROFILE=1` | Guest kernel waits by caller (existing) |
| `MOJORECOMP_CP_TIME_PROFILE=1` | `graphics CP perf` once a second (existing) |
| `MOJORECOMP_DETAILED_CPU_PROFILE=0` | Turns off the per-draw profiler timers and the `Vulkan CPU profile` line (existing) |

Phase 2 adds `MOJORECOMP_HFR_GUEST_VSYNC` and `MOJORECOMP_SNAPSHOT_COMMAND_MEMORY` ([HFR-PHASE2 9](HFR-PHASE2.md#9-environment-variables)).
Phase 3 adds `MOJORECOMP_CP_TIME_SIGNALS`, `MOJORECOMP_PM4_FRAMES_AHEAD`,
`MOJORECOMP_PM4_PROMPT_WAKE` and `MOJORECOMP_VERTEX_FAST_PATH` ([HFR-PHASE3 10](HFR-PHASE3.md#10-environment-variables)).
