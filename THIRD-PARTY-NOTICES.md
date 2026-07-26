# Third-party notices

The project itself is MIT-licensed (see [LICENSE](LICENSE)). This file covers
the third-party material it either **embeds** or **builds against**.

## Embedded in this repository

### Space Grotesk and JetBrains Mono (SIL Open Font License 1.1)

The generated LVGL font files under `ScrollKnob/src/` (`font_sg_*.c`,
`font_jbm_*.c`) contain glyph outlines rasterised from these typefaces by
[`lv_font_conv`](https://github.com/lvgl/lv_font_conv), driven by
[`tools/gen_lvgl_fonts.sh`](tools/gen_lvgl_fonts.sh). They are therefore
derivatives of the fonts and remain under the OFL, not the MIT licence above.

- **Space Grotesk** — Copyright the Space Grotesk Project Authors,
  <https://github.com/floriankarsten/space-grotesk>
- **JetBrains Mono** — Copyright the JetBrains Mono Project Authors,
  <https://github.com/JetBrains/JetBrainsMono>

Both are licensed under the SIL Open Font License, Version 1.1:
<https://openfontlicense.org>. The OFL permits bundling and redistribution with
the copyright and licence notice retained (this file), and forbids selling the
font data on its own.

## Dependencies (not distributed here)

These are installed separately through the Arduino Library Manager / board
manager and are not included in this repository. Each keeps its own licence:

- [LVGL](https://github.com/lvgl/lvgl) 8.4 — MIT
- [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) — BSD
- [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) (including
  the BLE HID stack) — mixed, see that project
- Adafruit DRV2605, and the CST816 touch driver — see each project

Panel init sequences and pin mappings were derived from the hardware references
listed under [Credits / references](README.md#credits--references) in the README.
