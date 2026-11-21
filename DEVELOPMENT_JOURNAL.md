# Development Journal — pd-perfmod-fsr4

A running account of the problems we hit while building this plugin and how we
solved them. Written for anyone who wants to understand *why* the code looks the
way it does — the bug reports, the wrong turns, and the final fixes.

---

## The starting point

We had a working **Windows** build: FSR4 upscaling through REFramework,
auto-selecting the best FSR version, with truthful version/GPU logging and a
crash-safe init path. The hard part began the moment we tried it on
**Linux / Proton**.

---

## 1. Colors were wrong on Linux (the red/blue swap)

On Proton the game's present backbuffer is **BGRA**, but the backbuffer's
*reported* format says **RGBA**. FSR always writes RGBA-ordered bytes, so the
final image came out with **red and blue swapped**.

**Fix attempt #1:** a tiny compute shader that reorders R↔B (RGBA → BGRA).
Added in `ceacdfb`.

---

## 2. The wrong revert

A later test on format `87` looked… fine? I misread the result and concluded the
swap wasn't needed, so I **reverted** it (`0f5cc72`) and forced Linux back to
RGBA (`28`). That was a mistake — the `87` test had *already* had the swap
applied; without it, `28` was actually broken.

**Fix:** re-added the swap (`5507416`), then broadened the gate to cover both
`28` and `87` and documented the vkd3d-source reason (`dc51175`).

---

## 3. Real in-game proof (stop guessing)

Instead of reasoning in circles, we ran **A/B tests in-game** and read the logs:

| Format | Result |
|--------|--------|
| `28` (RGBA)  | **all channels scrambled** — wrong present format |
| `87` (BGRA)  | **only a clean red/blue swap** — correct once the swap pass reorders bytes |
| `24` / `10` (HDR) | **clean colors** — different (converting) copy path |

That settled it: **Linux SDR default = `87` + swap.** Commits `29250bd`,
`137b2f3`.

---

## 4. HDR — the 8-bit trap

Someone asked: *does HDR work with `87`?* **No.** `87` is 8-bit; HDR games use a
10-bit (`24`) or float16 (`10`) backbuffer. Forcing `87` on HDR would downgrade
precision *and* recolor the image.

**Fix:** under Proton, if the backbuffer is HDR-capable (`24`/`10`), **keep that
container and skip the swap** (the converting copy path handles it correctly).
Commit `5fc879c`.

**Caveat we can't fix from here:** REFramework hardcodes `isContentHDR = false`,
so FSR itself runs **SDR**. We preserve the HDR *container* (passthrough), but
there's no FSR HDR tone mapping until REFramework is patched.

---

## 5. Sharpness — the slider that did nothing

REFramework exposes a **"Sharpness" toggle** plus a **"Sharpness Amount" slider
(0–5)** meant for FSR3. Two problems surfaced:

- Toggle ON + slider at `0` → `0` sharpening → looked like a **dead switch**.
  I added a `0.8` fallback. Too strong.
- FSR4's API expects `0–1`, not `0–5`. A hard clamp crushed the slider into the
  bottom fifth.

**Iterations:**
- `0.8` → `0.4` fallback (`c4d748c`) — gentler, but still a hidden default.
- Normalize `0–5 → 0–1` (`5ebb07a`) — but split mapping was confusing.
- Fully **linear** `0–5 → 0–1` (`9c6bf76`) — whole slider tracks RCAS evenly.
- Finally **removed the fallback** so `0` = off and the toggle respects the
  slider (`4411916`). Now it's honest and linear.

---

## 6. The ghost log line (stale DLL)

A user pasted a **NON-VERBOSE** log that contained a **verbose-only** line
(`GPU supports FSR version 3.1.5`). Impossible from source — that line is gated
behind the *same* verbose flag that prints `NON-VERBOSE`.

**Root cause:** my build scripts only copied the DLL to `release/`, **never to
the game folder**. The user had been testing a 9-minutes-older build from before
the verbose guard existed.

**Fix:** deployed the fresh build directly to the game folder. **Lesson:** always
verify the *deployed* binary's timestamp, not just the source.

---

## 7. The real root cause behind the Linux color bug (`v0.2.0`)

During the `v0.2.0` audit we discovered *why* format `28` (`DXGI_FORMAT_R8G8B8A8_UNORM`)
had scrambled every channel back in section 3:

`makeResource()` was passing raw `DXGI_FORMAT` values directly into
`FfxApiResourceDescription.format` instead of mapping them to the FidelityFX
`FfxApiSurfaceFormat` enum (`0..39`). In `FfxApiSurfaceFormat`, `28` is
`FFX_API_SURFACE_FORMAT_R32_FLOAT` — not RGBA8 (`10`)! Format `87` (`BGRA8`)
was out of bounds (`> 39`), so the runtime ignored it and fell back to the
D3D12 resource desc, leaving only a clean R↔B swap that the compute shader had
to undo.

**Fix:** once `makeResource()` mapped `D3D12_RESOURCE_DIMENSION`, `DXGI_FORMAT`,
and resource usage flags to the real FidelityFX SDK enums, `R8G8B8A8_UNORM`
rendered with proper colors natively under Proton — letting us delete the entire
R/B swap compute shader pass, staging texture, descriptor heap, and HLSL
bytecode.

---

## 8. Preset FSR 4 models vs. Model 4 (DRS) (`v0.2.0`)

Setting `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` unconditionally with
`maxRenderSize = displaySize` caused AMD's FSR 4 runtime to pick **Model 4
(DRS)** for every preset instead of the dedicated per-preset ML models
(Model 0 Native AA / 1 Quality / 2 Balanced / 3 Performance / 5 Ultra
Performance).

**Fix:** defaulted `[Backend] DynamicResolution=0` (omitting the DRS flag and
sizing `maxRenderSize` to the active REFramework preset), restored
`qualityLevel == 3` to Ultra Performance (`3.0x`), and added one-shot FFX
context recreation in `evaluate()` when REFramework's `"Use Native Res (DLAA)"`
toggle changes `renderSize` to `displaySize - 1` (snapping `maxRenderSize` to
`displaySize` for Model 0 Native AA). Users who want DRS Model 4 can opt in via
`DynamicResolution=1`.

---

## Open items (REFramework-side, not our bugs)

These are upstream REFramework behaviors we documented so users don't blame this
plugin:

- **HDR tone mapping off** — `isContentHDR` hardcoded `false`.
- **Raw format-blind backbuffer copy** — no channel/format awareness.
- **UI/HUD also gets upscaled** — REFramework's upscaler path includes the UI.
- **Deadlock risk** in REFramework's wait path on some early-return paths.

They don't block functionality; they're noted for transparency.
