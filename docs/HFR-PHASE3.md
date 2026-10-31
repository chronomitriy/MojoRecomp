# HFR Phase 3: decoupling the swap handshake

Branch `hfr-phase3`, based on `hfr-unlimited` at `fd64c93` (phase 1 + phase 2).

This phase was done without the game, like phase 1: code reading, a model of the swap
handshake that runs the real PM4 parser and executor, and Linux and Wine tests. No game
data, no generated `ppc\` output and no disassembly were used or added. Nothing was run
against Crash of the Titans; [section 9](#9-what-was-and-was-not-verified) lists exactly
what was verified. Guest addresses come from [HFR-PHASE2.md](HFR-PHASE2.md), which had the
executable.

> **Phase 4 update.** [HFR-PHASE4.md](HFR-PHASE4.md) (branch `hfr-phase4`) adds a motion
> blur toggle, FSR 1 upscaling/sharpening, and GPU and guest CPU profilers, all off by
> default.

## Contents

1. [Summary](#1-summary)
2. [Where the slow frames go](#2-where-the-slow-frames-go)
3. [CP-time signals](#3-cp-time-signals)
4. [Prompt PM4 wake-ups](#4-prompt-pm4-wake-ups)
5. [The swap handshake model](#5-the-swap-handshake-model)
6. [Letting the guest run further ahead](#6-letting-the-guest-run-further-ahead)
7. [Vulkan thread: the vertex path](#7-vulkan-thread-the-vertex-path)
8. [Test on your PC](#8-test-on-your-pc)
9. [What was and was not verified](#9-what-was-and-was-not-verified)
10. [Environment variables](#10-environment-variables)
11. [Commits](#11-commits)

## 1. Summary

| # | Finding | Change | Default | Confidence |
| --- | --- | --- | --- | --- |
| 1 | **Every swap drains the Vulkan thread.** The async executor queues every GPU memory write (fences, `MEM_WRITE`, the scratch-register write-backs) and every CP interrupt behind the draws. D3D's swap handshake first waits until its own `SCRATCH_REG1 = 1` write-back is visible, and that write-back lands only after the Vulkan thread has translated the whole frame. The interrupt in step 2 then waits behind it too. | New `[pipeline]` counters split PM4 stalls by cause, the guest's `cpWait` by what the PM4 thread was doing, and report ring backlog, pump timeouts, signals and presents in flight. The summarizer prints medians of slow and full-rate windows and names the dominant wait. | always on (diagnostics) | High for the mechanism (code, model). Your log confirms it if `stallBy[swap]` ≈ `stall` and `cpBy[swap]` is most of `cpWait`. |
| 2 | Every value a GPU store writes is computed on the PM4 thread, and the Vulkan thread reads guest memory only through each draw's immutable snapshot. Nothing behind the PM4 thread can observe what the guest does after a fence. | **CP-time signals**: in HFR, GPU stores and CP interrupts take effect where the PM4 thread meets them, as on the synchronous executor and in Xenia. A gate never lets one overtake a queued one; the PM4 thread stays at most one present ahead of the Vulkan thread. | on in 60 and unlocked modes; never in 30 FPS or during Bink | High for guest-visible semantics (proof in [3.2](#32-safety-argument), model, mutation checks). Medium that the title has no timing quirk the sync executor would also hit. |
| 3 | The PM4 pump's "250 µs" retry is `condition_variable::wait_for`, which Windows rounds up to 1 ms and then to the process timer tick (15.6 ms unless raised). Nothing woke the pump when a deferred store landed or when the guest blocked on the read pointer. | **Prompt wake**: high-resolution timer for the pump's poll; the guest's CP poll, deferred stores and the vblank wake it. | on in 60 and unlocked modes | High (host scheduling only). The `idleWait` average shows what your PC really did. |
| 4 | The vertex path decoded each index with a branchy call, twice per indexed draw (once to find the vertex range, once to write the host buffer), cleared 6.6 KiB of tables per draw, and allocated a hash node per upload. | SSE2 index scan and decode; a stamped per-draw fetch table; flat per-frame upload caches. Bit-identical output (differential tests). | on (all modes; output identical) | High for correctness. 16-bit scan+decode of 3,000 indices: 24.8 → 1.0 µs on the cloud Xeon. |

What to expect in the 60 mode on level 1: the slow windows should mostly disappear.
In the slow windows you measured, the guest worked about 4 ms of its 20.8 ms (the rest was
`cpWait`), PM4 6.5 ms, and the Vulkan thread 7.9 ms. Together with the handshake and
wake-up delays, that is the 21 ms frame.
Overlapped, the slowest stage is about 7–8 ms, well inside 16.7 ms. The 60 mode stays
capped at 60 by the `Game::Tick` floor; the headroom is what 120/144/165 can use.

## 2. Where the slow frames go

### 2.1 What the Vulkan thread holds back

Phase 1 [2.1](HFR-UNLIMITED.md#21-how-a-frame-flows-through-the-runtime) lists the three
threads. With the asynchronous executor (the production default):

| Packet | What the guest sees | When, before this phase |
| --- | --- | --- |
| Type-0 write to `SCRATCH_REG0..7` with `SCRATCH_UMSK` set | write-back to `SCRATCH_ADDR + 4n` (`pm4.cpp` `WriteRegister` → `StoreGpuRaw`) | when the Vulkan thread executes the queued `GuestStoreCommand` |
| `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE`, `EVENT_WRITE`, `EVENT_WRITE_SHD`, `EVENT_WRITE_EXT`, `EVENT_WRITE_ZPD` | the store (`StoreGpu`) | the same |
| `INTERRUPT` | the guest's source-1 ISR on the PM4 thread | when the Vulkan thread reaches the `InterruptCommand`, then a handshake back to the PM4 thread (`Pm4_RendererInterruptHandshake`) |
| CP read pointer | write-back slot | when the PM4 thread has parsed the packet (CP time already) |
| Draws, resolves, presents | nothing guest-visible | Vulkan thread, in order |

Stores and interrupts publish their batch at once (`AsynchronousGpuExecutor::Submit`), but
the worker runs batches in order, so each one waits for every draw queued before it.

### 2.2 The swap handshake under the async executor

D3D's handshake before each swap ([HFR-PHASE2 4.2](HFR-PHASE2.md#42-what-d3d-does-with-interval-one)),
traced through the code above:

1. `SCRATCH_REG1 = 1`, then `WAIT_REG_MEM` until its write-back reads 1. The write-back
   is queued behind the frame's draws, so **the PM4 thread stalls until the Vulkan thread
   has translated the whole frame**. Nothing wakes the pump when it lands (see 2.3).
2. `SCRATCH_REG4/5` (callback `0x82273A40`, parameter), `SCRATCH_REG0 = 4`, `INTERRUPT`,
   `WAIT_REG_MEM` until the handler clears `SCRATCH_REG0`. Those stores and the interrupt
   are queued too. With `REG0 = 4` not yet written back, this wait can pass at once on the
   value the previous frame's handler left; step 3 then waits instead.
3. `WAIT_REG_MEM` until `SCRATCH_REG1` reads 0. The callback writes that 0 when it flips
   immediately (phase 2's `MojoRecompHfrPresentImmediate`), but only once the worker has
   reached the interrupt and the handshake has run the ISR on the PM4 thread.

Only then does the `XE_SWAP` follow and the PM4 thread move on to the next frame. While it
waits, its read pointer stays put, the guest fills the ring and the command-buffer
segments it may reuse, and then spins in `MojoRecompGpuPollBackoff`. The three threads
run one after another instead of side by side:

```
guest   [frame N+1 .....]  waits for ring ............  [frame N+2 ...
PM4         [parse N] stall: Vulkan translates N ... ISR  [parse N+1] stall ...
Vulkan              [translate N ...............]                  [translate N+1
```

Phase 2 4.5 had predicted short stalls here; in heavy scenes they are most of the frame.

### 2.3 The pump's 250 µs poll on Windows

`vd.cpp` `PumpIdleWait` waited with `std::condition_variable::wait_for(250 µs)`. The MSVC
STL passes the timeout to `SleepConditionVariableSRW` in whole milliseconds, rounded up,
and that wait then ends on a timer tick. Since Windows 10 2004 the tick is per process:
15.6 ms unless the process raises its timer resolution. The runtime never calls
`timeBeginPeriod`; whether a library it loads does is unknown. Only an interrupt handshake
woke the pump early. So:

- A step-1 stall ended up to a tick after the store had landed. This fits your numbers:
  `stall` 10.6 ms against a Vulkan frame of 7.9 ms, and `vblankReleased` 0.4 per frame. A
  stall counts as vblank-released when a vblank falls between the last failing poll and
  the passing one. With 250 µs between polls that happens to about 1.5% of the stalls;
  0.4 per frame points to gaps of milliseconds.
- When the guest kicked new work while the pump slept, the pump noticed only at its next
  timeout.

The new `idleWait` field is the average length of pump waits that timed out, so your log
will show the real value: about 0.25 ms means the timer is fine, 1–2 ms means 1 ms
resolution, and anything near 8–15 ms means the default tick.

### 2.4 The new `[pipeline]` fields

A new section goes before `gate releases`, which the summarizer expects last. Example,
on one line in the log:

```
| waits cpBy[idle=0.40 parse=4.10 swap=11.90 gpu=0.00 other=0.10 bp=0.00 lag=0.00] polls=95000
  stallBy[swap=9.90 gpu=0.10 vblank=0.40 other=0.20] lag=0.00ms/0.0x idleWait=6.100ms/2.1x
  ring avg=21.3KiB max=60.2KiB of 64KiB signals cpTime=0.0 deferred=11.0 inflight=0.00 |
```

| Field | Meaning |
| --- | --- |
| `cpBy[...]` | The guest's `cpWait` per frame (ms), split by what the PM4 thread was doing at each poll: `idle` (waiting for work), `parse` (executing, including snapshots and the ISR), `swap` / `gpu` / `other` (stalled on a `WAIT_REG_MEM` of that class), `bp` (executor queue full), `lag` (holding a swap for the Vulkan thread, [3.4](#34-how-far-ahead-the-guest-may-run)). |
| `polls` | Iterations of the guest's CP-poll loop per frame. |
| `stallBy[...]` | PM4 `WAIT_REG_MEM` stall time per frame by cause: `swap` polled a scratch write-back slot (the D3D handshake); `gpu` polled memory an earlier GPU store wrote (fences); `vblank` was released by a guest vblank (whatever it polled); `other` is registers and CPU-written memory. They add up to `stall`. |
| `lag=ms/x` | Time and count of swaps held because the Vulkan thread still owed an earlier present. Excluded from `pm4 busy`, like backpressure. |
| `idleWait=ms/x` | Average length and count per frame of PM4 pump waits that timed out (2.3). |
| `ring avg/max` | Time-weighted average and largest ring backlog (dwords the guest published that PM4 had not parsed), and the ring size. |
| `signals` | GPU stores and CP interrupts per frame that took effect at CP time or were queued behind the Vulkan thread. |
| `inflight` | Presents the Vulkan thread still owed at each swap, averaged: how many frames it lags the PM4 thread. |

`tools\summarize-runtime-log.bat` prints all of them, the medians of slow windows
(fps < 50) and of full-rate windows (fps ≥ 55), and which part of `cpWait` and of `stall`
dominates each class.

## 3. CP-time signals

### 3.1 The change

`gpu/renderer_probe.cpp`:

- `OnStore`: when allowed, the store is written to guest memory on the PM4 thread, where
  the command processor reaches it, instead of being queued as a `GuestStoreCommand`.
- `OnInterruptCommand` now returns whether it queued the interrupt; when allowed it does
  not, and `pm4.cpp` delivers the ISR right at the `INTERRUPT` packet through the same
  interrupt sink as the synchronous executor.
- Allowed means (`CpTimeSignalsActive`, `gpu/cp_time_signals.h`):
  - async executor in use;
  - `MOJORECOMP_CP_TIME_SIGNALS` is not `0`;
  - an HFR title policy (60 or unlocked) and no Bink video, the same condition as phase 2's
    `TitlePresentImmediate` (`CommandProcessorTimeSignals`);
  - every store and interrupt queued earlier has completed (the gate, [3.3](#33-ordering-when-the-policy-changes)).
- `TrackPresent`: after enqueueing a present, the PM4 thread waits until at most
  `MOJORECOMP_PM4_FRAMES_AHEAD` (default 1) presents are outstanding.

With it, the handshake of 2.2 passes in one go: the write-backs are in memory before each
`WAIT_REG_MEM` polls them, the ISR runs at the `INTERRUPT` and its callback flips at once,
and the `XE_SWAP` follows. The host present stays in order on the Vulkan thread.

### 3.2 Safety argument

What the guest can see is when a value appears, never which value:

1. **The values are the same.** Every store's value is computed on the PM4 thread when it
   parses the packet: packet bodies (`MEM_WRITE`, `EVENT_WRITE*`, `COND_WRITE`), the PM4
   register file (`REG_TO_MEM`, scratch write-backs), the CP counter (`EVENT_WRITE_SHD`
   with bit 31) or constants (`EVENT_WRITE_EXT`). The queued path carries that value in the
   `GuestStoreCommand`; the executor's backend only copies it into memory. No store depends
   on rendering results; occlusion-query writes are immediates too.
2. **Nothing behind the PM4 thread reads guest memory.** On the Vulkan thread:
   - Draws read guest memory only through `GuestReadPtr` while the draw's snapshot is
     active (`ExecuteBackend` sets the thread-local snapshot). A range missing from the
     snapshot reads zeros and marks the draw failed; it never falls back to live memory.
   - Snapshots are immutable. A cached range is a shared, read-only copy; a guest write to
     a tracked page faults, bumps the page generation and makes the next capture copy
     again (`SnapshotWriteFaultHandler`).
   - `VkPresenter_Present`, resize, store and interrupt commands read no guest memory.
   - Shader microcode is hashed and translated on the PM4 thread at `IM_LOAD`
     (`BindShader` → `ShaderCache_OnBind`), not later.

   So once the PM4 thread has passed a packet, no later work depends on the guest memory
   that packet's fence would release.
3. **There is no GPU-written guest data to arrive late.** `vk_presenter.cpp` never writes
   guest memory. Resolves stay in host images; the readback paths are diagnostics into a
   host staging buffer. A title that read a resolve target with the CPU would see the same
   untouched guest memory in either mode, so early fences cannot expose a stale render
   target that the queued path would have filled in.
4. **Buffer recycling.** With CP-time fences the guest may rewrite a vertex buffer while
   the Vulkan thread is still translating a draw that used it. That draw renders its
   snapshot. The model ([5](#5-the-swap-handshake-model)) checks exactly this. It also shows
   the hazard is real: let the worker read live memory and draws see the next frame's bytes.
   Under Wine, with the real aliased views and write-fault handler, snapshots still hold.
5. **The swap callback.** Per phase 2 4.2, the ISR reads `SCRATCH_REG0` (4 = CPU callback)
   and `REG4/REG5` (callback, parameter); callback `0x82273A40` reads D3D's vblank count
   (written by the vblank ISR) and its last target, writes `SCRATCH_REG1 = 0` when the flip
   is due now, and the ISR clears `REG0`. All of these are register write-backs or
   CPU-side state; none means "rendering finished". CP-time delivery needs those three
   write-backs visible before the ISR runs, which CP-time stores and the gate guarantee.
6. **Precedent.** This is the timing of the synchronous executor
   (`MOJORECOMP_GPU_EXECUTOR_ASYNC=0`, the model before the async executor), where draws,
   stores and interrupts all happen at CP time. Xenia's command processor also performs
   these stores and dispatches CP interrupts as it parses the packets. On the console,
   scratch write-backs and `INTERRUPT` are command-processor events as well; only
   end-of-pipe fences complete later there, and point 2 is why that does not matter here.

### 3.3 Ordering when the policy changes

The policy changes when Bink starts or stops (the flag can flip mid-frame when a Bink draw
appears). Two hazards, and the rule that removes both:

- A CP-time store to an address with a queued store still pending would land first, and
  the queued one would then overwrite it. A fence could go backwards.
- A CP-time interrupt with queued stores still pending would run the ISR before the
  write-backs it reads.

`CpTimeSignalGate` opens only when the executor's completed sequence has reached the last
queued signal. Going from CP time to queued needs no care: queued signals land later than
earlier CP-time ones anyway.

### 3.4 How far ahead the guest may run

Before, the handshake bounded run-ahead to the frame being translated. Now:

- after the PM4 thread enqueues present N it waits until present N−1 is complete
  (`MOJORECOMP_PM4_FRAMES_AHEAD=1`), so the PM4 thread, and through the ring the guest, is
  at most one frame ahead of the Vulkan thread;
- the executor queue (32 batches) and the presenter's GPU frames in flight are unchanged.

Latency from input to display is therefore at most one frame longer than a fully
serialized frame. Frames also get shorter, so in milliseconds it changes little. `2` or
`3` allow more slack for uneven frames at the cost of latency.

### 3.5 Confidence and residual risks

- Guest-visible values and ordering: high (3.2, the model, the mutation checks).
- What cannot be checked here: a title routine that behaves differently when fences and
  flips complete sooner. The synchronous executor has the same timing. If anything
  misbehaves (a hang at a swap, missing geometry, flicker on reused buffers), rerun with
  `MOJORECOMP_CP_TIME_SIGNALS=0` and report it.
- 30 FPS and Bink: unchanged. Every store and interrupt is queued as before, there is no
  present bound, and the pump uses the condition-variable wait.

## 4. Prompt PM4 wake-ups

In the 60 and unlocked modes (not during Bink):

- `host::WakeableTimedWait` (`host/timing.cpp`) waits on an auto-reset event plus a
  high-resolution waitable timer, the timer type the vblank pump already uses. The pump's
  idle poll uses it instead of the condition variable.
- These wake the pump:
  - `MojoRecompGpuPollBackoff`, when the guest starts waiting for the read pointer and
    every 50 µs while it waits;
  - the executor backend after a queued store lands (relevant with
    `MOJORECOMP_CP_TIME_SIGNALS=0` and during policy transitions);
  - the vblank pump after each source-0 ISR, for vblank-released waits.

Waking only shortens a sleep; the pump then re-polls the same state, so nothing
guest-visible changes. `MOJORECOMP_PM4_PROMPT_WAKE=0` restores the old wait.

## 5. The swap handshake model

`runtime/tests/swap_handshake_test.cpp` (`MojoRecompSwapHandshakeTest`) is the phase 3
counterpart of phase 2's `SimulatePresentPath`, but it runs the real code:

- `pm4.cpp`, `AsynchronousGpuExecutor`, `GuestMemorySnapshot` and the shared gate;
- a guest thread writes frames into a ring: draws reading a double-buffered vertex buffer,
  a `MEM_WRITE` fence, and the handshake of 2.2 packet by packet, then `XE_SWAP`. It reuses
  a buffer once the fence of the frame that last drew from it is visible;
- a pump thread runs `Pm4_Execute` like `vd.cpp`; the ISR models D3D's handler and the
  immediate callback; the executor's worker has a fixed cost per draw;
- on Windows the guest memory is three aliased views, so snapshots use the real
  write-protection path.

It checks that:

- the ISR always finds `REG0 = 4` and that frame's `REG4/REG5`;
- stores apply in stream order per address, and no fence goes back;
- every draw renders its own frame's bytes;
- presents arrive in order;
- with CP-time signals, no more presents are outstanding than the bound allows;
- the queued ordering stalls in the handshake and the CP-time one does not and overlaps.

A flipping run toggles the policy at random packets inside the handshake, with a slow
store path, to stress transitions.

Results, stage costs guest 1.0 ms, PM4 1.5 ms, worker 2.5 ms per frame:

| Run | Frame | PM4 swap stall | Signals cpTime/queued |
| --- | --- | --- | --- |
| phase 2: queued, 1 ms pump timeout, no wakes | 5.1–5.3 ms (Linux), 6.0 ms (Wine) | 3.4–3.6 ms/frame | 0 / 360 |
| queued + prompt wake | 4.4–4.8 ms (Linux), 5.0–5.2 ms (Wine) | 2.7–3.1 ms/frame | 0 / 360 |
| **CP-time** | **2.6–2.8 ms (Linux), 2.7–2.9 ms (Wine)** | **0** | 360 / 0 |

Serialized, the frame is the sum of the stages; with CP-time signals it is the slowest
stage, the worker's 2.5 ms. That is the same signature as your level 1 windows.

Mutation checks (`--mutate=<name>`; each must make the run fail). On Linux the normal run
passed 10 of 10 times and each mutation failed 10 of 10 times; under Wine each failed too:

| Mutation | Caught by |
| --- | --- |
| `no-drain-guard`: CP-time signals may overtake queued ones | ~60 ISR, ~60 ordering and ~9 fence-regression violations per run |
| `hybrid-interrupt`: interrupts at CP time, stores still queued | the ISR finds stale write-backs in every frame |
| `live-reads`: the worker reads live guest memory, not the snapshot | ~1,800 draws render the next frame's bytes |
| `no-lag-bound`: swaps never wait for an earlier present | 33 presents outstanding with a bound of 1 |

## 6. Letting the guest run further ahead

Your task 3 suggested advancing the read pointer as soon as the PM4 thread has copied
commands out of the ring. Not done:

- With CP-time signals the read pointer already advances at parse speed; the only waits
  left are real ones (CPU-written memory, vblank in the guest-vsync A/B mode) and the
  present bound.
- D3D reuses command-buffer segments once the read pointer has passed the
  `INDIRECT_BUFFER` that points at them. An early read pointer would therefore require
  copying every indirect buffer, megabytes per frame, before parsing it. It only pays if
  PM4 parsing becomes the limit. `cpBy[parse]` now shows when it does.

## 7. Vulkan thread: the vertex path

Your profile had `vertex` 2.4 ms of `drawTotal` 4.6 ms. Per draw, `PrepareVertexBindings`:

1. **Decoded every index twice, one call per index.** First to find the largest vertex,
   then again to write the host index buffer; `ReadIndexValue` branches on width and
   endian for each index, and 32-bit indices went through `CopySwapped` one at a time. The
   scan also ran when the upload itself came from the frame cache, so every repeat of a
   draw with the same buffers (EDRAM tiles, extra passes) paid it again. Now
   `gpu/index_scan.h`:
   - `ScanGuestIndices` takes the minimum and maximum with SSE2 (unsigned compares via a
     sign bias), skipping the restart index;
   - with no index offset, Xenos' remap is a clamp, which is monotonic, so the minimum and
     maximum give the largest remapped index and whether any index changes. With an
     offset, or a 32-bit index above the 24-bit mask, it falls back to the loop;
   - `DecodeGuestIndices` writes the host buffer with SSE2, including the 16→32-bit
     widening that restart and remap need.
2. **Cleared 6.6 KiB per draw**: a 96-entry fetch table and two 128-entry binding arrays.
   The fetch table now carries a per-draw stamp; the binding arrays need no clearing.
3. **Allocated a hash node per upload.** The per-frame vertex and index upload caches were
   `std::unordered_map`s, emptied every frame. `gpu/frame_hash_map.h` is a flat table that
   empties by advancing a generation.

Measurements (cloud Xeon 2.8 GHz, GCC 13 `-O2`; `MojoRecompVertexPathTest --bench`):

| Work | Before | After |
| --- | --- | --- |
| 3,000 16-bit indices, scan + decode, endian 0 / 1 / 2 | 24.8 / 24.2 / 25.4 µs | 1.0 / 1.3 / 1.4 µs |
| 3,000 32-bit indices, endian 0 / 1 / 2 | 45.3 / 41.8 / 35.1 µs | 7.4 / 3.2 / 3.7 µs |
| 6,000 upload-cache lookups and inserts per frame | 508 µs (Linux), 782 µs (Wine) | 53 / 59 µs |

`vertex_path_test.cpp` compares against verbatim copies of `CopySwapped`,
`ReadIndexValue` and the loops: 14,080 cases (every endian mode, both widths, restart,
offsets, clamps including min > max), 6,771 of them through the fast bounds, with each
source ending at an inaccessible page. Six mutations of `index_scan.h` and four of
`frame_hash_map.h` each fail it. `MOJORECOMP_VERTEX_FAST_PATH=0` switches the index scan and
decode back for A/B timing. The stamp and the flat tables are refactors with identical
results and have no switch.

Not changed, candidates for the next round:

- **Constants**: `UploadConstants` copies 4 KiB of VS and 4 KiB of PS constants per draw
  (or clears 4 KiB when a stage reads none). At 3,000 draws that is 24 MiB per frame into
  the upload buffer. Read `const` in the `Vulkan CPU profile` line; if it is large,
  deduplicating unchanged blocks is the next step.
- **Textures**: source fingerprints already use the snapshot identity instead of hashing
  bytes. Without a `tex` figure from a heavy scene there was no evidence of a hot spot.
- **Vertex copies** start at vertex 0, not at the draw's lowest index. Trimming would need
  a non-zero `vertexOffset`, which changes `gl_VertexIndex` for translated shaders, so it
  was left alone.

## 8. Test on your PC

### 8.1 Get and build

The config did not change, so `tools\recompile.bat` is not needed.

```bat
cd /d C:\Users\ondrf\Desktop\RAND\MojoRecomp
```

```bat
git fetch origin
```

```bat
git checkout hfr-phase3
```

```bat
build-smoke.bat
```

### 8.2 Run 1: 60 mode, level 1

Play level 1 for at least 10 minutes, ideally your 19-minute route, then quit and summarize:

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
```

```bat
tools\summarize-runtime-log.bat
```

Compare with your fd64c93 numbers (slow windows: frame 21.2, cpWait 16.7, stall 10.6, Vulkan
7.9 ms). Fields marked "—" did not exist then; `MOJORECOMP_CP_TIME_SIGNALS=0`
([8.3](#83-ab-runs)) shows their old values on this build.

| Value | fd64c93 (slow windows) | Expected now | If not |
| --- | --- | --- | --- |
| `Vulkan perf` fps, level 1 | 56.1 average, 29% at 45–55 | Near 60 throughout; the summarizer's slow class nearly empty | See the next rows |
| `signals cpTime` / `deferred` | — (all queued) | `cpTime` > 0, `deferred` ≈ 0 in gameplay | `deferred` > 0 outside Bink: report it |
| `stallBy[swap]` | — (expected ≈ all of `stall`) | ≈ 0 | CP-time signals are not active: check the line above |
| `pm4 stall` | 10.6 ms | < 1 ms | Read `stallBy`: `gpu` or `other` would be a new kind of wait |
| `vblankReleased` | 0.4 | ≈ 0 | |
| `cpWait` | 16.7 ms | A few ms at most | Read `cpBy`: `parse` → PM4 parsing is now the limit; `lag` → the Vulkan thread is; `bp` → executor queue; `idle` → pump wake-ups |
| `inflight` | — (≈ 0) | 0–1 | Must never exceed 1 with the default bound |
| `lag` | — | Small; larger only where the Vulkan thread is the slowest stage | |
| `idleWait` | — | ≈ 0.25–0.5 ms | Much higher: the high-resolution timer is unavailable; report the value |
| `vulkan worker` | 7.9 ms | Lower by roughly the `vertex` saving (below) | |
| `Vulkan CPU profile: vertex` | 2.4 ms | A fraction of that | Compare with `MOJORECOMP_VERTEX_FAST_PATH=0` |
| `guest cpu` | 20.8 ms | Lower: less spinning | |
| `busiest` | guest (spinning) | Whichever stage really limits | |
| Gameplay and visuals | | Unchanged | [8.5](#85-what-to-watch-for) |

In the 60 mode the frame rate stays capped at 60; spare time shows as idle in all three
stages. To see the headroom, run 120 once:

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 120
```

### 8.3 A/B runs

Same build and route; set the variables of one row per run in the `cmd` window before
`tools\run-custom-runtime.bat`, and clear them afterwards with `set NAME=`.

| Isolates | Variable | Expected difference |
| --- | --- | --- |
| CP-time signals | `set MOJORECOMP_CP_TIME_SIGNALS=0` | `stallBy[swap]`, `cpWait` and the slow windows come back, a little lower than fd64c93 thanks to the prompt wake |
| Phase 2 pump behaviour as well | `set MOJORECOMP_CP_TIME_SIGNALS=0` and `set MOJORECOMP_PM4_PROMPT_WAKE=0` | Close to fd64c93 except the vertex path; `idleWait` shows your PC's real timer granularity |
| Index fast path | `set MOJORECOMP_VERTEX_FAST_PATH=0` | `vertex` in the `Vulkan CPU profile` line rises |
| More slack between PM4 and Vulkan | `set MOJORECOMP_PM4_FRAMES_AHEAD=2` | Possibly smoother uneven scenes, `inflight` up to 2, one frame more latency |

### 8.4 Unit tests (optional)

Configure as in [HFR-UNLIMITED.md 4.6](HFR-UNLIMITED.md#46-optional-unit-tests-and-benchmarks-on-windows),
then:

```bat
"C:\Program Files\CMake\bin\cmake.exe" --build runtime\build-tests --target MojoRecompSwapHandshakeTest MojoRecompVertexPathTest MojoRecompTimingTest MojoRecompPipelineMetricsTest MojoRecompFrameRatePolicyTest MojoRecompGpuExecutorTest MojoRecompPm4ExtentProbe
```

```bat
"C:\Program Files\CMake\bin\ctest.exe" --test-dir runtime\build-tests -R "SwapHandshake|VertexPath|HostTiming|PipelineMetrics|FrameRatePolicy|GpuExecutor|Pm4ExtentProbe" --output-on-failure
```

`HostTiming` prints the median of a 250 µs wait with the new timer and with the condition
variable on your PC. `runtime\build-tests\MojoRecompVertexPathTest.exe --bench` gives the
index timings on your CPU.

### 8.5 What to watch for

- Hangs at a scene change or right after a video. That would point at the transition
  between the two orderings ([3.3](#33-ordering-when-the-policy-changes)).
- Geometry flicker or garbage on dynamic objects such as particles, UI or skinned
  characters. That would be a draw reading data the snapshot did not capture.
- Bink videos: same as before (the log shows `Bink native cadence active`; signals are
  queued while it plays).
- Input latency compared with fd64c93: at most one frame more, see [3.4](#34-how-far-ahead-the-guest-may-run).
- The phase 2 [gameplay checklist](HFR-UNLIMITED.md#7-gameplay-checklist) for 120 and above.

For any of these, rerun with `MOJORECOMP_CP_TIME_SIGNALS=0`, then with
`MOJORECOMP_VERTEX_FAST_PATH=0`, and report which one changes it.

## 9. What was and was not verified

**Run in the cloud** (Linux x86-64, 4 vCPU Xeon 2.8 GHz):

- GCC 13 and Clang 18, all passing: StutterProfiler, FrameRatePolicy, PipelineMetrics,
  PpcConstantScan, HostTiming, PresentationMetrics, GpuExecutor, GuestMemorySnapshot,
  ShaderMetadata, TextureAbi, DebugOverlay, InputMerge, RuntimeConfig, RenderPolicy,
  DialogueCatalog, RcfDiagnostics, and the new SwapHandshake and VertexPath. Built from
  the source lists in `runtime/CMakeLists.txt`; a full CMake configure needs the libmspack
  checkout and `ppc\`.
- Wine 9, MinGW-w64 GCC 13 builds, all passing: GuestMemorySnapshot,
  GuestMemorySnapshotHotPages, GuestMemorySnapshotCommandMemory, Pm4ExtentProbe,
  GpuExecutor, PipelineMetrics, HostTiming, SwapHandshake (aliased views, real write
  faults), VertexPath (guard page via `VirtualProtect`), FrameRatePolicy. This MinGW lacks
  declarations for `SetThreadDescription`, `VirtualAlloc2` and `MapViewOfFile3`; a
  scratch force-include declared them and an import library was generated for
  `kernelbase.dll`. Nothing of that is in the repository.
- The mutation checks of sections 5 and 7.
- `tools/summarize-runtime-log.py` on a synthetic log whose `[pipeline]` lines come from
  the real `FormatPipelineSummary`.

**Compile-checked for Windows** (Clang 18 targeting `x86_64-w64-mingw32`, stub PPC headers
with this config's flags, the same scratch force-include plus `<cfloat>`): `pm4.cpp`,
`gpu_executor.cpp`, `renderer_probe.cpp`, `vd.cpp`, `title_hooks.cpp`, `vk_presenter.cpp`,
`pipeline_metrics.cpp`, `timing.cpp`. `vk_presenter.cpp` shows the same 214 warnings as
before the change.

**Not done:**

- No run of the game, no FPS measurement, no Windows machine, no clang-cl/MSVC build, no
  link of `cot-runtime.exe`.
- That the swap handshake dominates *your* slow frames, and the timer granularity on your
  PC, are predictions for 8.2; the counters exist to confirm or refute them.
- The model's costs are synthetic. It shows the mechanism, not your numbers.
- No gameplay, audio or visual validation.

## 10. Environment variables

New in this phase (phase 1: [HFR-UNLIMITED.md 8](HFR-UNLIMITED.md#8-environment-variables);
phase 2: [HFR-PHASE2.md 9](HFR-PHASE2.md#9-environment-variables)):

| Variable | Default | Effect |
| --- | --- | --- |
| `MOJORECOMP_CP_TIME_SIGNALS=0` | on in 60/unlocked, off in 30 and during Bink | Queue GPU stores and CP interrupts behind the Vulkan thread again (phase 2 ordering) |
| `MOJORECOMP_PM4_FRAMES_AHEAD=1..3` | 1 | With CP-time signals, presents the Vulkan thread may still owe after each swap |
| `MOJORECOMP_PM4_PROMPT_WAKE=0` | on in 60/unlocked, off in 30 and during Bink | Condition-variable pump wait, no wake-ups from the guest poll, stores or vblank |
| `MOJORECOMP_VERTEX_FAST_PATH=0` | on | Per-index scan and decode loops instead of SSE2 (identical output) |

## 11. Commits

All on `hfr-phase3`, in order, none touching `game\`, `ppc\` or `config\`:

| Commit | Content |
| --- | --- |
| `runtime: count PM4 stalls by cause and guest CP waits by PM4 state` | Sections 2.3–2.4: `[pipeline]` fields, PM4 stall classification, summarizer |
| `runtime: CP-time GPU signals and prompt PM4 wake-ups in HFR` | Sections 3–5: gate, present bound, wakeable timer, swap handshake model |
| `runtime: SSE2 index decode and flat upload caches on the Vulkan thread` | Section 7 |
| `docs: phase 3 findings, safety argument and test plan` | This file, pointers in the phase 1 and 2 documents |
