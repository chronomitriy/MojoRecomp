# HFR Phase 4: motion blur toggle, FSR 1, CPU and GPU profilers

Branch `hfr-phase4`, based on `hfr-phase3` at `bbf8a7a`.

The code was written in a cloud session (Linux, no game). It was finished and validated on
the owner's Windows PC: built with the project's clang-cl toolchain, unit-tested, checked
on the GPU (GTX 1080 Ti) with the presenter probes, and the motion blur replacement was run
against the runtime's own cached translation of the game's shader. Nothing was run against
the game itself; [section 8](#8-what-was-and-was-not-verified) lists exactly what was
verified. No game data, generated `ppc\` output or shader dumps are part of these changes.

Every new feature is **off by default**. With no new setting, the runtime should behave like
phase 3. The default path still has a few small additions, none of them measured yet:

- one thread-local load and branch per kernel import call and per call of the native
  memcpy, copy-forward and memset hooks (the profiler tag);
- a spin tag on each scheduler-floor pass;
- a title-shader role lookup (two entries) and a profiling branch per draw;
- an early return from the FSR path per present.

Run 0 in [section 7](#7-test-on-your-pc) checks this.

## Contents

1. [Options](#1-options)
2. [Motion blur toggle](#2-motion-blur-toggle)
3. [FSR 1 upscaling and sharpening](#3-fsr-1-upscaling-and-sharpening)
4. [GPU pass timing](#4-gpu-pass-timing)
5. [Guest CPU profiler](#5-guest-cpu-profiler)
6. [Fixes made while finishing the phase](#6-fixes-made-while-finishing-the-phase)
7. [Test on your PC](#7-test-on-your-pc)
8. [What was and was not verified](#8-what-was-and-was-not-verified)
9. [Not done](#9-not-done)
10. [Environment variables](#10-environment-variables)
11. [Files](#11-files)

## 1. Options

The official launcher 1.1.0 rewrites `settings.toml` from its own fields and drops keys it
does not know, so the new options are read from three places, later ones winning:

1. `settings.toml` (`[graphics]` and `[profiling]`, for a future launcher that writes them);
2. `custom-settings.toml` beside it (or the file named by `MOJORECOMP_CUSTOM_SETTINGS`).
   Only the keys below are accepted; any other key fails startup, so a typo cannot silently
   do nothing;
3. `MOJORECOMP_*` environment variables ([section 10](#10-environment-variables)).

```toml
schema_version = 1
[graphics]
motion_blur = false        # default true
upscaler = "fsr1"          # "bilinear" (default), "fsr1" or "rcas"
fsr_sharpness = 0.8        # 0..1, higher is sharper
[profiling]
guest_profile = "main"     # "off" (default), "main", "all" or a boolean
gpu_profile = true         # default false
```

The effective values are logged once at startup:
`cot-runtime: graphics resolution_scale=... motion_blur=... upscaler=... fsr_sharpness=...;
profiling guest=... gpu=...`.

## 2. Motion blur toggle

### 2.1 What the game draws

The game's `motionblurfp.out` is pixel shader `89B4E4C8E904ABD5` (PM4's FNV-1a over the
microcode, 165 dwords). It is the single full-screen "[front pass]" draw of each frame (with
vertex shader `821F51E34929F4DD`). Read from its translation, it has two blur parts, each
averaging several fetches of slot 0 (the scene) from `TEXCOORD0`:

- a per-pixel directional blur. Its vector comes from two buffers fetched at the pixel
  (slots 2 and 3) and constants c251-c253, and it applies only where that vector passes a
  length threshold (c255);
- for the other pixels, a blur along one screen-wide direction (c0.zw). Its weight appears
  to grow with the distance from the screen centre (c253-c254). This is the likely source
  of the "distance blur" seen in play.

It writes the result with alpha 1 to the front buffer.

### 2.2 The replacement

With `motion_blur = false` the runtime swaps this shader for a one-fetch copy:
`oC0 = float4(tfetch2D(s0, TEXCOORD0.xy).xyz * g_TextureSampleScale(0), 1)`, followed by the
same alpha test the translator appends to every pixel shader. The draw itself still happens,
because it is what writes the front buffer.

The replacement is built from the runtime's real translation of the shader
(`gpu/shader_replacement.cpp`): the prologue (constant buffers, descriptor macros, helpers)
and the entry-point signature are kept byte for byte, and only the body is replaced. The
per-title table (`title_pixel_shaders.cpp`) names the hash, and the replacement is applied
only when every check passes:

- the microcode is 165 dwords, the texture fetch constants are exactly {0, 2, 3} and the
  interpolated inputs exactly {0, 2, 4};
- fetch constant 0 is a 2D fetch, and `r0` is initialised from `TEXCOORD0`;
- the translation has the emitter's exact layout: one entry point, `oC0` as the only colour
  output, the optional float24 depth output and its prologue;
- the replacement compiles and DXC returns SPIR-V.

If any check fails, the log says `[ps-replace] ... replacement REJECTED, original kept:
<reason>` and the game draws its own shader. The first draw of the shader is logged as
`[ps-replace] first motion-blur draw: ... replacement drawing` (or `original drawing`).

The replacement runs after the disk shader cache, on both the translate path and the
start-up preload path, so the cache always holds the original: switching the option back
on needs no cache reset. The replacement's SPIR-V goes through the host shader cache, keyed
by its HLSL.

### 2.3 Why this is safe

- **Same output where the game does not blur.** The game's own shader starts from exactly
  this fetch (slot 0 at `TEXCOORD0`, scaled by `g_TextureSampleScale(0)`). Where neither
  blur part applies, it returns that fetch divided by c254.z. The shader uses c254.z as its
  constant 1 throughout (and c255.x as 0), so the two outputs should be the same. This was
  read from the translation, not measured: run A in [section 7](#7-test-on-your-pc) should
  show no brightness change in areas without blur.
- **Same interface.** Compiled with the pinned DXC and the runtime's arguments, the
  replacement's inputs are a subset of the original's (`TEXCOORD0` at location 0 in both;
  the original also reads `TEXCOORD2` and `TEXCOORD4`). The colour output and the descriptor
  sets and bindings are identical, in both the plain and the float24-depth variants. The
  vertex shader's extra outputs are simply unused.
- **No silent changes elsewhere.** Only that hash is replaced, and only after the interface
  checks. Another build of the game, or a translator change that alters the layout, rejects
  the replacement and keeps the original.

Depth of field (`DOF.out`, PS `62F77CF564D38C35`) is in the table as observe-only: its
interface is logged when it loads, and its first draw is logged if it ever draws. It was not
seen drawing in gameplay logs, and which input holds the sharp image is unknown, so there
is no toggle for it.

## 3. FSR 1 upscaling and sharpening

### 3.1 What it does

`upscaler = "fsr1"` replaces the presenter's final bilinear scale with AMD FidelityFX Super
Resolution 1 (`gpu/fsr1.h`, a port of `ffx_fsr1.h` v1.20210629, MIT;
`launcher/resources/licenses/AMD-FidelityFX-FSR1-LICENSE.txt`):

1. compose the visible crop of the internal image at its own resolution (a 1:1 copy);
2. FXAA at that resolution, when anti-aliasing is on (AMD's recommended order);
3. EASU (edge-adaptive upscale) to the output size;
4. RCAS (robust contrast-adaptive sharpening) into the swapchain; the letterbox bars are
   cleared.

EASU only scales up. When the internal image is not smaller than the output (2x on a 1440p
screen), or with `upscaler = "rcas"`, the crop is scaled to the output like the default
path and only RCAS runs, so it becomes a sharpening option.

`fsr_sharpness` (0..1, default 0.8) maps to AMD's stops as `2 * (1 - x)`: 1 is AMD's maximum
sharpness, 0 is two stops softer.

### 3.2 The "1080p" option

EDRAM emulation scales by an integer (`resolution_scale` 1-3). A non-integer scale was not
investigated in this phase. The practical in-between option is **1x + FSR 1**: the GPU cost
of 1x (1280x720 internal) with EASU's edge reconstruction to 2560x1440. That is FSR's
"Performance" ratio (2.0x per axis), so expect it to be softer than 2x but sharper and less
blocky than 1x bilinear.

### 3.3 Why this is safe

- The default `bilinear` path is unchanged.
- Pipelines, images and descriptor sets are created before the first command of the pass is
  recorded. If anything fails, `DrawFsrPresentation` returns false and the FXAA or default
  path draws the frame. Shader compile errors are logged as `Vulkan FSR shader compile
  failed`.
- The intermediate images use the swapchain format (the presenter prefers B8G8R8A8_UNORM),
  so FXAA and FSR share pipeline formats with the swapchain.
- The images are recreated only when the size changes (window resize, or native 16:9
  presentation toggling), after `vkDeviceWaitIdle`, because frames in flight may still
  sample the old ones.
- The cloud session compared the shaders with AMD's original `ffx_fsr1.h` on lavapipe:
  bit-identical for 4 EASU size pairs and RCAS at 0, 0.4 and 2 stops. The constants are
  word-identical to AMD's `FsrEasuCon`/`FsrRcasCon`.

## 4. GPU pass timing

`MOJORECOMP_GPU_PROFILE=1` writes a Vulkan timestamp whenever the recorded work changes
category. The interval up to the next timestamp is charged to the category that started it:
`scene` (guest draws), `front` (the motion blur pass, by PS hash), `resolve`, `transfer`
(EDRAM ownership transfers), `clear`, `copy` (texture uploads), `post` (compose + FXAA),
`fsr`, `present` and `other`. It also counts render passes, resolves, transfers, clears,
copies, draws and front-pass draws per frame.

Results are read when the frame's fence has signalled, so reading never stalls. Every 120
frames it logs:

```
[gpu-profile] frames=120 gpu=4.512ms max=6.010ms | other=0.120 scene=3.001 ... | per frame: renderPasses=24.0 ... overflow=0 malformed=0
```

A frame holds at most 4,096 timestamps. A frame that needs more is flagged (`overflow`)
and its later work is charged to the last category. Timestamps cost a little GPU time, so
don't compare fps from a profiling run with a normal run.

## 5. Guest CPU profiler

### 5.1 What it reports

`MOJORECOMP_GUEST_PROFILE=1` starts a sampler thread that, about once per millisecond,
suspends the main guest thread (`all`: every guest thread), reads its instruction pointer
and resumes it. The address is mapped to a bucket:

| Bucket | Meaning |
| --- | --- |
| `sub_XXXXXXXX` | Translated guest function. Its host range comes from the exe's `.pdata` (chained entries resolved to their primary function), and the `sub_` thunk is followed to its `__imp__sub_` body. |
| `spin:cp-poll` | The guest is polling the CP read pointer (tagged in `MojoRecompGpuPollBackoff`). |
| `spin:frame-gate-idle` | `Game::Tick` declined the update this pass: the title loop is idling until the next tick (tagged in the scheduler floor hook). |
| `wait:<import>` / `import:<import>` | Inside a kernel import (tagged by `GUEST_FUNCTION_HOOK`); blocking ones (`Wait`, `Delay`, `Sleep`, `Yield`) count as waits. The top guest callers come from `lr`. |
| `native:<name>` | The native CRT hooks (memcpy, forward copy, memset). |
| `host:<function>` | Other runtime code, named through DbgHelp and `cot-runtime.pdb`. |
| `system:<module>` | Other modules (driver, ntdll and so on). |

Every 15 s (`MOJORECOMP_GUEST_PROFILE_REPORT_S`) and once at exit, it logs a summary with
the share of each kind, the top 30 guest functions by self samples, the top 15 other
buckets, and the top 3 guest callers of each wait/import bucket. The exit report
(`scope=total`) is written when the window is closed normally or when the guest entry
returns.

These are self samples only. There is no inclusive (call-tree) time, because walking a
suspended thread's stack is not lock-free.

### 5.2 Why this is safe

- **Off is cheap.** Unregistered threads keep a null slot, so each tag is one thread-local
  load and a branch. No sampler thread or handle is created. The tags sit on hot paths
  (every import, the native memcpy/memset hooks), so run 0 measures them.
- **No deadlock while suspended.** Between `SuspendThread` and `ResumeThread` the sampler
  only calls `GetThreadContext` and reads the slot's atomics. It does not allocate, log or
  take a lock the suspended thread could hold (heap, loader, klog). Classification,
  aggregation and DbgHelp lookups happen after the resume.
- **Thread exit.** Each registration is shared, so a guest thread that exits while a sample
  is in flight cannot free the slot under the sampler. The duplicated handle has only
  suspend/resume, get-context and query rights.
- **Perturbation.** About 1,000 suspend/resume pairs per second per sampled thread, each a
  few microseconds. `all` multiplies that by the number of guest threads. The first report
  that names a host function loads the PDB through DbgHelp, which can take a moment.

## 6. Fixes made while finishing the phase

- **Build blocker:** `gpu/gpu_profile.cpp` and `host/guest_profiler.cpp` were added to
  `MOJORECOMP_KERNEL_SOURCES`, and `gpu_profile.cpp` to the `MojoRecompTextureGpuProbe`
  sources.
- **Macro bug:** `GUEST_FUNCTION_HOOK(guest, host)` gained a profiler tag that names
  `mojorecomp::host::...`. The preprocessor replaced that `host` with the macro's second
  argument (for example `mojorecomp::NtAllocateVirtualMemory_x::...`), so `imports.cpp`,
  `vd.cpp` and `xaudio.cpp` did not compile. The parameter is now `hostFunction`.
- **Import stub generator:** `tools/gen_link_stubs.py`, which runs at every CMake configure,
  scanned build trees other than `build` and `build-smoke`. After a `runtime\build-tests`
  tree (BUILD_TESTING=ON) had generated its test stubs for every import, the next configure
  decided that every import was implemented. It then wrote no link stubs, and
  `build-smoke.bat` failed to link. It now skips `runtime/build` and every `runtime/build-*`
  folder, as `.gitignore` does.
- **Tests and tools:** unit tests for FSR constants, GPU profile accounting and the profiler
  core; FSR GPU probes; `[gpu-profile]` and `[guest-profile]` support in
  `tools/summarize-runtime-log.py`. `tools/run-custom-runtime.bat` now logs the new
  variables. The FSR license file and notice entries were also added.

## 7. Test on your PC

Build with `build-smoke.bat` (no recompile is needed: `config\` is unchanged). Each run
below is started from a PowerShell terminal in the repository root: set the variables, start
the runtime with `tools\run-custom-runtime.bat` ([HFR-UNLIMITED.md 4](HFR-UNLIMITED.md#4-build-and-test-on-windows)),
play, quit normally, then summarise the log:

```powershell
python tools\summarize-runtime-log.py "$env:LOCALAPPDATA\MojoRecomp\logs\cot\runtime.log"
```

The summary prints the phase 4 lines as `phase4:`, `gpu-profile` and `guest-profile`.
Clear a variable afterwards with `$env:NAME = $null`.

**Run 0: default path, no new variables (level 3, 162 mode, 1x).** Phase 3 measured a
median of 161.6 fps (p10 134) in this run, limited by the guest and Vulkan threads, which
are exactly where the default-path additions sit. A clearly lower median or p10 means
those additions cost something.

```powershell
.\tools\run-custom-runtime.bat "$env:USERPROFILE\Games\MojoRecomp-Games\cot" 162
```

**Run A: motion blur off (level 1, 60 mode, your usual 2x).**

```powershell
$env:MOJORECOMP_MOTION_BLUR = "0"
.\tools\run-custom-runtime.bat "$env:USERPROFILE\Games\MojoRecomp-Games\cot" 60
```

- In the summary: `phase4: ... (motion-blur) REPLACED` and `first motion-blur draw ...
  replacement drawing`.
- In the game: the distance blur and its pixelation should be gone, and nothing else should
  change. Look for black or frozen frames, flicker and HUD changes. Compare one spot with
  the variable cleared.

**Run B: 1x + FSR 1, the "1080p" option (level 2, 120 mode).** Set the resolution to 1x
in the launcher's Game Settings first.

```powershell
$env:MOJORECOMP_UPSCALER = "fsr1"
.\tools\run-custom-runtime.bat "$env:USERPROFILE\Games\MojoRecomp-Games\cot" 120
```

- In the summary: `Vulkan FSR active: upscaler=fsr1 EASU+RCAS 1280x720 -> 2560x1440`.
- Compare image quality against 1x without FSR and against 2x. Compare fps against phase 3
  at 1x (120 locked).
- Try `$env:MOJORECOMP_FSR_SHARPNESS = "0.5"` and `"1.0"`.
- At 2x, `fsr1` falls back to sharpening only (`RCAS only` in the log).

**Run C: profilers (level 3, 162 mode, 1x).** This run is input for backlog item 4 (native
code for hot guest functions), not an fps measurement.

```powershell
$env:MOJORECOMP_GPU_PROFILE = "1"
$env:MOJORECOMP_GUEST_PROFILE = "1"
.\tools\run-custom-runtime.bat "$env:USERPROFILE\Games\MojoRecomp-Games\cot" 162
```

- The summary includes `gpu-profile` lines (GPU ms per category) and `guest-profile` lines
  (top guest functions and buckets).

## 8. What was and was not verified

Verified on the owner's PC (Windows 11, clang-cl 22, GTX 1080 Ti):

- `build-smoke.bat` builds `cot-runtime.exe` and the smoke test passes.
- The tests and probes of every changed area pass: `RuntimeConfig`, `ShaderReplacement`,
  `ShaderMetadata`, `PipelineMetrics`, `WindowMode`, the new `GuestProfiler`, `GpuProfile`
  and `Fsr1`, and the GPU probes `TextureGpuProbe`, `FxaaOrientationGpuProbe` (the FXAA
  pipeline split), plus the new `FsrEasuGpuProbe` (FXAA, EASU 320x180 to 640x360, RCAS) and
  `FsrRcasGpuProbe`, which check orientation and colour after the full chain.
- The motion blur replacement was run against the runtime's cached translation of
  `89B4E4C8E904ABD5` on this PC. A scratch harness, not committed, used the real table and
  checks. Every check passed, and both depth variants compiled with the pinned DXC with the
  interface described in [2.3](#23-why-this-is-safe). The cache stamp records 660 bytes
  (165 dwords), so the preload path matches too.
- `summarize-runtime-log.py` gives byte-identical output on earlier run logs. A synthetic
  log built from the runtime's format strings checks the new sections.

Not verified:

- Nothing was run against the game: the visual effect of motion blur off, FSR image quality
  on real frames, fps with FSR, and the profilers' overhead and their output on real code.
- The release flow (`validate-release`, `package-release`) was not run; both scripts pass
  `node --check` with the new license entry.

## 9. Not done

- **Launcher UI** for the new options (`main.rs`, `App.svelte`, `types.ts`). The owner runs
  the official launcher, which cannot show them. Environment variables and
  `custom-settings.toml` cover the workflow.
- **Motion blur strength, or turning off only the screen-wide part** ([2.1](#21-what-the-game-draws)).
  Either can be added to the same replacement once on/off has been seen in the game.
- **Depth of field toggle:** observe-only until the shader is seen drawing.

## 10. Environment variables

New in this phase (phase 1: [HFR-UNLIMITED.md 8](HFR-UNLIMITED.md#8-environment-variables);
phase 2: [HFR-PHASE2.md 9](HFR-PHASE2.md#9-environment-variables); phase 3:
[HFR-PHASE3.md 10](HFR-PHASE3.md#10-environment-variables)):

| Variable | Default | Effect |
| --- | --- | --- |
| `MOJORECOMP_MOTION_BLUR=0` | on | Replace the motion blur pass with a one-fetch copy (section 2) |
| `MOJORECOMP_UPSCALER=fsr1\|rcas` | `bilinear` | FSR 1 EASU + RCAS, or RCAS only (section 3) |
| `MOJORECOMP_FSR_SHARPNESS=0..1` | 0.8 | RCAS strength; RCAS gets `2 * (1 - x)` stops |
| `MOJORECOMP_GPU_PROFILE=1` | off | `[gpu-profile]` GPU time per category every 120 frames (section 4) |
| `MOJORECOMP_GUEST_PROFILE=1\|all` | off | `[guest-profile]` sampling profiler, main or all guest threads (section 5) |
| `MOJORECOMP_GUEST_PROFILE_INTERVAL_US=100..100000` | 1000 | Sampling interval |
| `MOJORECOMP_GUEST_PROFILE_REPORT_S=1..3600` | 15 | Report period |
| `MOJORECOMP_CUSTOM_SETTINGS=<path>` | beside `settings.toml` | Use this `custom-settings.toml`; startup fails if it is missing |

## 11. Files

| Area | Files |
| --- | --- |
| Options | `runtime/config/runtime_config.*`, `runtime/boot_probe.cpp` |
| Motion blur | `runtime/gpu/shader_replacement.*`, `runtime/title_pixel_shaders.*`, `runtime/gpu/shader_cache.cpp`, `runtime/gpu/shader_metadata.*` |
| FSR 1 | `runtime/gpu/fsr1.h`, `runtime/gpu/vk_presenter.cpp`, `launcher/resources/licenses/AMD-FidelityFX-FSR1-LICENSE.txt`, `launcher/resources/THIRD_PARTY_NOTICES.md`, `launcher/scripts/validate-release.mjs`, `launcher/scripts/package-release.mjs` |
| GPU timing | `runtime/gpu/gpu_profile.*`, `runtime/gpu/vk_presenter.cpp` |
| Guest profiler | `runtime/host/guest_profiler.*`, `runtime/kernel/guestcall.h`, `runtime/title_hooks.cpp`, `runtime/guest_native.cpp`, `runtime/cpu/guest_thread.cpp`, `runtime/host/window.*`, `runtime/host/pipeline_metrics.*` |
| Tests | `runtime/tests/fsr1_test.cpp`, `gpu_profile_test.cpp`, `guest_profiler_test.cpp`, `shader_replacement_test.cpp`, `runtime_config_test.cpp`, `texture_gpu_probe.cpp` (`--fsr-easu`, `--fsr-rcas`) |
| Tools | `tools/summarize-runtime-log.py`, `tools/run-custom-runtime.bat` |
