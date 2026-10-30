# HFR Phase 2: findings from the game executable

Phase 1 ([HFR-UNLIMITED.md](HFR-UNLIMITED.md)) was written without the game. This phase
had a copy of the Crash of the Titans `default.xex` (5,382,144 bytes, sha256 `e76824b2…6373`)
in a temporary private repository. The game could not be run there: no Windows, no
GPU, no game assets. Everything below comes from static analysis of the executable and
of XenonRecomp's translated C++, plus Linux and Wine tests.
[Section 8](#8-what-was-and-was-not-verified) lists exactly what was verified.

No game data is part of these commits. Guest addresses and a few instructions are quoted
so the reasoning can be checked against your own `ppc\` output.

## Contents

1. [Summary](#1-summary)
2. [Method](#2-method)
3. [The scheduler floor (E2)](#3-the-scheduler-floor-e2)
4. [The 40/20 FPS plateaus](#4-the-4020-fps-plateaus)
5. [Guest CPU: register localization](#5-guest-cpu-register-localization)
6. [PM4/snapshot thread: command memory](#6-pm4snapshot-thread-command-memory)
7. [Test on Windows](#7-test-on-windows)
8. [What was and was not verified](#8-what-was-and-was-not-verified)
9. [Environment variables](#9-environment-variables)
10. [Commits](#10-commits)

## 1. Summary

| # | Finding | Change | Confidence |
| --- | --- | --- | --- |
| 1 | **E2 is an accumulator.** Gameplay calls `Game::Tick` (`0x821FC198`) on every loop pass. It adds the real delta to `this+144` and returns while the sum is below 1/60 s, then hands the whole sum to the update. The `r28 > 33333` gate only runs while a state transition loads. | `MojoRecompHfrSchedulerFloor` after `lfs f13` at `0x821FC1C8`. The unlocked floor is now **exactly** the target period, not 80% of it. | High that this is the floor and how it works. Medium that the game is dt-correct below 1/60 s (untested). |
| 2 | **The plateaus come from D3D's vsync handshake.** In HFR the title asks for present interval ONE whenever exactly one vblank has passed. D3D then holds the command processor on a scratch register until the vblank interrupt flips, and the `XE_SWAP` that presents the frame comes after that wait. The PM4 thread and the host present therefore stop until the next 60 Hz guest vblank. | `MojoRecompHfrPresentImmediate` at `0x8238F060` keeps the immediate interval in HFR. | High that the wait exists and the hook removes it. Medium-high that it is what quantizes your FPS: confirm with `vblankReleased` and FPS. |
| 3 | `ctr`/`xer`/`reserved` localization is safe by ABI and by a control-flow liveness check over all 19,806 generated functions. `cr` and non-volatile localization also pass the checks, but change far more code. | **Experimental:** `ctr_as_local`, `xer_as_local`, `reserved_as_local` = true. | Medium: compiles, checked statically, never run. |
| 4 | Pure3D's immediate-mode primitives (via D3D `BeginVertices`) and D3D's resolve and clear rectangles keep their vertices **inside the command buffer**. Each such draw write-protected a command page that the next lap of the buffer rewrites, costing a guest write fault. | Command memory is never write-protected; small captures from it are copied fresh. Hot pages stay opt-in. | High for the mechanism. Unmeasured how many faults per frame it removes: read `faults=` and `cmd=`. |
| 5 | Stretch goal (native replacements of hot guest functions): not started in this pass. | — | — |

What to try first on your PC: the 60 mode with these commits, the same route as your
28-minute session. Compare `Vulkan perf` FPS, `[pipeline] vblankReleased` and
`snap faults` ([7.4](#74-run-1-60-mode-same-route-as-before)).

## 2. Method

- XenonRecomp (pinned commit with `patches/xenonrecomp-mojorecomp-runtime.patch`) was built
  for Linux and run on `default.xex` with `config/CrashOfTheTitans.toml`.
- **Build fix:** the committed Xenon patch had lost trailing whitespace on some context
  lines, so `git apply` (and therefore `tools\apply-xenon-patches.bat` in `setup.bat`)
  rejects it on a clean checkout. This branch regenerates it from the pinned commit. Only
  whitespace changes, and the result is byte-identical to what the old patch was made
  from ([10](#10-commits)).
- A small loader built on XenonUtils decrypted and decompressed the XEX into its mapped
  image and disassembled `.text` (with XenonRecomp's own PPC disassembler, which knows
  VMX128). It ran in a scratch directory and never left the machine.
- Phase 1's `cpu/ppc_constant_scan.cpp` was run over the real image: it finds 9 loads of
  1/60 and 25 of 60.0. Functions were read from the disassembly and from the generated
  C++. The pseudo-code below is a hand decompilation of both.
- No Ghidra: the generated C++ plus the disassembly was enough, and it is the code that
  actually runs.

## 3. The scheduler floor (E2)

### 3.1 The title loop

`main` (`0x821F9920`) calls the game singleton's loop `0x821FCFB8` (object `0x824A7A24`,
vtable `0x8201EC88`). Decompiled, inner loop only:

```c
for (;;) {                                   // 0x821FD064
    if (transition pending && !(flags & 0x40)) { finish it; continue outer; }
    if (flags & 0x08) return;                // quit
    now = TimeUs();                          // 0x82285748
    elapsedUs = now - timeBase;              // r28
    dt = float(elapsedUs) * 1e-6f;           // f31, constant 0x82019B30
    PerPassWork(dt);                         // input, loader pump, ... every pass
    if (!(flags & 0x40) && !subsystem17->entering /* +648 */) {
        timeBase = now;
        this->Tick(dt);                      // vtable +40 = 0x821FC198, every pass
    } else if (elapsedUs > 33333) {          // 0x821FD15C, MojoRecompHfrFrameGate
        timeBase = now;
        this->TransitionUpdate(dt);          // vtable +44 = 0x821FA398, 6 subsystems
    }
}
```

`flags & 0x40` (`this+152`) is set by the state handlers that start an asynchronous load
(`0x821FB0E0`, `0x821FB3F0`, `0x821FAF88`) and cleared when every subsystem reports ready
(`0x821FAE00`, `0x821FAF10`). Subsystem 17 (`this+96`) sets its `+648` byte when it enters
state 5 and clears it from its own tick once it is ready. So **the `r28` gate only runs on
loading and transition screens.** The comment above the frame-gate hook in the config said
it was the normal title loop; it is corrected in this branch. Consequences:

- The HFR60 frame-gate hook only speeds up loading screens.
- Phase 1's `[pipeline] gate ... ticks:` histogram was fed only by that gate, so it was
  empty during gameplay. The new floor hook feeds it from `Game::Tick`.

### 3.2 `Game::Tick`: how the floor works

From the generated C++ (`ppc_recomp.26.cpp`, `__imp__sub_821FC198`):

```c
void Game::Tick(float dt) {                  // 0x821FC198
    passes++;                                // this+320
    accum = float(accum + dt);               // this+144
    if (accum < 0.016666668f) return;        // lfs f13 <- [0x8201EF70]; fcmpu cr6,f0,f13; blt
    if (accum > 0.1f) accum = 0.1f;          // [0x82002168]
    BeginFrame();                            // 0x821F9C40 -> display Present 0x8238EFE8
    if (fabs(timeScale - 1.0f) > 1e-5f)      // 0x82233D70()->16
        accum *= timeScale;
    if (subsystem18->override)               // likely the cutscene clock
        accum = min(subsystem18->Step(accum), 0.1f);
    for (each of 19 subsystems in tick order) subsystem->Tick(accum);   // vtable +40
    accum = 0.0f;
    EndFrame();
}
```

The floor is the **accumulate** form. A step below 1/60 s is not skipped and not
clamped: it is carried in `this+144`, and the next update receives all of it. Lowering
the floor can therefore neither lose time nor speed the game up; it only changes how
often the update runs. On this path there is no other gate: in the 30 FPS mode the update
is paced by the present function's vblank wait (E3), and in HFR by this floor.

### 3.3 The other 1/60 constants

Of the 9 loads of 1/60 in the image, `0x821FC1C8` is the only one that sets a **minimum**.
The rest are maximum-step sub-steppers. For example, `0x82109938` and `0x82108748` split
`dt` into chunks of at most 1/60 s (above 1.1/60), and `0x82132970` is a damped spring
stepped in 1/60 s chunks that interpolates the remainder. Smaller `dt` simply means one
smaller chunk, so they are fine above 60 Hz. The 25 loads of 60.0 are mostly compared
against angles or distances (`fcmpu f31,60.0`); they were not all audited (see
[HFR-UNLIMITED.md 3.4](HFR-UNLIMITED.md#34-systems-at-risk-above-60)).

### 3.4 The change

```toml
[[midasm_hook]]
address = 0x821FC1C8
name = "MojoRecompHfrSchedulerFloor"
registers = ["f13", "f0"]
after_instruction = true
```

- The hook runs after `lfs f13,-4240(r11)` and before `fcmpu cr6,f0,f13`. `f13` holds
  `double(0.016666668f)`, which passes the interlock's 1e-6 check, so the latch trips at the
  first `Game::Tick` of an unlocked run.
- `MojoRecompHfrSchedulerFloor(floor, accumulated)` is a new two-register overload. It calls
  the phase 1 function, then records the dispatched interval for the `[pipeline]` tick
  histogram. The one-register form (what `MOJORECOMP_HFR_DISCOVERY` prints) still exists.
- **The unlocked floor is now exactly `1/rate`.** Phase 1 used 80% of the period, assuming an
  outer gate held the rate. On this path the floor *is* the gate, so 80% would have run the
  `120` setting at about 150 Hz and `162` at about 200 Hz, outside a 165 Hz VRR window. Each
  update is at least one period, and no time is lost.
- 30 and 60 FPS do not touch `f13`.
- Test (`FrameRatePolicy`): a model of the decompiled loop and `Game::Tick` checks, for
  every unlocked setting and three loop-pass costs, that:
  - the update rate stays at or below the target and above `1/(period + pass)`;
  - all real time reaches the updates;
  - no step is shorter than the floor;
  - the 60 FPS policy still ticks at 60 Hz.

  With the old 80% floor the test fails.

### 3.5 Confidence and alternatives

- **The floor: high.** The scanner's own selection rule (a 1/60 load on the `progress-wait`
  chain whose next use is an `fcmpu`) selects only this load. The chain is
  `0x8238EFE8 <- 0x821F9C40 <- 0x821FC198`, depth 2. You can cross-check with
  `MOJORECOMP_HFR_DISCOVERY=1` ([7.7](#77-optional-cross-check-with-the-discovery-tool)).
- **Game speed above 60: medium.** Each update gets the true elapsed time, but nothing
  below 1/60 s was ever tested by the developers. Run the
  [gameplay checklist](HFR-UNLIMITED.md#7-gameplay-checklist).
- **Alternative if something misbehaves above 60:** keep the hook and choose 120 (8.3 ms
  steps). The update order and the 0.1 s clamp are unchanged; only the step gets shorter.
  There is no other place to lower the rate.

## 4. The 40/20 FPS plateaus

### 4.1 The title's present path

`BeginFrame` calls the display object's vtable +60, `0x8238EFE8`, at the start of every
update. Mode 1 (`this+1660`) is the gameplay mode the phase 1 hooks target:

```c
counter = g_vblankCount;                     // [0x82526368]
interval = D3DPRESENT_INTERVAL_IMMEDIATE;    // r29 = 0x80000000
if (mode == 1) {
    while (counter - last < 1) { Sleep(0); counter = g_vblankCount; }  // MojoRecompHfrSkipProgressWait
    if (counter - last == 1) {
        last = counter + 1;                  // MojoRecompHfrProgressCadence removes the +1
        interval = 1;                        // li r29,1 at 0x8238F074
    } else
        last = counter;
} else if (mode == 0)
    interval = 1;                            // no wait loop, always vsynced
if (interval != published) SetRenderState_PresentInterval(dev, interval);  // dev+13444
D3D_PresentHandshake(dev);                   // 0x82274078 -> 0x82273BD8
D3DDevice_Swap(dev, frontBuffer[phase]);     // 0x82274080 -> VdSwap (XE_SWAP packet)
```

`[0x82526368]` is written by the title's vertical-blank callback `0x8238EE28`, which
D3D calls from its vblank handler with its own vblank count. In HFR the wait and the
look-ahead are already gone, but **any frame presented exactly one vblank after the
previous one still asks for interval ONE.** At 37 FPS that is about half the frames; at
a steady 60 it is every frame.

### 4.2 What D3D does with interval ONE

- **`0x82273BD8`** (the handshake before the swap):
  1. Writes `SCRATCH_REG1 = 1` and emits `WAIT_REG_MEM` (memory, ==1) on its write-back
     slot.
  2. Emits a CPU-callback block via `0x8226CA20`: `SCRATCH_REG4/5` = callback `0x82273A40`
     plus a parameter that carries the interval; `SCRATCH_REG0` = 4; `INTERRUPT`; then
     `WAIT_REG_MEM` until the interrupt handler clears `SCRATCH_REG0`.
  3. Emits `WAIT_REG_MEM` until `SCRATCH_REG1` reads **0**.
- **`0x82273A40`** (the callback, run from the source-1 interrupt) chooses the flip vblank.
  With `target = last + interval`, the current vblank count `vblanks = dev+16396`, and the
  immediate-present threshold of 0 (the default render state, never set by the title):
  - interval ONE: if `target <= vblanks`, then `target = vblanks + 1`. An on-time or late
    swap always goes to the **next** vblank.
  - immediate: if `target <= vblanks`, then `target = vblanks`, and the flip happens now.

  If the target is the current vblank, the callback writes `SCRATCH_REG1 = 0` itself.
  Otherwise it queues the flip.
- **`0x822738D0`** (the vblank handler, from source-0 interrupt `0x8226BF38`) runs each
  queued flip whose target has been reached, writes `SCRATCH_REG1 = 0`, and then calls the
  title's vblank callback.

So with interval ONE, the `WAIT_REG_MEM` in step 3 holds the command processor until the
next guest vblank. In MojoRecomp that is the PM4 thread (`gpu/pm4.cpp`, retried every
≤250 µs, counted as a stall released by a vblank). The `XE_SWAP` that makes the host
present follows it. Parsing and snapshotting the next frame's draws wait too.

### 4.3 Why that quantizes the frame rate

A model of exactly this logic (`SimulatePresentPath` in `tests/frame_rate_policy_test.cpp`)
gives, at 3 ms CP lag:

| Guest frame time | Frames held for a vblank (title's choice) | Average hold | With the hook |
| --- | --- | --- | --- |
| 16.7 ms (60) | 100% | 13.7 ms | 0 |
| 20 ms | 80% | 6.3 ms | 0 |
| 25 ms (40) | 50% | 13.8 ms | 0 |
| 27 ms | 38% | 8.0 ms | 0 |
| 30 ms | 20% | 13.7 ms | 0 |

Every held frame ends on a 16.7 ms boundary, and the PM4 thread loses that time from its
budget for the next frame. Once the PM4 thread plus the hold no longer fit in the frame
time, its cadence locks to whole vblanks. Alternating 1- and 2-tick frames average
1.5 ticks, which is **40 FPS**. Three ticks is **20 FPS**, and runs of 2- and 3-tick frames
give the 21–22 band. The model does not include how far the guest can run ahead (ring and
fences), so it shows the mechanism, not your exact numbers.

### 4.4 The change

```toml
[[midasm_hook]]
address = 0x8238F060
name = "MojoRecompHfrPresentImmediate"
registers = []
jump_address_on_true = 0x8238F078
```

- The hook sits on the `b 0x8238F074` that leads the "one vblank elapsed" branch to
  `li r29,1`. In HFR it jumps past that instruction, and `r29` keeps the immediate value it
  was loaded with on entry. The callback then flips at once, and the scratch wait passes in
  the same pass of the PM4 loop.
- Tearing and VSync are unaffected. The guest flip is pure bookkeeping in MojoRecomp, and
  the host presenter still applies your VSync setting (`IMMEDIATE`/`MAILBOX` with VSync off,
  as before).
- The 30 FPS mode and Bink playback (which drops to the 30 FPS title policy) are unchanged.
  So are the update rate (the 1/60 floor in the 60 mode) and game speed.
- `MOJORECOMP_HFR_GUEST_VSYNC=1` restores the title's interval, for an A/B run on the same
  build.
- `runtime.log` prints one line, `[hfr] present interval: immediate; ...`.
- Tests: the HFR-only contract (30, Bink, and unlocked before and after the latch), and
  the model above.

### 4.5 What can still couple the stages

- The `INTERRUPT` in step 2 is delivered only when the Vulkan thread reaches it (phase 1,
  2.1). Each swap therefore still waits for the Vulkan thread to translate the frame's
  draws. That wait is not tied to vblank and will show as short `stall` episodes that are
  not vblank-released. Delivering this interrupt early is not safe in general, so it is
  left as is.
- Mode 0 (interval ONE without the wait loop) is not hooked. Gameplay uses mode 1; if some
  menu uses mode 0, it stays vsynced at 60.
- If FPS still plateaus with `vblankReleased` near 0, the remaining limit is CPU time:
  `guest cpu` close to the frame time.

## 5. Guest CPU: register localization

### 5.1 What was run

The same XenonRecomp build, the same config, and four variants. Each variant's 79
generated translation units were compiled with Clang 18 for x86_64 MinGW, using the
runtime's flags (`-O2 -msse4.1 -mavx`, timebase force-include):

| Variant | Options set to true | Diff lines vs. current | Functions localized by the safety analysis | `.text` |
| --- | --- | --- | --- | --- |
| current | none | — | — | 13.37 MB |
| **A** | `ctr`, `xer`, `reserved` | 181k | all (not gated) | 12.97 MB (−3.0%) |
| B | A + `cr` | 198k | 7,199 of 19,879 | 12.79 MB (−4.3%) |
| C | B + `non_volatile` | 305k | 8,168 of 19,879 | 12.49 MB (−6.6%) |
| D | C + `non_argument` | 923k | 8,168 (r0/r2/r11/r12 not gated) | 11.76 MB (−12.0%) |

All variants compile. Code size is only a proxy, because nothing could be timed with the
game. The main saving in A: every integer compare copies `XER.SO` into the CR field, which
was a `PPCContext` load after each call and is now the constant 0. Carry chains (`srawi` +
`addze`) and CTR (one write per indirect call) also stay in host registers.

### 5.2 Safety checks

1. **Control-flow liveness.** For every generated function, a script checks whether a
   localized register (a zero-initialized C++ local) is read on any path before it is
   written. CR and XER are tracked per bit. Stack saves and `vxor v,v,v` do not count as
   reads. Results:
   - A: nothing.
   - B: nothing.
   - C: one function, `0x823E6384`. It is a data word between two functions, with no
     callers or references.
   - Everywhere: `XER.SO`, which no code writes except the `mtxer` below, so 0 in both
     builds.
   - D: 126 functions read `r12`, 3 read `r11` and 1 reads `r2` from their caller. These
     are MSVC exception funclets (`addi r31,r12,-320`) and helpers.
2. **Write side.** Some functions deliberately change registers for their caller:
   - `0x823E61FC`, the CRT's context restore for exception unwinding;
   - `0x823D2EA0`, `longjmp`, used by the embedded Lua's `luaD_throw` (`0x8229A588`) and one
     library error handler;
   - the exception funclets.

   Localization turns their register writes into dead locals. Their non-local jump already
   cannot work in a static recompilation: `blr` to a restored LR returns to the C++ caller
   instead. The runtime has no native `setjmp`/`longjmp`, and its `RtlRaiseException`
   stops with "needs SEH support" for anything but the SetThreadName exception
   (`kernel/imports.cpp`). Exceptions and Lua errors therefore do not work in 0.2.0 either,
   and they do not happen in normal play.
3. **Runtime code:** only a diagnostic print reads `ppc.ctr`. `preserve_local_context_storage`
   keeps the struct layout, and `title_hooks.cpp`, `guest_thread.cpp`, `guest_fiber.cpp`,
   `imports.cpp`, `memory.cpp`, `pm4.cpp` and `vd.cpp` compile against the new headers. No
   mid-asm hook takes `ctr`, `xer` or a CR field.

### 5.3 Recommendation

- **Committed, experimental: A.** CTR, XER and the reservation are volatile across calls in
  the Xenon ABI, so A cannot change what any function sees from its caller, and the checks
  agree. If anything regresses, set the three options back to `false` and rebuild.
- **Next local experiment: B, then C.** Both pass the same checks, and the patch's call-graph
  rule keeps them out of every function that can reach an indirect call, an import, a hook
  or the context switch. They change more of the generated code, though (B about 10% more
  lines than A, C about 70% more), and the original author shipped the analysis with the
  flags off ("enable these only after the new runtime reaches parity"). Try B alone first
  and play a long session before C.
- **Do not use D.** `non_argument_as_local` is not gated by the analysis and breaks the
  funclets and save helpers that receive `r11`/`r12` from their caller.

## 6. PM4/snapshot thread: command memory

### 6.1 Which draws read data the title rewrites every frame

- **Pure3D immediate-mode primitives.** `0x82391A28` maps primitive types 0–4 to D3D types
  and calls D3D `BeginVertices` (`0x823EA9A8`) or its sibling `0x823EAE48`. That reserves
  `count × stride` bytes with the command-buffer allocator `0x8226C308`, and the caller then
  writes its vertices there. Through `pddiPrimStream`, this is what text, HUD, sprites and
  similar effects most likely use.
- **Resolves and clears.** `0x8227F9E8` allocates six dwords with the same allocator
  (`bl 0x8226C308` with 6/4 at `0x822804D4`), writes the rectangle corners and draws with
  `DRAW_INDX_2`. `BuildDrawSnapshot` captures exactly these 24 bytes.
- **Command memory as a whole.** The allocator hands out space in the same segments the
  commands live in, so this data shares pages with the command stream. D3D rewrites those
  segments on every lap.

Each such capture went through the protected cache: `VirtualProtect` read-only on three
alias views, then one vectored write fault on the guest thread when the next lap rewrote
the page, plus three more `VirtualProtect` calls. That happened once per page per lap, for
every page holding primitive-stream vertices or a resolve rectangle. Static analysis cannot
count those pages per frame; phase 1 measured about 11 µs per fault under Wine (not
representative).

Other dynamic buffers in ordinary physical memory (for example skinned or particle vertex
buffers, if the title writes them each frame) could not be pinned down statically.

### 6.2 The change

- PM4 reports command memory through a new sink: the ring once, and every
  `INDIRECT_BUFFER` as it executes (`Pm4_SetCommandMemorySink`). The snapshot layer marks
  those pages (sticky).
- A capture of 256 KiB or less that touches a marked page is copied fresh: never cached,
  never protected. Larger ranges keep the protected cache, so the rule "a cached range is
  only served while all its pages are protected" still holds.
- On by default; `MOJORECOMP_SNAPSHOT_COMMAND_MEMORY=0` restores phase 1 behaviour.
- `[pipeline] ... snap ... cmd=` reports these captures per frame.
- Tests, built with MinGW and run under Wine 9 with real write faults on the aliased views:
  - the new `GuestMemorySnapshotCommandMemory`. Over four laps it sees fresh bytes, 0 faults
    and 0 protects; ordinary ranges are still cached and protected; a large range over
    command pages is still protected, and the pages are not re-protected afterwards. It fails
    with the policy turned off.
  - `GuestMemorySnapshot` and `GuestMemorySnapshotHotPages` still pass.
  - `Pm4ExtentProbe` checks the ring and IB reports.

### 6.3 `MOJORECOMP_SNAPSHOT_HOT_PAGES`: keep it opt-in

The deterministic part of the problem (command memory) is now handled without a heuristic.
Hot pages copy every capture of up to 256 KiB fresh, **per draw**. A single dynamic vertex
buffer of 200 KiB shared by 100 draws would copy 20 MiB per frame. Without a measurement
that is not a safe default. Decision rule after this build:

- If `snap faults` is still above about 50 per frame and `guest cpu` is the busiest stage,
  A/B `MOJORECOMP_SNAPSHOT_HOT_PAGES=1`.
- Keep it if FPS rises and `snap copied` stays below a few MiB per frame.

## 7. Test on Windows

These steps build on [HFR-UNLIMITED.md section 4](HFR-UNLIMITED.md#4-build-and-test-on-windows):
toolchain, `game\default.xex`, `tools\run-custom-runtime.bat`.

### 7.1 Get the commits

The commits exist only in the private repository. In your clone of the public fork, fetch
them, then cherry-pick everything after the temporary `default.xex` commit `f972ddd` onto
`hfr-unlimited`. That range is exactly the commits in [section 10](#10-commits); none of
them touches `game\` or `ppc\`.

```bat
git remote add hfr-private https://github.com/chronomitriy/MojoRecomp-hfr-private.git
```

```bat
git fetch hfr-private hfr-phase2
```

```bat
git checkout hfr-unlimited
```

```bat
git cherry-pick f972ddd..hfr-private/hfr-phase2
```

```bat
git remote remove hfr-private
```

Check that `git log --stat -8` shows no `game/` or `ppc/` path before you push.

The fetch also copies the temporary `default.xex` commit into your local object store. It
is not reachable from `hfr-unlimited`, so pushing that branch never sends it.

### 7.2 Rebuild

The config changed (two hooks and the localization flags), so the translated code must be
regenerated. If you have never built from source, or `setup.bat` stopped with "MojoRecomp
Xenon runtime patch conflicts with this checkout", run it first; the patch fix in this
branch makes it pass.

```bat
setup.bat
```

Then:

```bat
tools\recompile.bat
```

```bat
build-smoke.bat
```

### 7.3 Unit tests (optional)

Same configure command as in [HFR-UNLIMITED.md 4.6](HFR-UNLIMITED.md#46-optional-unit-tests-and-benchmarks-on-windows).
Then:

```bat
"C:\Program Files\CMake\bin\cmake.exe" --build runtime\build-tests --target MojoRecompFrameRatePolicyTest MojoRecompPipelineMetricsTest MojoRecompGuestMemorySnapshotTest MojoRecompGuestMemorySnapshotHotPagesTest MojoRecompGuestMemorySnapshotCommandMemoryTest MojoRecompPm4ExtentProbe MojoRecompSnapshotFaultBenchmark
```

```bat
"C:\Program Files\CMake\bin\ctest.exe" --test-dir runtime\build-tests -R "FrameRatePolicy|PipelineMetrics|GuestMemorySnapshot|Pm4ExtentProbe" --output-on-failure
```

```bat
runtime\build-tests\MojoRecompSnapshotFaultBenchmark.exe
```

The benchmark gives the real µs per write fault on your PC. Multiply it by the change in
`snap faults` per frame to get the guest time saved.

### 7.4 Run 1: 60 mode, same route as before

Play the same scene and route as your 28-minute session for at least 5 minutes, then quit
and summarize:

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
```

```bat
tools\summarize-runtime-log.bat
```

Compare against your phase 1 numbers (median 36.9 FPS, p10 29.6, plateaus at 40–41 and
21–22):

| Check | Expected |
| --- | --- |
| `[hfr] present interval: immediate; ...` in `runtime.log` | Present once |
| `[pipeline] vblankReleased` | Near 0 (it was how often a PM4 stall waited for a vblank) |
| `[pipeline] stall` | Lower; what remains is the Vulkan-drain handshake |
| `Vulkan perf` FPS | Higher median. The 40–41 and 21–22 plateaus should turn into a spread |
| Title update intervals (summarizer) | Now filled during gameplay. Intervals should spread over `1.5` and `2.5` too; mass only at `1`, `2`, `3` would mean a vblank wait remains |
| `snap cmd` | Above 0 (primitive-stream and resolve captures copied fresh) |
| `snap faults`, `snap protects` | Lower than with `MOJORECOMP_SNAPSHOT_COMMAND_MEMORY=0` |
| `guest cpu` | If it is now near the frame time, the remaining limit is guest CPU |
| Gameplay and speed | Unchanged ([checklist](HFR-UNLIMITED.md#7-gameplay-checklist)); timing is unchanged in the 60 mode |

### 7.5 A/B runs (same build, same route)

Set one variable per run in the `cmd` window, then start the game as in 7.4:

| Isolates | Variable |
| --- | --- |
| The plateau fix | `set MOJORECOMP_HFR_GUEST_VSYNC=1` |
| The command-memory snapshot change | `set MOJORECOMP_SNAPSHOT_COMMAND_MEMORY=0` |
| Hot pages on top | `set MOJORECOMP_SNAPSHOT_HOT_PAGES=1` |

Register localization cannot be toggled at run time. To isolate it, set the three
`*_as_local` lines in `config\CrashOfTheTitans.toml` back to `false`, then run
`tools\recompile.bat` and `build-smoke.bat`.

### 7.6 Run 2: above 60

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 120
```

| Check | Expected |
| --- | --- |
| `[hfr] scheduler floor lifted: 16.667 ms -> 8.333 ms; title gate now 120 Hz` | At the first gameplay or menu update after loading |
| `[hfr] MojoRecompHfrSchedulerFloor saw ... instead of 1/60 s` | Must not appear |
| `[hfr] unlocked rate ... has not run yet` | May appear during a long boot loading screen. If it appears and the floor is never lifted, report it |
| `Vulkan frame pacing: target=8.333 ms` | After the latch |
| Title update intervals | Mass in `<1` (under 12.5 ms) when the CPU keeps up |
| Game speed | Same as the 60 mode ([checklist](HFR-UNLIMITED.md#7-gameplay-checklist), speed check within 2%) |

Then try 144, 165, `162` (G-Sync headroom) and `uncapped`. The update rate is now capped
by the floor at the setting itself: `162` gives at most 162 updates per second.

### 7.7 Optional: cross-check with the discovery tool

```bat
set MOJORECOMP_HFR_DISCOVERY=1
```

```bat
tools\run-custom-runtime.bat "%USERPROFILE%\Games\MojoRecomp-Games\cot" 60
```

During gameplay the log should show `site=progress-wait ... in function 8238EFE8; callers:
... (fn 821F9C40) ... (fn 821FC198) ...`, and a `site=progress-wait depth=2 1/60` line with
`load=821FC1C8 lfs f13,...` whose next use is `fcmpu cr6,f0,f13`. The printed `toml|` block
names only `f13`. The committed block also passes `f0`, which only feeds the histogram.

## 8. What was and was not verified

**Run in the cloud:**

- XenonRecomp (patched, Linux build) on `default.xex` with the final config: all 19,806
  functions translated. Both new hooks are emitted at the intended instructions in the
  generated C++.
- All 79 generated units of the final config (= variant A) compile with Clang 18 for
  x86_64 MinGW with the runtime's flags. So do variants B, C and D.
- MinGW compile checks against the real generated headers: `title_hooks.cpp`,
  `title_hfr_discovery.cpp`, `guest_thread.cpp`, `guest_fiber.cpp`, `timebase.cpp`,
  `imports.cpp`, `memory.cpp`, `heap.cpp`, `vd.cpp`, `pm4.cpp`, `renderer_probe.cpp`,
  `guest_memory_snapshot.cpp`, `pipeline_metrics.cpp`, `runtime_state.cpp`. The hook symbols
  of the generated units match the definitions.
- Linux (GCC 13), all passing: FrameRatePolicy, PipelineMetrics, PpcConstantScan,
  StutterProfiler, HostTiming, PresentationMetrics, GpuExecutor, GuestMemorySnapshot,
  ShaderMetadata, TextureAbi, DebugOverlay, InputMerge, RuntimeConfig, RenderPolicy,
  DialogueCatalog, RcfDiagnostics. FrameRatePolicy also passes with Clang 18.
- Wine 9 (MinGW GCC build), all passing: GuestMemorySnapshot, GuestMemorySnapshotHotPages,
  GuestMemorySnapshotCommandMemory, Pm4ExtentProbe.
- Mutation checks: the 80% floor fails the new `Game::Tick` test, and disabling the
  command-memory policy fails its test.
- The static analyses in sections 3–6 (disassembly, generated C++, liveness scripts).

**Not done:**

- No run of the game, no FPS measurement, no Windows/MSVC/clang-cl build, no link of
  `cot-runtime.exe`.
- The plateau explanation is from code. That it explains *your* plateaus is a prediction
  for 7.4.
- dt-correctness of the game below 1/60 s is untested; there was no audit of every
  frame-count-based system.
- How many write faults the snapshot change removes per frame is unknown.
- No gameplay, audio or visual validation.

## 9. Environment variables

New in this phase (the phase 1 list is in [HFR-UNLIMITED.md 8](HFR-UNLIMITED.md#8-environment-variables)):

| Variable | Effect |
| --- | --- |
| `MOJORECOMP_HFR_GUEST_VSYNC=1` | Keep the title's own present interval in the HFR modes (undoes the plateau fix, for A/B runs) |
| `MOJORECOMP_SNAPSHOT_COMMAND_MEMORY=0` | Write-protect command memory again (phase 1 behaviour) |

## 10. Commits

All on `hfr-phase2`, in order, none touching `game\` or `ppc\`:

| Commit | Content |
| --- | --- |
| `runtime, config: hook the Game::Tick 1/60 s floor for unlocked rates` | E2 hook, exact-period floor, corrected frame-gate comment, Tick model test |
| `runtime, config: present immediately in HFR instead of waiting for vblank` | Plateau fix and tests |
| `config: EXPERIMENTAL ctr/xer/reserved register localization` | Variant A |
| `runtime: never write-protect command memory in draw snapshots` | Snapshot change, `cmd=` metric, Windows test, PM4 probe |
| `tools: summarize the [pipeline] fields and [hfr] notices` | Summarizer |
| `patches: restore exact context whitespace in the Xenon runtime patch` | Build fix for `setup.bat` (independent of HFR) |
| `docs: phase 2 findings and test plan` | This file and pointers in HFR-UNLIMITED.md |
