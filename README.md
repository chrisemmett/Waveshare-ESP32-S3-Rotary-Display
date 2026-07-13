# rotary-display

Turn a **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** into a scroll wheel — over
**USB** or **Bluetooth** — that uses its round display to show what it's doing.

The sketch has two build modes, chosen by the `USE_BLE` toggle at the top of
[`ScrollKnob/ScrollKnob.ino`](ScrollKnob/ScrollKnob.ino):

- **USB mode (`USE_BLE 0`).** Turn the knob and the screen shows a green **up**
  arrow / orange **down** arrow, a running detent counter, and a speed bar. The
  board acts as a USB mouse wheel and scrolls whatever window has focus. Press
  the knob to zero the counter.
- **Bluetooth mode (`USE_BLE 1`, the default).** The board is a BLE mouse wheel.
  See [Bluetooth mode](#bluetooth-mode) below for the pairing and screen
  behaviour.

> **Status:** working end-to-end on real hardware. Getting there took untangling
> several board-specific quirks that aren't obvious from the datasheet or the
> usual example code — they're all written up in
> [Lessons learned](#lessons-learned-read-this-before-you-debug) below. If
> something doesn't work, start there.

## The sketch

Everything lives in [`ScrollKnob/ScrollKnob.ino`](ScrollKnob/ScrollKnob.ino).
It is a single, self-contained sketch — no LVGL, no config headers. It brings
the ST77916 panel up itself with the Arduino_GFX library and decodes the knob
with a small interrupt-driven state machine (no extra encoder library needed).

**Display orientation.** The panel is rotated via `LCD_ROTATION` near the top of
the sketch (`0` = normal, `2` = flipped 180° for an upside-down mount; `1`/`3` =
90°/270°). This drives both the drawing and the touch mapping — `tpRead()`
mirrors the raw CST816 coordinates to match when `LCD_ROTATION` is `2`, so the
DISCONNECT button still lines up after a flip.

## Bluetooth mode

With `USE_BLE 1` (the default) the board is a **Bluetooth Low Energy mouse
wheel** built on the ESP32-S3's radio — no dongle, no extra hardware. It uses
[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) for a small-footprint
HID-over-GATT implementation, and the board's **CST816 touch panel** (idle until
now) for the on-screen controls.

### Behaviour

| Situation | Screen | What's happening |
|---|---|---|
| **Unpaired** | Backlight on, shows **"DISCOVERABLE"** | Advertising as an HID mouse; pair it from your PC's Bluetooth settings (it shows up as **ScrollKnob**). Pairing is "Just Works" — no PIN. |
| **Connected, knob still** | **Off** (panel cleared, backlight off) | The dark screen is the normal resting state. |
| **Connected, turning the knob** | A little **man runs** across the dial | While the screen is otherwise dark, turning the wheel wakes it and animates a run cycle — he runs faster the faster you turn, and faces the direction you scroll. It blanks again ~1.5 s after you stop. |
| **Tap the dark screen** | Shows a **DISCONNECT** button | You have 10 s to act. |
| **Tap DISCONNECT** | → back to "DISCOVERABLE" | *Forgets every Bluetooth bond* and drops the link, so the host must re-pair. |
| **No tap for 10 s** | → back to **off** | The prompt times out. |

The scroll direction, `WHEEL_INVERT`, and `PULSES_PER_DETENT` tunables work
exactly as in USB mode. Scrolling is ignored while unpaired (so it can't jump
the instant you connect).

> **Notes**
> - The running-man is a small pixel-art run cycle stored in
>   [`ScrollKnob/sprites.h`](ScrollKnob/sprites.h) (an indexed-palette bitmap,
>   scaled up with `fillRect` blocks). It's generated from
>   [`tools/sprite_author.py`](tools/sprite_author.py) — edit the ASCII frames
>   there and re-run it to change the character. Tune `RUN_IDLE_MS` (blank delay)
>   and `RUN_STEP_DETENTS` (leg speed) at the top of the sketch.
> - The S3 does **BLE**, not Bluetooth Classic. A BLE HID mouse is supported
>   natively by Windows, macOS, Linux, Android and iOS/iPadOS.
> - This board has no battery, so "Bluetooth" means the *data* is wireless — it
>   still needs USB (or any 5 V source) for power.
> - The 10 s timeout is `DISCONNECT_PROMPT_MS` at the top of the sketch.

### Extra requirements for BLE mode

- **Library:** install **NimBLE-Arduino** by h2zero, **version 2.x** (Library
  Manager). The sketch uses the 2.x API (`getInputReport`, `enableScanResponse`,
  the two-argument connection callbacks); 1.x will not compile.
- **Board settings:** identical to USB mode (see below). The NimBLE stack fits
  comfortably in the 3 MB app partition, and `USB Mode: USB-OTG (TinyUSB)` is
  still fine — it's just not required for HID here.
- Switch back to USB by setting `USE_BLE 0`; NimBLE is then not needed.

> **Compiling — `'digitalPinToGPIONumber' is not a type` (handled for you):**
> this is an Arduino_GFX ↔ ESP32-core incompatibility, not a Bluetooth issue (it
> breaks USB mode too). Board profiles that define `BOARD_HAS_PIN_REMAP` make the
> core turn `pinMode` / `digitalRead` / `digitalWrite` into macros that clash
> with Arduino_GFX's I/O-expander headers. The fix must apply to the *whole*
> build (Arduino_GFX compiles as its own unit, so a sketch `#undef`/`#define`
> can't fix it). The sketch folder therefore ships a
> [`build_opt.h`](ScrollKnob/build_opt.h) containing
> `-DBOARD_USES_HW_GPIO_NUMBERS`; Arduino copies it into every compile command,
> which disables the remap macros globally — **so it should just compile, on any
> board.**
>
> If your toolchain ignores `build_opt.h`, fall back to either: **select the
> "ESP32S3 Dev Module" board** (its `esp32s3` variant doesn't enable remapping),
> or pass the flag yourself — arduino-cli `--build-property
> "compiler.cpp.extra_flags=-DBOARD_USES_HW_GPIO_NUMBERS"`. (Arduino IDE 2.x may
> ask you to allow the sketch's build options the first time, and needs a clean
> rebuild to pick the file up.)
>
> Separately, the ESP32 core's own `touchRead(pin)` macro (also remap-gated) is
> why the touch reader here is called `tpRead()`.

## Flashing it

This board is unusual to flash. Read [Lesson 1](#1-flashing-the-usb-port-talks-to-two-different-chips)
and [Lesson 2](#2-flashing-the-partition-scheme-must-not-reserve-a-model-partition)
before your first upload — the two most confusing failures happen here.

### 1. Libraries (Arduino IDE → Tools → Manage Libraries…)

| Library | Author | Why |
|---|---|---|
| **GFX Library for Arduino** | moononournation | drives the ST77916 QSPI panel |
| **NimBLE-Arduino** (2.x) | h2zero | BLE HID mouse — **only for Bluetooth mode** (`USE_BLE 1`) |

USB HID needs no extra library — it ships with the ESP32 Arduino core (install
"esp32 by Espressif Systems" via the Boards Manager if you haven't). **No
encoder library is required** — the knob is decoded in the sketch itself, and
the CST816 touch panel is driven inline (no touch library needed either).

### 2. Board settings (Arduino IDE → Tools)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** — prefer this over a vendor-specific profile; some enable pin remapping, which breaks the Arduino_GFX build (see the compile note above) |
| PSRAM | **OPI PSRAM** / Enabled |
| USB CDC On Boot | **Enabled** |
| USB Mode | **USB-OTG (TinyUSB)** ← required for USB mode; harmless in Bluetooth mode |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | any scheme **without** an "ESP SR … MODEL" partition, e.g. **16M Flash (3MB APP/9.9MB FATFS)** |

### 3. Put the board into download mode, then upload

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

Four separate, non-obvious problems stood between "compiles fine" and "works."
Each is written as **symptom → cause → fix** so you can jump straight to yours.

### 1. Flashing: the USB port talks to *two* different chips

**Symptom:** upload fails with `A fatal error occurred: This chip is ESP32, not
ESP32-S3. Wrong chip argument?`

**Cause:** this is a **dual-MCU board** — an ESP32-S3 (runs the display/knob)
*and* a classic ESP32 co-processor — sharing a single USB-C port through an
analog switch. **The orientation of the USB-C plug selects which chip you're
connected to.** In one orientation esptool talks to the classic ESP32 (hence
the error); flipped, it talks to the S3.

**Fix:** flip the USB-C plug so you reach the S3. The S3 side enumerates as
`/dev/cu.usbmodem…`; the classic-ESP32 side is `/dev/cu.usbserial…`.

### 2. Flashing: the partition scheme must not reserve a MODEL partition

**Symptom:** compile succeeds, then upload dies with
`No such file or directory: …/srmodels.bin`.

**Cause:** the default Waveshare board profile selects a partition scheme like
**"ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"**. That `MODEL` partition is for
ESP-SR speech recognition, so the build system expects a `srmodels.bin` to
flash — but this sketch has no voice model, so the file never exists and the
flash aborts.

**Fix:** choose a partition scheme **without** a MODEL partition, e.g.
**16M Flash (3MB APP/9.9MB FATFS)**.

### 3. Display: the panel needs the ST77916 *"150"* init sequence

**Symptom:** the sketch runs (serial is alive, the loop ticks) but the round
screen shows **garbled lines** instead of the UI.

**Cause:** Arduino_GFX's `Arduino_ST77916` class **defaults to the
`st77916_180_init_operations`** sequence. This board's panel (`JC3636K518`, a
1.53″-class 360×360 module) needs the **`st77916_150_init_operations`**
sequence instead. Wrong init → garbage on screen.

**Fix:** pass the 150 init explicitly to the constructor:

```cpp
Arduino_GFX *gfx = new Arduino_ST77916(
  bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H,
  0, 0, 0, 0,
  st77916_150_init_operations, sizeof(st77916_150_init_operations));
```

### 4. Knob: it isn't a normal quadrature encoder

**Symptoms, in the order we hit them:**

- With the **ESP32Encoder** (hardware PCNT) library, the count only ever reached
  ±1 and never accumulated — PCNT only registered one channel on this board.
- With a **standard quadrature decoder** (and with Ben Buxton's state-machine
  decoder), every click flashed *up then down*, or counted nothing at all.

**Cause:** this knob **rests with both channels HIGH (A=B=1) and, per click,
briefly drops only one channel and returns** — it never walks the full
`11 → 01 → 00 → 10 → 11` quadrature cycle (we never see `A=B=0`). Textbook
decoders assume that full cycle, so they either net to zero (count +1 leaving
rest, −1 returning) or reject the motion entirely. On top of that, contact
bounce briefly tickles the *other* channel after a click, adding false
reverse counts.

**Fix:** a small **debounced excursion decoder** (in the sketch). It reads both
channels on a pin-change interrupt, takes the direction from *which channel
drops first* after rest, commits one count when the knob returns to rest, and
refuses to arm a new count until the knob has been quietly at rest for
`REST_QUIET_US` — which is what rejects the post-click bounce. This gives a
clean ±1 per detent.

## Pin map (this exact board)

Verified against the community configs for the panel module (`JC3636K518`,
ST77916 + CST816) and Waveshare's schematic. You should not need to touch these.

| Function | GPIO |
|---|---|
| LCD CS / SCK | 14 / 13 |
| LCD D0–D3 | 15 / 16 / 17 / 18 |
| LCD RST / Backlight | 21 / 47 |
| Encoder A / B | 8 / 7 |
| Knob button | 0 |
| Touch (CST816) SDA/SCL/INT/RST | 11 / 12 / 9 / 10 *(used for the on-screen controls in Bluetooth mode)* |

## Tuning

All knobs are `#define`s at the top of the sketch:

- **Scroll feels backwards?** Set `WHEEL_INVERT` to `true`.
- **False reverse counts on slow turns?** Raise `REST_QUIET_US` (µs of quiet
  required before a new count is armed) toward `8000`–`10000`. If fast cranking
  *drops* clicks, lower it toward `2000`. Default is `5000` (5 ms).
- **One click moves the counter by more than 1?** Set `PULSES_PER_DETENT` to
  match. The excursion decoder emits one count per detent, so this is `1`.
- **Speed bar fills too easily / too slowly?** Change `VEL_FULL_SCALE`
  (detents-per-second that fills the bar, USB mode).
- **USB or Bluetooth?** Set `USE_BLE` (`1` = Bluetooth, `0` = USB).
- **DISCONNECT button times out too fast / slow?** Change `DISCONNECT_PROMPT_MS`
  (Bluetooth mode, default `10000` = 10 s).
- **Just want the display test, no USB mouse?** In USB mode set `ENABLE_USB_HID`
  to `0`.
- **Debugging?** Set `DIAG` to `1` to add a 4 s boot delay and stream setup
  progress + live encoder-channel readings over Serial. Leave it `0` for normal
  use.

## Notes on the design

- The knob is decoded on a **pin-change interrupt**, so counts are never dropped
  even while the screen is redrawing — no background task or shared-state
  juggling needed.
- The display only repaints the parts that change (arrow on direction change,
  number and bar on each 80 ms tick), so there's no full-screen flicker.
- HID output and the on-screen numbers are driven from the **same** encoder
  reading each loop, so the scroll and the display can't drift apart.

## Credits / references

- Panel init sequences: [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX)
  (`Arduino_ST77916.h`), and [freddy-/st77916-esp32](https://github.com/freddy-/st77916-esp32)
  for confirming the `_150` sequence on the JC3636K518.
- Board hardware notes: [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8),
  [KrX3D/WaveShare-Knob-Esp32S3](https://github.com/KrX3D/WaveShare-Knob-Esp32S3),
  and [nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518](https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518).
