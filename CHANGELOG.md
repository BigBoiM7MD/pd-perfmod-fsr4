# Changelog

All notable changes to **pd-perfmod-fsr4** are documented here. This project
follows a simple versioning scheme: `v0.x.y` for iterative releases.

---

## [0.1.1] — 2026-07-10 (current)

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
- **Linux/Proton support** at the basic level (loads and runs under Proton-GE +
  DXVK).

### Notes
- Correct Linux color handling and HDR container support landed in `0.1.1`.
