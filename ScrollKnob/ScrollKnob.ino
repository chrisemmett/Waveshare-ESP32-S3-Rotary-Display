/*
 * ScrollKnob - turn the knob into a scroll wheel and show the scroll
 *              direction / speed on the round display.
 *
 * Target board : Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 *                (panel module "JC3636K518": ST77916 QSPI LCD, 360x360)
 *
 * WHY THIS IS SELF-CONTAINED
 *   The earlier draft was written to be pasted into Waveshare's LVGL demo and
 *   left the whole display bring-up as a "paste it here" placeholder, so it
 *   couldn't be flashed on its own. This version brings the panel up itself
 *   using the Arduino_GFX library, so it is one sketch you can compile and run
 *   with nothing but two libraries installed. No LVGL, no lv_conf.h, no
 *   dual-chip flashing.
 *
 *   The knob is a standard quadrature encoder read through the ESP32's hardware
 *   pulse counter (via the ESP32Encoder library), so counts are never missed
 *   even while the screen is being redrawn.
 *
 * ---------------------------------------------------------------------------
 * LIBRARIES (Arduino IDE -> Tools -> Manage Libraries...):
 *   1. "GFX Library for Arduino"  by moononournation   (provides ST77916 + QSPI)
 *   2. "ESP32Encoder"             by Kevin Harrington
 *   USB HID (optional, on by default) needs no extra library - it ships with
 *   the ESP32 Arduino core.
 *
 * BOARD SETTINGS (Arduino IDE -> Tools):
 *   Board:            "ESP32S3 Dev Module"
 *   PSRAM:            "OPI PSRAM"
 *   USB CDC On Boot:  "Enabled"            (keeps Serial working over USB)
 *   USB Mode:         "USB-OTG (TinyUSB)"  (REQUIRED for the HID scroll wheel)
 *   Flash Size:       "16MB (128Mb)"
 *   Upload Mode:      "UART0 / Hardware CDC"
 *
 *   If the board refuses to enumerate as an S3, flip the USB-C plug over and
 *   reflash - that orientation quirk is called out in the Waveshare FAQ.
 * ---------------------------------------------------------------------------
 */

#include <Arduino_GFX_Library.h>
#include <ESP32Encoder.h>

// ============================ Feature toggles ============================
// USB HID scroll wheel. On by default because you asked for a scroll wheel.
// If the board won't enumerate over USB, set this to 0: the display demo
// then still works fully as a standalone "watch the knob" test.
#define ENABLE_USB_HID   1

// ============================ Pin map ============================
// Verified against two independent community configs for this exact board
// (the ESPHome and Tasmota threads for JC3636K518 / ST77916). If your unit
// behaves oddly, these are the first things to re-check.
//
// Display (ST77916, QSPI)
#define LCD_CS     14
#define LCD_SCK    13
#define LCD_D0     15
#define LCD_D1     16
#define LCD_D2     17
#define LCD_D3     18
#define LCD_RST    21
#define LCD_BL     47      // backlight
#define LCD_W      360
#define LCD_H      360

// Rotary encoder (the knob)
#define ENC_A_PIN   8
#define ENC_B_PIN   7

// Push button (pressing the knob) - GPIO0, active low. Press to zero the count.
#define BTN_PIN     0

// ============================ Tunables ============================
// One physical "click" (detent) of this knob produces this many raw encoder
// counts. attachHalfQuad below gives 2 for a typical detented encoder. If one
// click makes the on-screen number jump by 4, set this to 4; if by 1, set 1.
#define PULSES_PER_DETENT   2

#define VEL_WINDOW_MS       80      // how often speed is recomputed & UI redrawn
#define WHEEL_INVERT        false   // flip if scroll direction feels backwards
#define VEL_FULL_SCALE      25.0f   // detents/sec that fills the speed bar (feel)

// ============================ Colours (RGB565) ============================
#define RGB565(r, g, b)  ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
static const uint16_t C_BG    = RGB565(0,   0,   0);     // black
static const uint16_t C_TITLE = RGB565(150, 150, 150);   // dim grey text
static const uint16_t C_IDLE  = RGB565(90,  90,  90);    // grey  - not moving
static const uint16_t C_UP    = RGB565(40,  200, 90);    // green - scrolling up
static const uint16_t C_DOWN  = RGB565(240, 140, 30);    // orange- scrolling down
static const uint16_t C_TRACK = RGB565(35,  35,  35);    // speed-bar background

// ============================ Objects ============================
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_ST77916(
    bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

ESP32Encoder encoder;

#if ENABLE_USB_HID
#include "USB.h"
#include "USBHIDMouse.h"
USBHIDMouse Mouse;
#endif

// ============================ State ============================
enum Dir { DIR_IDLE, DIR_UP, DIR_DOWN };

static long     lastRaw      = 0;   // last raw encoder count
static long     scrollAccum  = 0;   // raw counts waiting to become HID steps
static long     velAccum     = 0;   // raw counts inside the current speed window
static uint32_t lastVelMs    = 0;
static float    detentsPerSec = 0;  // signed: + one way, - the other

// what is currently drawn, so we only repaint what changed
static Dir  drawnDir  = (Dir)-1;
static long drawnPos  = 0x7FFFFFFF;
static int  drawnBar  = -1;

// ============================ Small text helper ============================
// Draws a string centred horizontally at (cx, topY) using the built-in font at
// the given size, over a solid background so it never leaves ghosts behind.
static void drawCentered(const char *s, int cx, int topY, uint8_t size,
                         uint16_t fg, uint16_t bg) {
  int w = (int)strlen(s) * 6 * size;   // classic font cell is 6px wide * size
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(cx - w / 2, topY);
  gfx->print(s);
}

// ============================ UI ============================
static const int CX      = LCD_W / 2;   // 180
static const int ARROW_CY = 150;        // centre of the direction arrow
static const int ARROW_HW = 62;         // arrow half-width / half-height
static const int POS_TOPY = 232;        // top of the position number
static const int BAR_X    = 40;
static const int BAR_Y    = 300;
static const int BAR_W    = 280;
static const int BAR_H    = 26;

static void drawStaticUI() {
  gfx->fillScreen(C_BG);
  drawCentered("SCROLL", CX, 46, 3, C_TITLE, C_BG);
  // speed-bar track
  gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_TRACK);
}

// Redraw the big centre arrow for the given direction.
static void drawArrow(Dir dir) {
  // erase the arrow's bounding box first
  gfx->fillRect(CX - ARROW_HW - 4, ARROW_CY - ARROW_HW - 4,
                (ARROW_HW + 4) * 2, (ARROW_HW + 4) * 2, C_BG);

  if (dir == DIR_UP) {
    // upward triangle + stem
    gfx->fillTriangle(CX - ARROW_HW, ARROW_CY + 8,
                      CX + ARROW_HW, ARROW_CY + 8,
                      CX,            ARROW_CY - ARROW_HW, C_UP);
    gfx->fillRect(CX - 18, ARROW_CY + 8, 36, ARROW_HW - 12, C_UP);
  } else if (dir == DIR_DOWN) {
    // downward triangle + stem
    gfx->fillTriangle(CX - ARROW_HW, ARROW_CY - 8,
                      CX + ARROW_HW, ARROW_CY - 8,
                      CX,            ARROW_CY + ARROW_HW, C_DOWN);
    gfx->fillRect(CX - 18, ARROW_CY - (ARROW_HW - 12), 36, ARROW_HW - 12, C_DOWN);
  } else {
    // idle: a flat dash
    gfx->fillRoundRect(CX - ARROW_HW, ARROW_CY - 10, ARROW_HW * 2, 20, 10, C_IDLE);
  }
}

static uint16_t dirColor(Dir dir) {
  return dir == DIR_UP ? C_UP : dir == DIR_DOWN ? C_DOWN : C_IDLE;
}

// Repaint whatever changed since last time.
static void render(Dir dir, long pos, float speed) {
  if (dir != drawnDir) {
    drawArrow(dir);
    drawnDir = dir;
    drawnPos = 0x7FFFFFFF;   // force the number to repaint in the new colour
  }

  if (pos != drawnPos) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%+ld", pos);   // e.g. "+12", "-3", "+0"
    // clear the number row, then draw centred so shrinking numbers leave no tail
    gfx->fillRect(0, POS_TOPY - 2, LCD_W, 8 * 5 + 4, C_BG);
    drawCentered(buf, CX, POS_TOPY, 5, dirColor(dir), C_BG);
    drawnPos = pos;
  }

  // speed bar: width proportional to |speed|, clamped to the track
  float mag = fabsf(speed);
  int fillW = (int)(fminf(mag / VEL_FULL_SCALE, 1.0f) * (BAR_W - 4));
  if (fillW != drawnBar) {
    // repaint the whole track, then the filled part on top
    gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_TRACK);
    if (fillW > 0) {
      gfx->fillRoundRect(BAR_X + 2, BAR_Y + 2, fillW, BAR_H - 4,
                         (BAR_H - 4) / 2, dirColor(dir));
    }
    drawnBar = fillW;
  }
}

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);

  // --- Encoder (hardware pulse counter, so counts are never dropped) ---
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_A_PIN, ENC_B_PIN);
  encoder.clearCount();

  // --- Button ---
  pinMode(BTN_PIN, INPUT_PULLUP);

#if ENABLE_USB_HID
  // --- USB HID scroll wheel ---
  Mouse.begin();
  USB.begin();
#endif

  // --- Display ---
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed - check PSRAM setting and wiring");
  }
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);   // backlight on
  drawStaticUI();

  lastVelMs = millis();
  Serial.println("ScrollKnob ready - turn the knob.");
}

void loop() {
  // --- Read the knob ---
  long raw   = encoder.getCount();
  long delta = raw - lastRaw;
  lastRaw    = raw;

  if (delta != 0) {
    velAccum    += delta;
    scrollAccum += delta;

#if ENABLE_USB_HID
    // Emit one mouse-wheel step per detent crossed.
    while (labs(scrollAccum) >= PULSES_PER_DETENT) {
      int step   = (scrollAccum > 0) ? 1 : -1;
      int8_t whl = WHEEL_INVERT ? -step : step;   // +ve wheel scrolls up
      Mouse.move(0, 0, whl);
      scrollAccum -= step * PULSES_PER_DETENT;
    }
#else
    // Without HID, still drain the accumulator so it can't overflow.
    scrollAccum = 0;
#endif
  }

  // --- Button: press to zero the counter ---
  static bool btnWas = false;
  bool btnNow = (digitalRead(BTN_PIN) == LOW);
  if (btnNow && !btnWas) {
    encoder.clearCount();
    lastRaw = velAccum = scrollAccum = 0;
    detentsPerSec = 0;
  }
  btnWas = btnNow;

  // --- Recompute speed & redraw on each window boundary ---
  uint32_t now = millis();
  if (now - lastVelMs >= VEL_WINDOW_MS) {
    float secs    = (now - lastVelMs) / 1000.0f;
    detentsPerSec = ((float)velAccum / PULSES_PER_DETENT) / secs;  // signed
    velAccum      = 0;
    lastVelMs     = now;

    long pos = raw / PULSES_PER_DETENT;                 // detents, signed
    Dir  dir = DIR_IDLE;
    if (detentsPerSec >  0.4f) dir = DIR_UP;
    else if (detentsPerSec < -0.4f) dir = DIR_DOWN;

    render(dir, pos, detentsPerSec);
  }

  delay(2);   // ~500 Hz loop; PCNT counts in hardware so nothing is missed
}
