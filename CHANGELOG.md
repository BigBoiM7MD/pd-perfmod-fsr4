# Changelog

All notable changes to **pd-perfmod-fsr4** are documented here. This project
follows a simple versioning scheme: `v0.x.y` for iterative releases.

---

## [0.2.0] — 2026-11-22 (current)

Major reliability, FFX ABI, D3D12 lifecycle, concurrency, and host-integration
hardening release.

### Fixed
- **Intermittent launch hangs/crashes with REFramework:**
  - Stripped all `<iostream>` / `std::cout` / path I/O from `DllMain` (now strictly `DisableThreadLibraryCalls`), eliminating OS loader-lock deadlocks at startup.
  - Switched `PDPerfPlugin.dll` to static CRT (`/MT` + `/EHa`) and dynamically resolved `D3D12SerializeRootSignature` and `CreateDXGIFactory1` so `PDPerfPlugin.dll` has only `KERNEL32.dll` as a static import (no `VCRUNTIME140` / `MSVCP140` / `d3d12` / `dxgi` load-time failures).
  - Replaced unsynchronized one-time init in `initialize()` and `Logging::init()` with `InitOnceExecuteOnce`, deferring startup banner logging until after `InitOnceExecuteOnce` finishes.
  - Replaced `MAX_PATH` ANSI path buffers with shared wide-char (`std::wstring`) helpers supporting non-ASCII and extended-length (`\\?\`) paths for INI, log, and `amd_fidelityfx_loader_dx12.dll` lookup (probing next to the DLL first).
- **Safe fallback & COM ownership:**
  - Hardened `SetupDirectX` and `FSR4Backend::setup()` input validation, partial-init rollbacks, and device reference cleanup.
  - Created `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` (`D3D12_RESOURCE_STATE_UNORDERED_ACCESS`) fallback textures (with `R8G8B8A8_UNORM` retry for non-UAV formats) on setup or context creation failure, and gated query/setter/release exports on `isAvailable()`.
- **FidelityFX API ABI accuracy:**
  - Fixed `makeResource()` to map `D3D12_RESOURCE_DIMENSION` -> `FfxApiResourceType`, `DXGI_FORMAT` -> `FfxApiSurfaceFormat` (`0..39` + depth/BGRAX), and D3D12 flags -> `FFX_API_RESOURCE_USAGE_*` (previously passing raw DXGI values caused `R8G8B8A8_UNORM = 28` to collide with `R32_FLOAT = 28`), backed by 11 compile-time `sizeof`/`offsetof` `static_assert`s.
- **D3D12 lifecycle safety, concurrency & multi-frame submission:**
  - Replaced single-allocator per-frame stalls with a 3-slot command allocator + fence-value ring (`kNumCmdSlots = 3`).
  - Guarded `FSR4Backend::Impl` and `PDPerfPlugin` fallback state with non-nested `SRWLOCK`s, drained pending GPU fence work before releasing/overwriting contexts or tearing down, validated inputs before `cmdList->Reset()`, checked `cmdList->Close()` `HRESULT`, and recovered open internal command lists across early returns and SEH exceptions.
- **VR multipass, DLAA, multi-version fallback & Linux R/B swap:**
  - Wrapped `EvaluateUpscaler` and `SimpleEvaluate` under `/EHa` `try/catch`, isolated external `cmdList`s, and recorded dispatches when `execute == false` (submitting once when `execute == true`) for VR dual-eye multipass.
  - Enabled `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` with `maxRenderSize = displaySize`, `qualityLevel == 3` (DLAA / native res), and dynamic `renderSizeX`/`renderSizeY` clamping.
  - Added sequential fallback across all enumerated FSR versions in `createContext()` if the newest version fails to initialize.
  - Fixed Linux/Proton R/B swap to default SDR to `R8G8B8A8_UNORM` (`28`) with matching D3D12 SRV/UAV formats, `UNORDERED_ACCESS` / `UAV` barriers, per-context `rbSwapEnabled` tracking, and a `[Backend] RBSwap` (`-1` auto / `0` off / `1` force on) INI toggle.
- **Host logging, frame timing & defaults:**
  - Wired `InitLogDelegate` to forward `Logging::always` and `Logging::error` messages to REFramework outside `Logging::s_lock` under SEH/C++ guards.
  - Replaced hardcoded `16.6f` `frameTimeDelta` with real per-context clamped (`[0.1, 250.0]` ms) `QueryPerformanceCounter` elapsed milliseconds.
  - Aligned `Logging.cpp` and `README.md` on the `Verbose=0` (`NON-VERBOSE`) default.

### Changed
- Extracted the Linux/Proton R/B swap compute pipeline into standalone `include/RbSwapPass.h` and `src/RbSwapPass.cpp`, and removed dead code (`logCallback`, `ffxGetResourceFn`, `<map>`, `s_jitterLogged`, `FSR4_DIAGNOSTICS`).
- Raised minimum CMake version to `3.15` (`CMP0091` static `/MT` enforcement) and added a 5-suite PE64 Wine CTest harness (`phase1_startup`, `phase1_fallback_com`, `phase2_abi_lifecycle`, `phase2_runtime_paths`, `phase3_logging_timing`).

---

## [0.1.1] — 2026-07-10

The "bugfix run" release. Focus: correct colors and honest controls on every
platform.

### Fixed
- **Linux/Proton: mod failed to open (REFramework crash on load).** The
  load-time exports (`SetupDirectX` / `InitUpscaler` / `SimpleInit`) were not
  exception-guarded, so a failure inside the D3D12/DXGI/FFX calls escaped into
  REFramework and **crashed REFramework** (the plugin wouldn't open at all under
  Wine/Proton). The init path is now wrapped in the same `try/catch` as
  `evaluate()`, so any failure cleanly disables FSR4 (game falls back to its own
  upscaler) instead of taking REFramework down. On native Windows behavior is
  unchanged.
- **Linux/Proton colors:** auto-output BGRA (`87`) with the R/B swap pass, so
  SDR images are color-correct. The old RGBA (`28`) default scrambled all
  channels and is no longer used.
- **HDR on Linux:** preserve the HDR container (`24` = 10-bit, `10` = float16)
  and skip the swap on the HDR path, so HDR colors pass through cleanly.
- **Sharpness slider:** now maps the REFramework `0–5` slider **linearly** to
  FSR's `0–1` RCAS strength. `0` = off (no hidden fallback); the toggle
  respects the slider.
- **Swap-skip warning:** logs a one-time warning if the R/B swap is skipped
  (non-null destination) so color bugs aren't mysterious.

### Changed
- Linux auto-default output format is now `87` (was `28`, which was broken).

### Notes
- REFramework still reports the game as SDR to FSR (`isContentHDR = false`), so
  there is **no FSR HDR tone mapping yet** — HDR is passthrough only. Fixing that
  requires a REFramework change.
- All changes are fail-safe: a bad format or skipped swap logs and continues
  instead of crashing.

---

## [0.1.0] — Initial release

First public version.

### Added
- FSR4 upscaling through REFramework, with **automatic FSR 3.1 fallback** for
  non-FSR4 GPUs (picks the newest version the GPU actually supports at load).
- **Truthful version + GPU logging** — no hardcoded constants.
- **Exception-guarded init path** — a load-time failure (missing loader DLL,
  etc.) cleanly disables FSR4 instead of crashing REFramework / the game.
- **Live, tail-able log** (unbuffered, written next to the DLL).

### Notes
- Correct Linux color handling and HDR container support landed in `0.1.1`.
