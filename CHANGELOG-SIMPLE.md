# What's New in v0.1.1 — plain version

This is the easy-to-read summary. For the full technical details, see
[CHANGELOG.md](CHANGELOG.md).

---

## 1. The mod now opens on Linux / Steam Deck (Proton)
Before this update, launching the mod under Linux/Proton would make **REFramework
crash** as the game started, so the mod wouldn't load at all. Now it loads safely
— if something goes wrong, the mod quietly turns itself off instead of taking
REFramework down, and the game keeps running on its normal upscaler.

## 2. Colors are correct on Linux / Proton
Previously, images could come out with **red and blue swapped** (a known quirk of
Proton's graphics layer). The mod now fixes the colors automatically, so the
picture looks right out of the box.

## 3. HDR games keep their HDR look
If you play in **HDR**, the richer, brighter colors are now passed through
correctly instead of being broken by the upscaler.
> Small note: the mod doesn't *re-tone-map* HDR yet (that step has to happen in
> REFramework itself, not this mod). You get HDR colors, just without FSR's extra
> HDR tone mapping.

## 4. The sharpness slider works the way you'd expect
- Turn **"Sharpness"** ON in REFramework's upscaler menu.
- Move the **"Sharpness Amount"** slider: `0` = off, `5` = strongest, and it's a
  smooth, even scale in between.
- `0` truly means no sharpening now — no hidden defaults.

---

That's it. Install is the same as before: drop the files next to the game's
`.exe` (same folder as `dinput8.dll`), and launch. See the README for the full
install guide.
