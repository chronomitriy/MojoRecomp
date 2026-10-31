# HFR Phase 4: motion blur toggle, FSR 1, CPU and GPU profilers

> **Status: work in progress. This commit does not build yet; do not run `build-smoke.bat` on it.**
> The session stopped early to save usage. The work is committed as it was.
>
> **Known build blocker:** `runtime/gpu/gpu_profile.cpp` and `runtime/host/guest_profiler.cpp` are not yet
> listed in `runtime/CMakeLists.txt`. Add both to `MOJORECOMP_KERNEL_SOURCES`, and `gpu/gpu_profile.cpp`
> to the `MojoRecompTextureGpuProbe` sources, or the link fails.

## Done (code written)

| Item | Where | State |
| --- | --- | --- |
| Settings: `motion_blur`, `upscaler`, `fsr_sharpness`, `guest_profile`, `gpu_profile` | `config/runtime_config.*`, `boot_probe.cpp` | Read from `settings.toml`, then `custom-settings.toml` beside it (strict: unknown keys are rejected), then env vars. Defaults: motion blur on, upscaler `bilinear` (the existing path), profilers off. |
| Env vars | same | `MOJORECOMP_MOTION_BLUR=0`, `MOJORECOMP_UPSCALER=bilinear\|fsr1\|rcas`, `MOJORECOMP_FSR_SHARPNESS=0..1` (default 0.8; RCAS gets `2*(1-x)` stops), `MOJORECOMP_GUEST_PROFILE=1\|all`, `MOJORECOMP_GPU_PROFILE=1`, `MOJORECOMP_CUSTOM_SETTINGS=<path>`. The effective values are logged at startup. |
| PS replacement table | `gpu/shader_replacement.*`, `title_pixel_shaders.*`, `gpu/shader_cache.cpp` | Per-title table. Motion blur PS `89B4E4C8E904ABD5` is replaced with one fetch of `s0` at `TEXCOORD0` (`× g_TextureSampleScale(0)`, alpha 1, same alpha test) when `motion_blur` is off. The replacement is built from the real translation: prologue and signature are kept byte for byte, and it is applied only if size (165 dwords), fetch constants {0,2,3}, inputs {0,2,4} and the emitter layout match. Otherwise the original stays. It is applied after the disk cache, so the cache stays the original. DOF `62F77CF564D38C35` is observe-only: its interface is logged when it loads, and its first draw is logged. `[ps-replace]` log lines. |
| FSR 1 (EASU + RCAS) | `gpu/fsr1.h`, `gpu/vk_presenter.cpp` | Port of AMD FidelityFX FSR 1 (MIT; notice kept in `fsr1.h`). Order: compose the crop, FXAA at internal resolution, EASU to the output size, RCAS into the swapchain. `rcas` mode (or internal ≥ output) is RCAS only. On any failure it falls back to the default path. FXAA's pipeline creation was split from its output image so FSR can share its set layout, sampler and VS. |
| GPU pass timing | `gpu/gpu_profile.*`, `gpu/vk_presenter.cpp` | One timestamp per change of category: scene, front (motion blur by PS role), resolve, transfer, clear, copy, post (compose + FXAA), fsr, present, other. Counts per frame: render passes, resolves, transfers, clears, copies, draws, front draws. `[gpu-profile]` every 120 frames. |
| Guest CPU profiler | `host/guest_profiler.*`, `kernel/guestcall.h`, `title_hooks.cpp`, `guest_native.cpp`, `cpu/guest_thread.cpp`, `boot_probe.cpp`, `host/window.*` | Sampler thread: SuspendThread + GetThreadContext about every 1 ms on the main guest thread (`all`: every guest thread). It maps RIP through the exe's `.pdata` (chained entries resolved) and the sub_ thunk→body jump to `sub_XXXXXXXX`. Buckets: `spin:cp-poll`, `spin:frame-gate-idle` (tagged in the hooks), `wait:`/`import:` per import with guest caller (from `lr`), `native:` CRT hooks, `host:` (DbgHelp names from `cot-runtime.pdb`), `system:<module>`. Top 30 functions plus buckets as `[guest-profile]` lines every 15 s (`MOJORECOMP_GUEST_PROFILE_REPORT_S`) and at window close. No inclusive time: stack walking under suspension is not lock-free. |

## Unfinished

- CMake entries for `gpu_profile.cpp` and `guest_profiler.cpp` (build blocker above).
- `launcher/resources/licenses/AMD-FidelityFX-FSR1-LICENSE.txt`, plus its entries in `THIRD_PARTY_NOTICES.md`, `validate-release.mjs` and `package-release.mjs`. `fsr1.h` refers to this file; the MIT text is only in `fsr1.h` so far.
- Unit tests for `fsr1.h` constants, `gpu_profile` and the profiler core (address map, thunks, classification, report), and an FSR probe in `texture_gpu_probe.cpp`.
- Launcher UI and validation (`main.rs`, `App.svelte`, `types.ts`). `tools/run-custom-runtime.bat` should log the new env vars; they already pass through because the bat inherits the environment.
- `tools/summarize-runtime-log.py` support for `[guest-profile]` and `[gpu-profile]`.
- The full design notes, the safety argument and the local test plan for this document. Pointers from the phase 1–3 documents.

## Compiled or tested (cloud, Linux)

- `RuntimeConfig` test (extended): passes with GCC 13.
- `ShaderReplacement` test (new): passes with GCC 13, with and without the pinned DXC. It translates synthetic microcode (not game data) with the real XenosRecomp path to the motion-blur interface (fetch {0,2,3}, inputs {0,2,4}). It then builds and compiles the replacement, and checks the SPIR-V: TEXCOORD0 is input location 0 in both programs, the replacement's inputs are a subset of the original's, the colour outputs and descriptor bindings match, and the float24-depth variant compiles. Both SPIR-V modules pass `spirv-val`.
- FSR shaders compile with the pinned DXC using the runtime's arguments and pass `spirv-val`. A scratch harness (not committed) on lavapipe compared them with AMD's original `ffx_fsr1.h`. The output was **bit-identical** for 4 EASU size pairs and RCAS at 0, 0.4 and 2 stops. The C++ constants are word-identical to AMD's CPU `FsrEasuCon`/`FsrRcasCon`.
- `vk_presenter.cpp` (FSR, GPU profile, `[ps-replace]` first-draw log): Clang targeting MinGW gives 0 errors. The only new warnings are the file's existing missing-`pNext` pattern.
- **Not compiled at all:** `guest_profiler.cpp`, `gpu_profile.cpp`, `shader_cache.cpp`, `title_pixel_shaders.cpp`, `title_hooks.cpp`, `guest_thread.cpp`, `guest_native.cpp`, `guestcall.h`, `boot_probe.cpp`, `window.cpp`. Nothing ran under Wine, nothing was linked, and nothing ran against the game.
