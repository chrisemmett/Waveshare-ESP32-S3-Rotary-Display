# rotary-display

Turn a **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** into a polished, **multi-function
knob** — a radial-arc launcher on its 360×360 round display with three apps:

- **Scroll Wheel** — a Bluetooth-HID scroll controller for a laptop.
- **Countdown** — a timer with a depleting progress ring and a haptic alarm.
- **Settings** — brightness, haptics, and Bluetooth disconnect.

The UI is built with **LVGL 8.4** (real fonts, anti-aliased arcs, animated ring
transitions), driven by the knob's rotary encoder + push button, the CST816
touch panel, and a DRV2605 haptic motor. It brings the ST77916 QSPI panel up
itself with Arduino_GFX and decodes the knob with a small interrupt-driven state
machine.

> **Status:** the hardware bring-up (panel init, encoder decode, touch, BLE) is
> proven on real hardware; the LVGL UI on top is new. Getting the board itself
> working took untangling several board-specific quirks — they're written up in
> [Lessons learned](#lessons-learned-read-this-before-you-debug) below. If
> something doesn't work, start there.

> **History:** an earlier single-file build (plain Arduino_GFX; a USB **or** BLE
> scroll wheel with an on-screen menu) lives in the git history. This rewrite
> replaces that UI layer with LVGL while reusing the same hardware layer.

## Interaction model

This knob has **no shaft press** — the UI is driven by **rotate + touch**.
(GPIO0 is only the BOOT button, used for flashing, not a UI control.)

| Input | Effect |
|---|---|
| **Rotate** | Move focus / adjust the active value. One detent fires a short haptic. |
| **Tap** | Activate — open the focused app (tap the centre or its icon), start/pause the timer (tap the centre), toggle the scroll mode pill, or activate a settings row. Fires a confirm haptic. |
| **Tap MENU** | The chevron at bottom-centre returns to the launcher from any app. |

## The apps

### Launcher (radial arc menu) — the home screen
App icons sit on a ring; the **focused one animates to 12 o'clock** under a fixed
pointer, enlarged and glowing, the others dimmed around the ring. The centre
shows the focused app's icon, name, description, and a `"{i} / {n} · PRESS TO
OPEN"` meta line. Rotate to change focus; tap the centre (or the focused item) to open.

### Scroll Wheel
Rotating emits BLE HID wheel events to a paired host. A viewport scrolls with the
knob and the active-direction chevron highlights (fading ~480 ms after the last
tick). **Tapping the mode pill toggles LINE / PAGE** (PAGE sends a larger delta); a running
tick counter shows in the mode pill. Scrolling only reaches a host once paired.

### Countdown
A full-bleed progress ring (idle grey / running amber / finished red) around a
big `MM:SS` readout. **Rotate** (while not running) sets the time in **30 s**
steps, 0–59:59. **Tapping the centre** starts / pauses; tapping when finished resets to the last
value. At zero the ring flashes and the DRV2605 buzzes.

### Safe Cracker
A safe-cracking game (5-tumbler combo, dial 0–49, wraps). Rotate to hunt for each
tumbler; a **listening waveform** and status label (`QUIET` → `GETTING WARMER` →
`VERY STRONG`) strengthen as you near the target. Each tumbler must be approached
from a required direction (alternating CW/ACW, shown by the arrow up top);
arriving from the correct direction and **holding the dial still for 3 s** (a ring
fills around the number) captures it — any movement cancels the hold. Five
captures → **SAFE OPEN**. Five dots track progress. Tap **NEW** (top-left) — or
tap the centre once open — to start a fresh game; **MENU** returns to the
launcher. The combo can be printed to Serial with `GAME_DEBUG 1`; flip
`GAME_INVERT_DIR` if clockwise feels reversed (both in `ui_game.cpp`).

### Settings
A focusable list: **Brightness** (tap to edit, then rotate ±5 % — drives the
backlight PWM live, with a progress bar), **Haptics** (ON/OFF, gates all haptic
feedback), **Bluetooth** (`DISCONNECT` — forgets every bond and drops the link),
and **Always-On** (`AUTO`, static for now).

## Code layout

Everything is in [`ScrollKnob/`](ScrollKnob):

- `ScrollKnob.ino` — hardware + glue: ST77916/QSPI bring-up, the encoder ISR,
  CST816 touch, NimBLE HID mouse, backlight PWM, DRV2605 init, and the LVGL
  display/touch driver + input dispatch. Exposes hardware services via `app.h`
  and forwards input to the UI via `ui.h`.
- `ui.cpp` + `ui_launcher.cpp` / `ui_scroll.cpp` / `ui_timer.cpp` /
  `ui_settings.cpp` / `ui_game.cpp` — the LVGL screens (see `ui_internal.h` for
  shared tokens).
- `haptics.cpp` / `haptics.h` — minimal DRV2605 driver over the shared I²C bus.
- `src/*.c` — LVGL fonts generated from Space Grotesk / JetBrains Mono by
  [`tools/gen_lvgl_fonts.sh`](tools/gen_lvgl_fonts.sh); declared in `fonts_knob.h`.
  (They live in `src/` because the Arduino build only compiles the sketch root
  and a `src/` subfolder — not arbitrary subdirectories.)
- `build_opt.h` — global compiler flags (see below).

**Display orientation.** `LCD_ROTATION` (top of the `.ino`; `2` = flipped 180°
for an upside-down mount) drives both drawing and the touch mapping — `tpRead()`
mirrors the raw CST816 coordinates so taps line up after a flip.

## Building

### 1. Libraries (Arduino IDE → Tools → Manage Libraries…)

| Library | Author | Why |
|---|---|---|
| **GFX Library for Arduino** | moononournation | drives the ST77916 QSPI panel (LVGL's flush target) |
| **lvgl** (8.4.x) | LVGL | the UI toolkit |
| **NimBLE-Arduino** (2.x) | h2zero | BLE HID mouse (the Scroll app) |

Install "esp32 by Espressif Systems" via the Boards Manager if you haven't. No
encoder or touch library is needed — both are driven inline.

### 2. LVGL configuration (no `lv_conf.h` to place)

Rather than the usual hunt-for-`lv_conf.h`, this sketch injects LVGL's config via
compiler flags in [`ScrollKnob/build_opt.h`](ScrollKnob/build_opt.h) (Arduino
applies that file to every translation unit):

```
-DLV_CONF_SKIP=1              # use LVGL's built-in defaults...
-DLV_COLOR_16_SWAP=1          # ...overridden here: big-endian 565 for the QSPI panel
-DLV_MEM_SIZE=0x18000         # 96 KB LVGL heap (four retained screens)
-DLV_LVGL_H_INCLUDE_SIMPLE=1  # fonts include "lvgl.h"
```

If your colours come out byte-swapped on first boot, flip `LV_COLOR_16_SWAP` to
`0` and use `draw16bitRGBBitmap` in place of `draw16bitBeRGBBitmap` in `lvFlush`.

### 3. Fonts

The generated LVGL fonts are committed under `ScrollKnob/src/`. To regenerate
(e.g. to change sizes or add glyphs), run [`tools/gen_lvgl_fonts.sh`](tools/gen_lvgl_fonts.sh)
(needs Node.js; pulls the TTFs from the `@fontsource` npm packages and runs
`lv_font_conv`).

### 4. Board settings (Arduino IDE → Tools)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** — prefer this over a vendor profile; some enable pin remapping, which breaks the Arduino_GFX build (see the compile note below) |
| PSRAM | **OPI PSRAM** / Enabled (LVGL draw buffers live here) |
| USB CDC On Boot | **Enabled** |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | any scheme **without** an "ESP SR … MODEL" partition, e.g. **16M Flash (3MB APP/9.9MB FATFS)**. LVGL + NimBLE + the fonts are tight in a 3 MB app partition — if the link overflows, use a custom `partitions.csv` with a larger app slot (still no MODEL partition — Lesson 2). |

> **Compiling — `'digitalPinToGPIONumber' is not a type` (handled for you):**
> `build_opt.h` ships `-DBOARD_USES_HW_GPIO_NUMBERS`, which disables the ESP32
> core's pin-remap macros globally (they clash with Arduino_GFX). It should just
> compile on any board. If your toolchain ignores `build_opt.h`, select the
> "ESP32S3 Dev Module" board or pass the flag via
> `--build-property "compiler.cpp.extra_flags=..."`.

## Flashing it

This board is unusual to flash. Read [Lesson 1](#1-flashing-the-usb-port-talks-to-two-different-chips)
and [Lesson 2](#2-flashing-the-partition-scheme-must-not-reserve-a-model-partition)
before your first upload — the two most confusing failures happen here.

Because of the dual-chip design (see Lesson 1), you can't just hit Upload:

1. Set the USB-C cable to the **ESP32-S3 orientation** — flip the plug 180° if
   needed. You're in the right orientation when a `usbmodem…` port appears
   (the classic-ESP32 side shows up as `usbserial…`).
2. **Hold the BOOT button** (next to the USB-C port), and while holding it,
   **switch the power off then on** (or replug the cable). Release BOOT. The
   board is now in ROM download mode and a new port appears.
3. Select that port in Arduino IDE and **Upload**.

Every future flash needs this same dance.

## Lessons learned (read this before you debug)

Each is written **symptom → cause → fix** so you can jump straight to yours.

### 1. Flashing: the USB port talks to *two* different chips

**Symptom:** upload fails with `A fatal error occurred: This chip is ESP32, not
ESP32-S3. Wrong chip argument?`

**Cause:** this is a **dual-MCU board** — an ESP32-S3 (runs the display/knob)
*and* a classic ESP32 co-processor — sharing a single USB-C port through an
analog switch. **The orientation of the USB-C plug selects which chip you're
connected to.**

**Fix:** flip the USB-C plug so you reach the S3. The S3 side enumerates as
`/dev/cu.usbmodem…`; the classic-ESP32 side is `/dev/cu.usbserial…`.

### 2. Flashing: the partition scheme must not reserve a MODEL partition

**Symptom:** compile succeeds, then upload dies with
`No such file or directory: …/srmodels.bin`.

**Cause:** the default Waveshare profile selects a scheme with an ESP-SR `MODEL`
partition, so the build expects a `srmodels.bin` this sketch never produces.

**Fix:** choose a partition scheme **without** a MODEL partition, e.g.
**16M Flash (3MB APP/9.9MB FATFS)**.

### 3. Display: the panel needs the ST77916 *"150"* init sequence

**Symptom:** the sketch runs but the round screen shows **garbled lines**.

**Cause:** `Arduino_ST77916` defaults to the `st77916_180_init_operations`
sequence; this panel (`JC3636K518`) needs **`st77916_150_init_operations`**.

**Fix:** pass the 150 init explicitly to the constructor (as this sketch does).

### 4. Knob: it isn't a normal quadrature encoder

**Cause:** this knob **rests with both channels HIGH (A=B=1) and, per click,
briefly drops only one channel and returns** — it never walks the full
quadrature cycle, so textbook decoders net to zero or reject the motion.

**Fix:** a small **debounced excursion decoder** (in the sketch): it takes the
direction from *which channel drops first* after rest, commits one count on
return to rest, and refuses to re-arm until quiet for `REST_QUIET_US`. Clean ±1
per detent.

## Pin map (this exact board)

| Function | GPIO |
|---|---|
| LCD CS / SCK | 14 / 13 |
| LCD D0–D3 | 15 / 16 / 17 / 18 |
| LCD RST / Backlight (PWM) | 21 / 47 |
| Encoder A / B | 8 / 7 |
| Knob button | 0 |
| Touch (CST816) SDA/SCL/INT/RST | 11 / 12 / 9 / 10 |
| Haptics (DRV2605) | on the CST816 I²C bus (SDA 11 / SCL 12), addr **0x5A** |

> The DRV2605 defaults to the ERM effect library. If your unit has an **LRA**
> motor, switch to library 6 and set the LRA bit in `FEEDBACK_CTRL` (0x1A) — see
> the note in `haptics.cpp`. If no DRV2605 answers at 0x5A, haptics degrade to
> silent (set `DIAG 1` to print an I²C scan at boot).

## Tuning

`#define`s at the top of `ScrollKnob.ino` (and per-screen constants in the
`ui_*.cpp` files):

- **Scroll feels backwards?** Set `WHEEL_INVERT` to `true`.
- **False reverse counts on slow turns?** Raise `REST_QUIET_US` toward
  `8000`–`10000`; if fast cranking drops clicks, lower toward `2000` (default `5000`).
- **Timer step / max?** `TM_STEP` and `TM_MAX` in `ui_timer.cpp`.
- **Haptic feel?** The `HAPTIC_FX_*` effect ids in `haptics.h`.
- **Debugging?** Set `DIAG 1` for a boot delay, an I²C scan, and setup logs.

## Credits / references

- Panel init sequences: [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX)
  (`Arduino_ST77916.h`), and [freddy-/st77916-esp32](https://github.com/freddy-/st77916-esp32)
  for confirming the `_150` sequence on the JC3636K518.
- UI toolkit: [LVGL](https://lvgl.io) 8.4. Fonts: Space Grotesk & JetBrains Mono
  (via [Fontsource](https://fontsource.org)), converted with
  [lv_font_conv](https://github.com/lvgl/lv_font_conv).
- Board hardware notes: [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8),
  [KrX3D/WaveShare-Knob-Esp32S3](https://github.com/KrX3D/WaveShare-Knob-Esp32S3),
  [nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518](https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518).
