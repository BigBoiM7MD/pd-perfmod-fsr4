# pd-perfmod-fsr4

A REFramework plugin that brings AMD FidelityFX Super Resolution upscaling to
games that ship an FSR 3 / FSR 2 upscaler path but lack native FSR 4 support
(e.g. RE Engine titles via REFramework). It is a drop-in `PDPerfPlugin.dll`
(UpscalerBasePlugin) that hooks the game's existing upscaler interface and
dispatches through the real AMD FidelityFX (FFX) runtime.

## What it does
- **Auto-selects the newest FSR version your GPU actually supports** at load:
  - FSR 4 (ML upscaler) on RDNA4 cards (e.g. RX 9000 series).
  - FSR 3.1 on older FSR-capable GPUs (falls back automatically).
- **Logs the truthful running version + your GPU** on load — no hardcoded
  constants. First line of the log reads e.g.
  `[INFO] FSR version: 4.1.1  |  GPU: <name>`.
- **Live, readable log.** The log is unbuffered and share-opened, so it can be
  tailed while the game runs (see "Reading the log live" below).

## Why this mod exists
The existing REFramework upscaler plugin does **not** support FSR 4 — it only
exposes FSR 2/3 by default, so RDNA4 owners can't get the newer ML upscaler
through it. Worse, that original plugin is **closed source** and only
distributed through NexusMods, so it can't be inspected, built, or extended by
the community.

This project is an **open-source** reimplementation of the `PDPerfPlugin`
(UpscalerBasePlugin) interface that adds FSR 4 (with automatic FSR 3.1 fallback)
and is freely available in source form. The goal: bring FSR 4 to these games
without depending on a closed, Nexus-only binary.

## Install
1. Install [REFramework](https://github.com/praydog/REFramework) for your game.
2. Copy **all** files from the package into the game folder (same place as
   `dinput8.dll` / REFramework's `reframework/` dir). The two `amd_fidelityfx_*`
   DLLs must sit next to the game `.exe` for the plugin to find them.
3. (Optional) If you also want DLSS/XeSS, install OptiScaler per its guide
   (see "Using DLSS / XeSS as well?" below).
4. Launch the game. The first line of `pd-perfmod-fsr4.log` will show the FSR
   version + GPU. If you instead see `FFX loader NOT FOUND … black screen`, the
   loader/payload DLLs are missing or in the wrong place.

## Want DLSS / XeSS too? Use OptiScaler
If you want DLSS and/or XeSS in addition to (or instead of) FSR, run
**[OptiScaler](https://github.com/optiscaler/OptiScaler)** in conjunction with
this mod. OptiScaler swaps the underlying upscaler backend (it can present
DLSS/XeSS/FSR to the game); this plugin is the FSR path. They are complementary:
install REFramework + this plugin for FSR4, and add OptiScaler if you also want
DLSS/XeSS selection. Follow OptiScaler's own install guide for its part.

## Package contents
The distributable `pd-perfmod-fsr4-v0.1.0.zip` contains:
- `PDPerfPlugin.dll` — the plugin (built from this repo)
- `amd_fidelityfx_loader_dx12.dll` — AMD FSR loader (required; loaded at runtime)
- `amd_fidelityfx_upscaler_dx12.dll` — FSR4 upscaler payload (required)
- `pd-perfmod-fsr4.ini` — settings (created automatically if missing)
- `pd-perfmod-fsr4.log` — runtime log (created next to the DLL on first load)
- `LICENSE-AMD-FSR.md` — AMD FidelityFX SDK redistribution license (see "License")

If you built from source instead of using the zip, copy your
`build/Release/PDPerfPlugin.dll` plus the two `amd_fidelityfx_*` DLLs (from a
FidelityFX SDK / AMD runtime drop) into the game folder.

## Settings (`pd-perfmod-fsr4.ini`)
```
[Logging]
Verbose=0      ; 0 = NON-VERBOSE (version + GPU + errors only)
              ; 1 = VERBOSE (also lists every supported FSR version + internal steps)
```
Change the value and restart the game to apply.

## Reading the log live
The log is written line-by-line and is readable while the game runs (no need to
exit first). To watch it update in real time:
```
Get-Content "path\to\pd-perfmod-fsr4.log" -Wait -Tail 20
```
(Notepad does not auto-refresh — reopen the file, or use the command above.)

## What this plugin is NOT
- It only provides **AMD FSR**. It does not, by itself, add NVIDIA DLSS or Intel
  XeSS. (For those, see the OptiScaler note above.)
- The on-screen upscaler UI you see in a REFramework game is drawn by
  **REFramework itself**, not by this plugin — so any UI/overlay behavior you
  attribute to "the upscaler" is REFramework's, independent of which backend is
  actually running. (An earlier `IDEA` note incorrectly claimed this plugin
  "replaced puredark's plugin / separated the UI from the upscaling path"; that
  was false and has been removed.)

## Linux / Proton
FSR4 (via the AMD loader) **can** run on newer Proton / community Proton forks
(e.g. Proton-GE) with DXVK — it is not Windows-only. If REFramework threw an
exception and stopped working while loading the plugin under Wine/Proton, that
was a bug in this plugin's init path: the load-time exports
(`SetupDirectX`/`InitUpscaler`/`SimpleInit`) were not exception-guarded, so a
failure inside the D3D12/DXGI/FFX calls escaped into REFramework and took down
the game. The init path is now wrapped in the same `try/catch` used by
`evaluate()`, so any failure cleanly disables FSR4 (the game falls back to its
own upscaler) instead of crashing. On native Windows behavior is unchanged.
If FSR4 still fails to start under Proton, the `pd-perfmod-fsr4.log` will now
say exactly which call threw — send that line over.

## How it works
- **FSR 4 first** — at runtime the plugin loads the AMD FidelityFX loader
  (`amd_fidelityfx_loader_dx12.dll`) plus the FSR4 upscaler payload through the
  FFX API. The FFX structs are declared locally in the plugin; no SDK headers are
  compiled in.
- **No silent fallback to the original plugin.** If the FSR loader/payload files
  are missing, the upscaler fails to initialize and you'll see a black screen —
  the log tells you exactly which file to drop next to the game `.exe`.
- The plugin builds as a SHARED library named `PDPerfPlugin` (no `lib` prefix),
  links only `d3d12` + `dxgi`, and enables `/EHa` so both C++ and SEH exceptions
  are caught.

## Building
You do **not** need the FidelityFX SDK to compile. The plugin loads the AMD
loader DLL at runtime via `LoadLibrary` and declares the FFX API structs locally,
so there are no FFX SDK include/lib paths to configure. The SDK (large,
gitignored) is only the source of the runtime loader DLL + FSR4 payload you ship
alongside the game.

### Prerequisites
- Windows 10 / 11
- **Visual Studio 2022** (Community edition is fine) with the
  *Desktop development with C++* workload, including:
  - C++17 support
  - Windows 10 or Windows 11 SDK (provides `d3d12.h`, `dxgi1_6.h`)
- **CMake** >= 3.10 (bundled with VS2022, or install standalone)
- **Git**

### Steps
```bash
git clone https://github.com/BigBoiM7MD/pd-perfmod-fsr4.git
cd pd-perfmod-fsr4

# Configure (generates a VS2022 x64 solution in build/)
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build the Release DLL
cmake --build build --config Release
```

### Output
- `build/Release/PDPerfPlugin.dll` — the plugin (`build/` is gitignored, so it
  won't be committed).
- Also produced alongside it: `PDPerfPlugin.lib` / `PDPerfPlugin.exp` (link
  artifacts; not needed by users).

### Optional: build into a separate dir
The instructions above use `build/` at the repo root. To keep the tree cleaner
you can build anywhere, e.g. `cmake -B out/build -G "Visual Studio 17 2022" -A x64`
and `cmake --build out/build --config Release`. The result is the same DLL.

## License
The AMD FidelityFX loader, upscaler payload, and FSR SDK are © Advanced Micro
Devices, Inc. and redistributed under the AMD FidelityFX SDK license
(`LICENSE-AMD-FSR.md`): **binary redistribution is permitted, free of charge,
provided the copyright + permission notice are included** — which they are, in
that file. Do not reverse-engineer, decompile, or disassemble the AMD binaries,
and do not imply AMD endorsement of this mod.

## Status — first release
This is the **first release (v0.1.0)**.
- Verified on RDNA4 (FSR4 4.1.1): functional and matches/stabilizes expected
  performance vs native TAA on the author's hardware.
- The FSR 3.1 fallback path (non-FSR4 GPUs) is verified at the logic/ABI level;
  confirm on real unsupported hardware before relying on it. If it misbehaves,
  report the log's `FSR version:` line.
- FSR4 on Quality mode should meet or beat native TAA FPS; if you see a regression, check the
  log and report FPS/GPU% numbers.

## Disclaimer
This mod was developed with the assistance of AI tools. It is provided as-is,
without warranty of any kind — use at your own risk. The author is not affiliated
with AMD, Capcom, or the REFramework / OptiScaler projects, and none of them
endorse this mod.
