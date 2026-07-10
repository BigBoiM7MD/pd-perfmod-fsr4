# pd-perfmod-fsr4

> Get **AMD FSR 4** upscaling in games that don't ship it natively — like
> **Resident Evil** (RE Engine) titles via REFramework. Drop one DLL in and go.
> Works on **Windows** and **Linux / Proton** (Steam Deck included).

## TL;DR

- Picks the **best FSR version your GPU supports** (FSR 4 on RX 9000, FSR 3.1 on older cards).
- **Correct colors on Linux/Proton** — we fix the red/blue swap automatically.
- **HDR is preserved** when the game runs in HDR.
- Open source, drop-in `PDPerfPlugin.dll`. No closed Nexus-only binaries.

## What it is / What it does

A REFramework plugin (`PDPerfPlugin.dll`, an `UpscalerBasePlugin`) that hooks the
game's existing upscaler interface and dispatches through the real AMD
FidelityFX (FFX) runtime. At load it:

- **Auto-selects the newest FSR version your GPU actually supports** — FSR 4
  (ML upscaler) on RDNA4, or FSR 3.1 automatically on older FSR-capable GPUs.
- **Logs the truthful running version + your GPU** — no hardcoded constants.
  First log line reads e.g. `[INFO] FSR version: 4.1.1  |  GPU: AMD Radeon RX 9070 XT`.
- **Handles Linux/Proton color and HDR quirks** so the image looks right
  (see [Linux / Proton](#linux--proton-colors--hdr)).
- Writes a **live, tail-able log** you can watch while the game runs.

## Why this mod exists

The existing REFramework upscaler plugin does **not** support FSR 4 — it only
exposes FSR 2/3 by default, so RDNA4 owners can't get the newer ML upscaler
through it. Worse, that original plugin is **closed source** and only
distributed through NexusMods, so it can't be inspected, built, or extended by
the community.

This project is an **open-source** reimplementation of the `PDPerfPlugin`
interface that adds FSR 4 (with automatic FSR 3.1 fallback) and is freely
available in source form. The goal: bring FSR 4 to these games without
depending on a closed, Nexus-only binary.

## Install

**Quick start**

1. Install [REFramework](https://github.com/praydog/REFramework) for your game.
2. Copy **all** files from the package into the game folder (next to
   `dinput8.dll` / REFramework's `reframework/` dir). The two `amd_fidelityfx_*`
   DLLs must sit next to the game `.exe`.
3. Launch the game. The first log line shows the FSR version + GPU.

**Details**

- The two `amd_fidelityfx_*` DLLs are **required** — the plugin loads them at
  runtime. If they're missing you'll get a black screen and a log line
  `FFX loader NOT FOUND … black screen`.
- (Optional) For DLSS/XeSS too, see [OptiScaler](#want-dlss--xess-too-use-optiscaler).

## Want DLSS / XeSS too? Use OptiScaler

If you want DLSS and/or XeSS in addition to (or instead of) FSR, run
**[OptiScaler](https://github.com/optiscaler/OptiScaler)** alongside this mod.
OptiScaler swaps the underlying upscaler backend (it can present
DLSS/XeSS/FSR to the game); this plugin is the FSR path. They are complementary:
install REFramework + this plugin for FSR4, and add OptiScaler if you also want
DLSS/XeSS selection.

## Package contents

The distributable `pd-perfmod-fsr4-v0.1.1.zip` contains:

- `PDPerfPlugin.dll` — the plugin (built from this repo)
- `amd_fidelityfx_loader_dx12.dll` — AMD FSR loader (required; loaded at runtime)
- `amd_fidelityfx_upscaler_dx12.dll` — FSR4 upscaler payload (required)
- `pd-perfmod-fsr4.ini` — settings (created automatically if missing)
- `pd-perfmod-fsr4.log` — runtime log (created next to the DLL on first load)
- `LICENSE-AMD-FSR.md` — AMD FidelityFX SDK redistribution license

If you built from source instead of using the zip, copy your
`build/Release/PDPerfPlugin.dll` plus the two `amd_fidelityfx_*` DLLs (from a
FidelityFX SDK / AMD runtime drop) into the game folder.

## Settings (`pd-perfmod-fsr4.ini`)

```ini
[Logging]
Verbose=0      ; 0 = NON-VERBOSE (version + GPU + errors only)
              ; 1 = VERBOSE (also lists every supported FSR version + internal steps)

[Backend]
; OutputFormat: override the output texture format. ADVANCED — leave at 0.
; 0  = auto (recommended): Linux picks the right format per HDR/SDR;
;      Windows uses the native backbuffer.
; 28 = R8G8B8A8 (RGBA)     — SDR. WRONG under Linux/Proton (all channels scramble).
; 87 = B8G8R8A8 (BGRA)     — SDR. Correct under Linux/Proton (R/B swap applied).
; 24 = R10G10B10A2         — HDR (10-bit). Auto-selected on Linux in HDR.
; 10 = R16G16B16A16_FLOAT  — HDR (float16). Auto-selected on Linux in HDR.
```

**Sharpness** is controlled from REFramework's upscaler UI, *not* the INI:

- **"Sharpness" toggle** — turn ON to enable sharpening.
- **"Sharpness Amount" slider (0.0 – 5.0)** — maps **linearly** to FSR strength
  (`0` → off, `2.5` → medium `0.5`, `5.0` → maximum `1.0`).
- The toggle **alone does nothing** — you must also raise the Amount slider
  above `0`. `0` means no sharpening.

Change any setting and restart the game to apply.

## Linux / Proton (colors + HDR)

FSR4 (via the AMD loader) **runs on newer Proton / Proton-GE with DXVK** — it is
not Windows-only.

- **Colors are correct out of the box.** Under Proton the game's present
  backbuffer is BGRA, so the plugin automatically outputs BGRA (format `87`) and
  runs a tiny red↔blue swap pass. No setup needed.
- **HDR is preserved.** When the game runs in HDR, the plugin keeps the HDR
  pixel format (10-bit `24` or float16 `10`) end-to-end, so HDR colors pass
  through untouched.
  - *Caveat:* REFramework currently reports the game as SDR to the upscaler
    (`isContentHDR = false`), so FSR does **not** apply HDR-specific tone mapping
    — you get HDR **passthrough**, not FSR HDR tone mapping. Fixing that needs a
    REFramework change, outside this plugin.
- **Crash safety.** If anything fails at load (missing loader DLL, etc.), the
  plugin disables itself cleanly — the game falls back to its own upscaler
  instead of crashing.

If FSR4 won't start under Proton, the log will name the exact failing call —
send that line over.

## Reading the log live

The log is written line-by-line and is readable while the game runs. To watch
it update in real time:

```powershell
Get-Content "path\to\pd-perfmod-fsr4.log" -Wait -Tail 20
```

(Notepad does not auto-refresh — reopen the file, or use the command above.)

## What this plugin is NOT

- It only provides **AMD FSR**. It does not, by itself, add NVIDIA DLSS or Intel
  XeSS. (For those, see the OptiScaler note above.)
- The on-screen upscaler UI you see in a REFramework game is drawn by
  **REFramework itself**, not by this plugin — so any UI/overlay behavior you
  attribute to "the upscaler" is REFramework's. (The in-game HUD/UI is also
  upscaled by REFramework's upscaler path — that's a REFramework behavior, not
  ours.)
- It does **not** apply FSR HDR tone mapping (see the Linux/HDR caveat above).

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
- **Windows:** uses the native backbuffer format, untouched.
- **Linux/Proton:** auto-selects BGRA (`87`) + R/B swap for SDR, or keeps the
  HDR container (`24`/`10`) for HDR; both paths are fail-safe.

## Building from source

You do **not** need the FidelityFX SDK to compile. The plugin loads the AMD
loader DLL at runtime via `LoadLibrary` and declares the FFX API structs locally,
so there are no FFX SDK include/lib paths to configure.

### Prerequisites

- Windows 10 / 11
- **Visual Studio 2022** (Community is fine) with the *Desktop development with C++* workload
- **CMake** >= 3.10 (bundled with VS2022, or standalone)
- **Git**

### Steps

```bash
git clone https://github.com/BigBoiM7MD/pd-perfmod-fsr4.git
cd pd-perfmod-fsr4
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Output

- `build/Release/PDPerfPlugin.dll` — the plugin (`build/` is gitignored).
- Also produced: `PDPerfPlugin.lib` / `PDPerfPlugin.exp` (link artifacts; not needed by users).

## License

The AMD FidelityFX loader, upscaler payload, and FSR SDK are © Advanced Micro
Devices, Inc. and redistributed under the AMD FidelityFX SDK license
(`LICENSE-AMD-FSR.md`): **binary redistribution is permitted, free of charge,
provided the copyright + permission notice are included** — which they are, in
that file. Do not reverse-engineer, decompile, or disassemble the AMD binaries,
and do not imply AMD endorsement of this mod.

## Status

Current release: **v0.1.1** — see [CHANGELOG.md](CHANGELOG.md) for the full list
of fixes in this round (Linux colors, HDR, sharpness).

Verified on RDNA4 (FSR4 4.1.1) on the author's hardware. The FSR 3.1 fallback
path is verified at the logic/ABI level; confirm on real unsupported hardware
before relying on it.

## AI development disclaimer

This mod was developed with the assistance of AI tools. It is provided as-is,
without warranty of any kind — use at your own risk. The author is not affiliated
with AMD, Capcom, or the REFramework / OptiScaler projects, and none of them
endorse this mod.
