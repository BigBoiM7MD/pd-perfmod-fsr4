# pd-perfmod-fsr4

Drop-in replacement for PDPerfPlugin.dll (UpscalerBasePlugin) with AMD FSR 4 support.

## How it works

1. **FSR 4 first** — loads `amd_fidelityfx_loader.dll` + `amd_fidelityfx_upscaler_dx12.dll` at runtime
2. **Fallback** — if FSR 4 unavailable, loads `PDPerfPlugin_original.dll` and forwards calls

## Usage

1. Build `PDPerfPlugin.dll` and place in game folder alongside REFramework's `dinput8.dll`
2. For FSR 4: also place `amd_fidelityfx_loader.dll` and `amd_fidelityfx_upscaler_dx12.dll`
3. For fallback: rename existing `PDPerfPlugin.dll` to `PDPerfPlugin_original.dll`

## Build

Requires Visual Studio 2022 with C++17 support.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/PDPerfPlugin.dll`
