# pd-perfmod-fsr4

Drop-in replacement for PDPerfPlugin.dll (UpscalerBasePlugin) with AMD FSR 4 support.
Brings FSR 4 upscaling to games that ship an FSR 3 / FSR 2 upscaler path but lack
native FSR 4 support (e.g. RE Engine titles via REFramework).

## How it works

- **FSR 4 first** — loads the AMD FidelityFX loader (`amd_fidelityfx_loader_dx12.dll`)
  plus the FSR 4 upscaler payload at runtime and dispatches through the FFX API.
- No original-plugin fallback: if the FSR 4 loader/payload files are missing the
  upscaler fails to initialize (see the log for what to drop next to the game .exe).

## Usage

1. Build `PDPerfPlugin.dll` and place it in the game folder alongside REFramework's
   `dinput8.dll`.
2. For FSR 4: also place `amd_fidelityfx_loader_dx12.dll` (and the FSR 4 shader/payload
   files it depends on) next to the game .exe. If they're missing you'll get a black
   screen — the log tells you what's required.

## Build

Requires Visual Studio 2022 with C++17 support.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/PDPerfPlugin.dll`

The FidelityFX SDK is expected to be available locally (it is large and not committed);
point your include/lib paths at it as needed for the build.
