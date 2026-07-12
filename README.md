# rotary-display

Turn a **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** into a USB scroll wheel that
shows the scroll direction and speed on its round display.

Turn the knob and the screen shows a green **up** arrow / orange **down** arrow,
a running detent counter, and a speed bar. At the same time the board acts as a
USB mouse wheel, so it scrolls whatever window has focus on your computer.
Press the knob to zero the counter.

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

## Flashing it

This board is unusual to flash. Read [Lesson 1](#1-flashing-the-usb-port-talks-to-two-different-chips)
and [Lesson 2](#2-flashing-the-partition-scheme-must-not-reserve-a-model-partition)
before your first upload — the two most confusing failures happen here.

### 1. Libraries (Arduino IDE → Tools → Manage Libraries…)

| Library | Author | Why |
|---|---|---|
| **GFX Library for Arduino** | moononournation | drives the ST77916 QSPI panel |

USB HID needs no extra library — it ships with the ESP32 Arduino core (install
"esp32 by Espressif Systems" via the Boards Manager if you haven't). **No
encoder library is required** — the knob is decoded in the sketch itself.

### 2. Board settings (Arduino IDE → Tools)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** (or the Waveshare profile) |
| PSRAM | **OPI PSRAM** / Enabled |
| USB CDC On Boot | **Enabled** |
| USB Mode | **USB-OTG (TinyUSB)** ← required for the HID scroll wheel |
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
| Touch (CST816) SDA/SCL/INT/RST | 11 / 12 / 9 / 10 *(unused by this sketch)* |

## Tuning

All knobs are `#define`s at the top of the sketch:

- **Scroll feels backwards?** Set `WHEEL_INVERT` to `true`.
- **False reverse counts on slow turns?** Raise `REST_QUIET_US` (µs of quiet
  required before a new count is armed) toward `8000`–`10000`. If fast cranking
  *drops* clicks, lower it toward `2000`. Default is `5000` (5 ms).
- **One click moves the counter by more than 1?** Set `PULSES_PER_DETENT` to
  match. The excursion decoder emits one count per detent, so this is `1`.
- **Speed bar fills too easily / too slowly?** Change `VEL_FULL_SCALE`
  (detents-per-second that fills the bar).
- **Just want the display test, no USB mouse?** Set `ENABLE_USB_HID` to `0`.
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
