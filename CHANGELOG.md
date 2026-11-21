# Changelog

Technical changelog for **pd-perfmod-fsr4** (`v0.x.y`). For a plain-English summary, see [CHANGELOG-SIMPLE.md](CHANGELOG-SIMPLE.md).

---

## [0.2.0] — 2026-11-22

### Fixed
- **Startup hangs and crashes with REFramework:**
  - Removed `<iostream>`, `std::cout`, and path lookup from `DllMain` (now only calls `DisableThreadLibraryCalls`), preventing Windows loader-lock deadlocks.
  - Switched `PDPerfPlugin.dll` to the static MSVC runtime (`/MT` with `/EHa`) and loaded `CreateDXGIFactory1` dynamically so `KERNEL32.dll` is the only static DLL import.
  - Replaced unsynchronized one-time setup in `initialize()` and `Logging::init()` with `InitOnceExecuteOnce`, emitting the startup log line after init finishes to avoid re-entrant deadlocks.
  - Replaced fixed 260-char ANSI path buffers with wide-char (`std::wstring`) helpers supporting Unicode and `\\?\` paths for the INI, log, and loader DLL (probing next to `PDPerfPlugin.dll` first).
- **Init validation and safe fallback:**
  - Validated `SetupDirectX` / `FSR4Backend::setup()` inputs, added partial-init rollback, and fixed `ID3D12Device` reference cleanup.
  - Changed fallback textures to `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` / `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` (retrying `R8G8B8A8_UNORM` on non-UAV formats) and gated query/release exports on `isAvailable()`.
- **FidelityFX API struct and enum mapping:**
  - Fixed `makeResource()` to translate `D3D12_RESOURCE_DIMENSION` to `FfxApiResourceType`, `DXGI_FORMAT` to `FfxApiSurfaceFormat` (`0..39` + depth formats), and D3D12 flags to `FFX_API_RESOURCE_USAGE_*`. Previously, raw `DXGI_FORMAT_R8G8B8A8_UNORM` (`28`) was read by FFX as `R32_FLOAT` (`28`). Added compile-time `sizeof`/`offsetof` `static_assert` checks.
- **D3D12 lifetime, locking, and multi-frame submission:**
  - Replaced the single per-frame stalling command allocator with a 3-slot allocator and fence ring.
  - Added `SRWLOCK` protection around `FSR4Backend::Impl` and `PDPerfPlugin` fallback state, waited on pending GPU fence work before destroying/overwriting contexts, validated inputs before `cmdList->Reset()`, checked `cmdList->Close()`, and closed stranded internal command lists on error/SEH paths.
- **FSR 4 model selection, VR multipass, and version fallback:**
  - Wrapped `EvaluateUpscaler` and `SimpleEvaluate` in `/EHa` `try/catch`, stopped touching caller-provided command lists, and recorded dispatches when `execute == false` (submitting once on `execute == true`) for VR dual-eye rendering.
  - Defaulted `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` to OFF (`[Backend] DynamicResolution=0`) and sized `maxRenderSize` to the active REFramework preset (`Performance` `2.0x` -> Model 3, `Balanced` `1.7x` -> Model 2, `Quality` `1.5x` -> Model 1, `Ultra Performance` `3.0x` -> Model 5) instead of forcing Model 4 (DRS). Toggling REFramework's "Use Native Res (DLAA)" recreates the FFX context at `1.0x` (Model 0). Setting `DynamicResolution=1` enables DRS (Model 4).
  - Kept all enumerated FFX versions and tried them sequentially in `createContext()` if the newest fails.
- **Host logging, frame timing, and defaults:**
  - Wired `InitLogDelegate` to forward `Logging::always` and `Logging::error` lines to REFramework outside `Logging::s_lock` under SEH/C++ guards.
  - Replaced hardcoded `16.6f` `frameTimeDelta` with per-context `QueryPerformanceCounter` deltas clamped to `[0.1, 250.0]` ms.
  - Fixed `Logging.cpp` so `Verbose=0` is actually the default (matching `README.md`).

### Changed
- Removed the Linux/Proton R/B swap compute shader pass (`rb_swap.hlsl` / `rb_swap_cs.h`) now that fixing `FfxApiSurfaceFormat` makes `R8G8B8A8_UNORM` output proper colors directly.
- Removed unused code (`logCallback`, `ffxGetResourceFn`, `<map>`, `s_jitterLogged`, `FSR4_DIAGNOSTICS`).
- Raised minimum CMake version to `3.15` for `/MT` (`CMP0091`) and added a CTest suite targeting PE64 under Wine.

---

## [0.1.1] — 2026-07-10

### Fixed
- Wrapped `SetupDirectX`, `InitUpscaler`, and `SimpleInit` in `try/catch` so startup failures under Wine/Proton disable FSR4 instead of crashing REFramework.
- Worked around swapped red/blue SDR colors on Linux/Proton by outputting BGRA (`87`) with a compute-shader R/B swap pass (later removed in `v0.2.0` once the format mapping bug was found).
- Preserved HDR containers (`R10G10B10A2_UNORM` / `R16G16B16A16_FLOAT`) on Linux/Proton instead of forcing 8-bit output.
- Mapped REFramework's `0–5` sharpness slider linearly to FSR's `0–1` RCAS range and removed the hidden non-zero fallback when set to `0`.
- Logged a one-time warning when the swap pass was skipped due to a non-null destination.

### Notes
- REFramework still passes `isContentHDR = false`, so HDR is passthrough only (no FSR HDR tone mapping).

---

## [0.1.0] — Initial release

### Added
- Initial FSR 4 `PDPerfPlugin.dll` implementation for REFramework with automatic FSR 3.1 fallback on older GPUs.
- Runtime version + GPU logging and unbuffered log output next to the DLL.
- Exception-guarded evaluation path.
