# rotary-display

Turn a **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** into a USB scroll wheel that
shows the scroll direction and speed on its round display.

Turn the knob and the screen shows a green **up** arrow / orange **down** arrow,
a running detent counter, and a speed bar. At the same time the board acts as a
USB mouse wheel, so it scrolls whatever window has focus on your computer.
Press the knob to zero the counter.

## The sketch

Everything lives in [`ScrollKnob/ScrollKnob.ino`](ScrollKnob/ScrollKnob.ino).
It is a single, self-contained sketch — no LVGL, no config headers, no
dual-chip flashing. It brings the panel up itself with the Arduino_GFX library
and reads the knob through the ESP32's hardware pulse counter.

## Flashing it

### 1. Libraries (Arduino IDE → Tools → Manage Libraries…)

| Library | Author | Why |
|---|---|---|
| **GFX Library for Arduino** | moononournation | drives the ST77916 QSPI panel |
| **ESP32Encoder** | Kevin Harrington | reads the quadrature knob |

USB HID needs no extra library — it ships with the ESP32 Arduino core (install
"esp32 by Espressif Systems" via the Boards Manager if you haven't).

### 2. Board settings (Arduino IDE → Tools)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| PSRAM | **OPI PSRAM** |
| USB CDC On Boot | **Enabled** |
| USB Mode | **USB-OTG (TinyUSB)** ← required for the scroll wheel |
| Flash Size | 16MB (128Mb) |

> If the board won't enumerate as an S3 when flashing, flip the USB-C plug over
> and try again — that orientation quirk is in the Waveshare FAQ.

### 3. Upload, then turn the knob.

## Pin map (this exact board)

Verified against two independent community configs for the panel module
(`JC3636K518`, ST77916 + CST816). You should not need to touch these.

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
- **One click moves the counter by 2 or 4 instead of 1?** Set
  `PULSES_PER_DETENT` to match (2 is the default for a detented encoder read
  with `attachHalfQuad`).
- **Speed bar fills too easily / too slowly?** Change `VEL_FULL_SCALE`
  (detents-per-second that fills the bar).
- **Just want the display test, no USB mouse?** Set `ENABLE_USB_HID` to `0`.
  The display half then works on its own, which is handy for first-flash
  bring-up before you worry about USB mode.

## Notes on the design

- The knob is a standard quadrature encoder read through the ESP32 **hardware
  pulse counter** (PCNT), so counts are never dropped even while the screen is
  redrawing — no background task or shared-state juggling needed.
- The display only repaints the parts that change (arrow on direction change,
  number and bar on each 80 ms tick), so there's no full-screen flicker.
- HID output and the on-screen numbers are driven from the **same** encoder
  reading each loop, so the scroll and the display can't drift apart.
