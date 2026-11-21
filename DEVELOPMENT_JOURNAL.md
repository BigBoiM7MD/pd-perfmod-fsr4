# Development Journal — pd-perfmod-fsr4

Notes on bugs hit while testing this plugin in-game and how they were tracked down and fixed. The code and fixes were written with AI while the repo owner tested builds on real hardware and reported what actually happened in-game.

---

## 1. Initial Linux / Proton color bug (the red/blue swap detour)

After the initial Windows build worked, testing under Linux / Proton showed broken colors:

- Trying `DXGI_FORMAT_R8G8B8A8_UNORM` (`28`) scrambled every color channel.
- Trying `DXGI_FORMAT_B8G8R8A8_UNORM` (`87`) gave a clean red/blue channel swap.
- Trying HDR formats (`24` = 10-bit, `10` = float16) didn't look right either — one came out scrambled and the other had the same red/blue swap issue.

In `v0.1.1`, the plugin worked around SDR by defaulting Linux to BGRA (`87`) plus a compute shader (`rb_swap.hlsl`) to swap R and B back, while trying to pass HDR containers (`24`/`10`) through without forcing 8-bit.

---

## 2. Sharpness slider (`v0.1.1`)

REFramework's sharpness slider runs `0–5` (meant for FSR3), while FFX expects `0–1`. Early attempts clamped it or injected a fallback value when set to `0`, which made the slider feel broken. It was changed to a plain linear map (`value / 5.0`), with `0` meaning off.

---

## 3. Stale DLL in the game folder (`v0.1.1`)

A `NON-VERBOSE` log once showed verbose-only lines because the build output wasn't copied over the older `PDPerfPlugin.dll` in the game folder. Lesson: always check the timestamp of the deployed DLL when testing.

---

## 4. Startup hangs and crashes with REFramework (`v0.2.0`)

Launching a game with REFramework would sometimes hang or fail to start at all. A full code audit found several startup bugs:

- `DllMain` was doing `<iostream>` / `std::cout` and path calls under the Windows OS loader lock.
- `PDPerfPlugin.dll` dynamically linked the MSVC runtime (`/MD`) and `d3d12.lib`/`dxgi.lib`, adding load-time dependencies.
- One-time init (`initialize()` and `Logging::init()`) had no synchronization across threads, and path handling used 260-char ANSI buffers.

These were fixed by stripping `DllMain` down to `DisableThreadLibraryCalls`, switching to static `/MT` (only `KERNEL32.dll` imported statically), using `InitOnceExecuteOnce`, and moving path resolution to wide-char helpers.

---

## 5. The real cause of the Linux color bug (`v0.2.0`)

The audit also revealed *why* format `28` (`R8G8B8A8_UNORM`) and the HDR formats (`10`, `24`) were broken back in section 1:

`makeResource()` was passing raw `DXGI_FORMAT` numbers straight into `FfxApiResourceDescription.format` instead of converting them to `FfxApiSurfaceFormat` (`0..39`). In `FfxApiSurfaceFormat`:

- `28` (`DXGI_FORMAT_R8G8B8A8_UNORM`) collided with `R32_FLOAT` instead of `R8G8B8A8_UNORM` (`10`).
- `10` (`DXGI_FORMAT_R16G16B16A16_FLOAT`) collided with `R8G8B8A8_UNORM` instead of `R16G16B16A16_FLOAT` (`4`).
- `24` (`DXGI_FORMAT_R10G10B10A2_UNORM`) collided with `R16_SNORM` instead of `R10G10B10A2_UNORM` (`17`).
- `87` (`DXGI_FORMAT_B8G8R8A8_UNORM`) was out of bounds (`> 39`), so the runtime ignored it and fell back to the D3D12 resource desc, which is why `87` only had a red/blue swap while the other formats broke.

Once `makeResource()` mapped D3D12 dimensions, DXGI formats, and usage flags to the real FFX enums, in-game testing on Proton confirmed `R8G8B8A8_UNORM` gave proper colors with `RBSwap=0` (while enabling the swap actually broke colors). The entire R/B swap compute shader pass was deleted.

---

## 6. FSR 4 stuck on Model 4 (DRS) (`v0.2.0`)

Testing with OptiScaler's overlay showed FSR 4 was using **Model 4 (DRS)** even when picking fixed presets in REFramework. Two bugs caused this:

- `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` was always set with `maxRenderSize = displaySize`, telling FSR 4 to pick the DRS model regardless of preset.
- `qualityLevel == 3` had been mapped to `1.0x` (DLAA), even though REFramework uses `3` for **Ultra Performance (`3.0x`)** and implements "Use Native Res (DLAA)" as a separate checkbox that passes `displaySize - 1` at evaluate time.

Fix: `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` now defaults to off (`[Backend] DynamicResolution=0`) with `maxRenderSize` sized to the active preset so FSR 4 picks Models 1/2/3/5 properly. Toggling "Use Native Res (DLAA)" recreates the FFX context once at `1.0x` for Model 0, and `DynamicResolution=1` opts back into Model 4 (DRS).

---

## Upstream REFramework limitations (outside this plugin)

- `isContentHDR` is hardcoded to `false`, so HDR is passthrough only (no FSR HDR tone mapping).
- REFramework's upscaler path includes the game UI/HUD.
