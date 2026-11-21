# pd-perfmod-fsr4

An open-source `PDPerfPlugin.dll` (`UpscalerBasePlugin`) for [REFramework](https://github.com/praydog/REFramework) (RE Engine games) that runs **AMD FSR 4** on RDNA4 GPUs and falls back to **FSR 3.1** on older FSR-capable cards. Works on Windows and Linux / Proton.

> **Transparency note:** The C++ code, bug fixes, and tests in this repo were written with AI. I don't code C++ myself — I set up the project, tested builds in-game on my own hardware (Windows and Linux/Proton), tracked down what broke, and directed what to fix or remove. Use it with that in mind.

## Why this exists

REFramework's existing upscaler plugin only exposes FSR 2/3, is closed source, and is only hosted on NexusMods. This is an open-source drop-in replacement for `PDPerfPlugin.dll` that loads AMD's FidelityFX runtime directly so RDNA4 users can run FSR 4 without needing OptiScaler or closed binaries.

If you also want DLSS or XeSS, you can still use [OptiScaler](https://github.com/optiscaler/OptiScaler) — this plugin only handles AMD FSR.

## What it does

- Loads `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` at runtime and picks the newest FSR version your GPU supports (trying older enumerated versions if the newest fails).
- Uses the matching FSR 4 model for your REFramework preset (Quality, Balanced, Performance, Ultra Performance, and "Use Native Res (DLAA)"), with an optional INI switch for Dynamic Resolution Scaling (Model 4).
- Supports Windows and Linux / Proton (`vkd3d-proton`), VR dual-eye rendering, and HDR passthrough.
- Logs startup status, running FSR version, GPU name, and errors both to `pd-perfmod-fsr4.log` (live, unbuffered) and to REFramework's log via `InitLogDelegate`.
- Links the MSVC runtime statically (`/MT`) with `KERNEL32.dll` as its only static DLL import so it doesn't fail to load due to missing VC++ redistributables.

## Install

1. Install [REFramework](https://github.com/praydog/REFramework) (`pd-upscaler` branch / build with `TemporalUpscaler`).
2. Drop the files into the game folder next to the game's `.exe` (same folder as `dinput8.dll`):
   - `PDPerfPlugin.dll`
   - `amd_fidelityfx_loader_dx12.dll` (required, from AMD FidelityFX SDK / driver runtime)
   - `amd_fidelityfx_upscaler_dx12.dll` (required)
   - `LICENSE-AMD-FSR.md` (included in release zips)
3. Launch the game. `pd-perfmod-fsr4.ini` and `pd-perfmod-fsr4.log` are created next to the DLL automatically.

If the two `amd_fidelityfx_*` DLLs are missing, upscaling won't initialize and the log will tell you which file couldn't be loaded.

## Settings (`pd-perfmod-fsr4.ini`)

```ini
[Logging]
Verbose=0      ; 0 = default (startup, active FSR version + GPU, errors only)
               ; 1 = verbose (all detected FSR versions + internal steps)

[Backend]
; 0 = off (default): FSR 4 picks the dedicated model for your REFramework preset
;     (Model 0 Native AA / 1 Quality / 2 Balanced / 3 Performance / 5 Ultra Performance).
;     Toggling "Use Native Res (DLAA)" in REFramework switches cleanly to Model 0.
; 1 = on: enables FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION (FSR 4 Model 4 / DRS)
;     for games that vary render resolution every frame.
DynamicResolution=0
```

Restart the game after editing the INI.

**Sharpness** is controlled in REFramework's upscaler menu:
- Turn **Sharpness** on and raise **Sharpness Amount** (`0.0 – 5.0`, mapped linearly to FSR's `0.0 – 1.0`).
- Leaving the slider at `0` means no sharpening even if the toggle is checked.

## Linux / Proton notes

- Works under Proton / Proton-GE with `vkd3d-proton`.
- **HDR:** HDR surfaces are passed through untouched, but REFramework currently passes `isContentHDR = false` to upscaler plugins, so FSR runs without HDR tone mapping (passthrough only — fixing that requires a REFramework change).
- **Crash safety:** Startup and per-frame calls are wrapped under `/EHa` so loader or D3D12 failures disable the plugin instead of taking REFramework down.

## Watching the log live

```powershell
Get-Content "path\to\pd-perfmod-fsr4.log" -Wait -Tail 20
```

## What this plugin is not

- It does not provide DLSS or XeSS (AMD FSR only).
- The upscaler UI and whether the game's HUD gets upscaled are controlled by REFramework, not this plugin.
- It does not add FSR HDR tone mapping while REFramework reports `isContentHDR = false`.

## Building from source

No FidelityFX SDK headers or libs are needed to build — the FFX C structs are declared locally and the AMD loader DLL is loaded at runtime.

### Prerequisites
- Windows 10 / 11 with Visual Studio 2022 (*Desktop development with C++*) and CMake >= 3.15
- (Or Linux cross-compilation with `clang-cl` + `xwin` and `wine` for tests)

### Build & test (Windows)
```bash
git clone https://github.com/BigBoiM7MD/pd-perfmod-fsr4.git
cd pd-perfmod-fsr4
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Output: `build/Release/PDPerfPlugin.dll` (single-config generators output `build/PDPerfPlugin.dll` and run `ctest --test-dir build --output-on-failure`).

## Status & changelog

Current release: **v0.2.0**
- Plain summary: [CHANGELOG-SIMPLE.md](CHANGELOG-SIMPLE.md)
- Technical details: [CHANGELOG.md](CHANGELOG.md)
- Bug/fix history: [DEVELOPMENT_JOURNAL.md](DEVELOPMENT_JOURNAL.md)

Tested in-game on RDNA4 (FSR 4.1.1) on Linux/Proton. The FSR 3.1 fallback path is covered by mock tests and ABI checks, not yet tested on non-RDNA4 hardware.

## License

Plugin source code is licensed under [LICENSE](LICENSE). The AMD FidelityFX loader and upscaler DLLs are © Advanced Micro Devices, Inc. and redistributed under the AMD FidelityFX SDK license (`LICENSE-AMD-FSR.md`). This project is not affiliated with or endorsed by AMD, Capcom, REFramework, or OptiScaler.
