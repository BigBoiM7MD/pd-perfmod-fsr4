# Changelog (Simple)

Plain-English summary of what changed in each release. For technical details, see [CHANGELOG.md](CHANGELOG.md).

---

## v0.2.0 — 2026-11-22

- **Fixed random launch hangs and crashes with REFramework.** Starting a game with the plugin installed shouldn't randomly freeze or fail to open anymore, including in long or non-ASCII install folders and on systems without extra Visual C++ runtimes installed.
- **Fixed FSR 4 using the wrong model.** Previously, FSR 4 was stuck on Model 4 (the Dynamic Resolution Scaling model) no matter which preset you picked. Now Quality, Balanced, Performance, Ultra Performance, and "Use Native Res (DLAA)" each use their own FSR 4 model. If a game actually uses dynamic resolution, you can turn DRS back on with `DynamicResolution=1` in `pd-perfmod-fsr4.ini`.
- **Removed the Linux/Proton color-swap workaround.** Fixing an underlying texture format bug made Linux/Proton colors work natively without needing the extra red/blue swap shader pass.
- **Fixed VR dual-eye upscaling.** Left-eye frames are no longer skipped when REFramework batches both eyes together.
- **Better fallback if FSR 4 fails to start.** If the newest detected FSR version fails to initialize on your GPU/driver, the plugin now tries older supported FSR versions before giving up.
- **Smoother frame handling and real frame timing.** Replaced the single-frame GPU wait and hardcoded `16.6 ms` frame delta with triple-buffered submission and real frame timing, and fixed cleanup crashes when changing settings or closing the game.
- **Logs show up in REFramework.** Important startup lines and errors are now forwarded to REFramework's own log, and `Verbose=0` is actually the default.

---

## v0.1.1 — 2026-07-10

- **Fixed crashes on load under Linux / Proton.** Errors during startup now disable the plugin instead of crashing REFramework.
- **Fixed swapped red/blue colors on Linux / Proton** (using a temporary swap shader pass, later replaced by a proper format fix in `v0.2.0`).
- **Stopped forcing 8-bit output on HDR games** under Linux / Proton so HDR containers are preserved instead of downgraded. (REFramework still tells the plugin the game is SDR, so there is no FSR HDR tone mapping.)
- **Fixed the sharpness slider** in REFramework so `0` is off and `0–5` scales smoothly to FSR's `0–1` range.

---

## v0.1.0

- First release: FSR 4 upscaling through REFramework with automatic FSR 3.1 fallback on older GPUs, version/GPU logging, and a live log file next to the DLL.

---

Install is the same across versions: put `PDPerfPlugin.dll`, `amd_fidelityfx_loader_dx12.dll`, and `amd_fidelityfx_upscaler_dx12.dll` next to the game's `.exe` (same folder as `dinput8.dll`). See [README.md](README.md) for details.
