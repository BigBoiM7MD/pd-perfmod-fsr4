# What's New in v0.2.0 — plain version

This is the easy-to-read summary. For the full technical details, see
[CHANGELOG.md](CHANGELOG.md).

---

## 1. Fixes random launch hangs & crashes with REFramework
Previously, launching the game could occasionally hang or crash during startup
because of unsafe work inside `DllMain`, missing Visual C++ runtime DLLs, or
race conditions during initialization. `PDPerfPlugin.dll` now links its runtime
statically (depends only on `KERNEL32.dll`), initializes safely across threads,
and handles long/Unicode game install folders cleanly.

## 2. Correct FSR4 resource handling, smoother pacing & crash safety
- Fixed a low-level FidelityFX API mismatch where texture formats and types were
  passed as raw DirectX numbers instead of FFX values (which was also the root
  cause behind scrambled RGBA colors on Linux/Proton).
- Upgraded internal GPU command submission from a single stalling allocator to a
  triple-buffered ring with thread-safe locking and clean GPU teardown when
  switching resolutions or closing the game.
- Added real per-frame delta timing (`QueryPerformanceCounter`) instead of a
  hardcoded `16.6 ms` value.

## 3. Better VR, DLAA, multi-version fallback & Linux R/B swap control
- **VR multipass:** left-eye passes (`execute = false`) now record properly and
  submit together with the right eye instead of being skipped.
- **Native Res / DLAA:** supported cleanly via dynamic resolution.
- **Automatic version fallback:** if the newest detected FSR version fails to
  initialize on your driver, the plugin automatically tries older available FSR
  versions before giving up.
- **Linux / Proton:** SDR uses valid matched `R8G8B8A8` formats with R/B swap,
  and you can now toggle `[Backend] RBSwap` (`-1` auto / `0` off / `1` force on)
  in `pd-perfmod-fsr4.ini`.

## 4. Cleaner logs inside REFramework
- Important startup lines and errors now appear directly in REFramework's log
  via `InitLogDelegate`, and `Verbose=0` is now truly the default (matching the
  README).

---

Install is the same as before: drop `PDPerfPlugin.dll` and the two
`amd_fidelityfx_*` DLLs next to the game's `.exe` (same folder as `dinput8.dll`),
and launch. See the README for the full install guide.
