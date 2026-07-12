/*
 * ScrollKnob - turn the knob into a scroll wheel and show the scroll
 *              direction / speed on the round display.
 *
 * Target board : Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 *                (panel module "JC3636K518": ST77916 QSPI LCD, 360x360)
 *
 * FIXES IN THIS REVISION (board bring-up)
 *   1. DISPLAY: the JC3636K518 panel needs the ST77916 "150" init sequence.
 *      Arduino_GFX's Arduino_ST77916 DEFAULTS to the "180" init, which produced
 *      a garbled screen. We now pass st77916_150_init_operations explicitly.
 *   2. ENCODER: the ESP32Encoder (hardware PCNT) path only registered one
 *      channel on this board (count never accumulated past +/-1). Replaced with
 *      an interrupt-driven quadrature decoder on the two GPIOs - the same
 *      polling-style approach the working community ESPHome config uses. This
 *      also can't drop counts during a screen redraw.
 *
 * ---------------------------------------------------------------------------
 * LIBRARIES (Arduino IDE -> Tools -> Manage Libraries...):
 *   1. "GFX Library for Arduino"  by moononournation   (provides ST77916 + QSPI)
 *   ESP32Encoder is no longer required.
 *   USB HID (optional, on by default) needs no extra library - it ships with
 *   the ESP32 Arduino core.
 *
 * BOARD SETTINGS (Arduino IDE -> Tools):
 *   Board:            "ESP32S3 Dev Module" (or the Waveshare profile)
 *   PSRAM:            "OPI PSRAM" / "Enabled"
 *   USB CDC On Boot:  "Enabled"            (keeps Serial working over USB)
 *   USB Mode:         "USB-OTG (TinyUSB)"  (REQUIRED for the HID scroll wheel)
 *   Flash Size:       "16MB (128Mb)"
 *   Partition Scheme: any WITHOUT an "ESP SR ... MODEL" partition
 *                     (e.g. "16M Flash (3MB APP/9.9MB FATFS)")
 * ---------------------------------------------------------------------------
 */

#include <Arduino_GFX_Library.h>

// ============================ Feature toggles ============================
// USB HID scroll wheel. On by default because you asked for a scroll wheel.
#define ENABLE_USB_HID 1

// Set to 1 to print live diagnostics (setup progress + encoder channels) over
// Serial and add a 4 s boot delay. Handy while bringing the board up; leave at
// 0 for normal use.
#define DIAG 0

// ============================ Pin map ============================
// Display (ST77916, QSPI)
#define LCD_CS 14
#define LCD_SCK 13
#define LCD_D0 15
#define LCD_D1 16
#define LCD_D2 17
#define LCD_D3 18
#define LCD_RST 21
#define LCD_BL 47  // backlight
#define LCD_W 360
#define LCD_H 360

// Rotary encoder (the knob)
#define ENC_A_PIN 8
#define ENC_B_PIN 7

// Push button (pressing the knob) - GPIO0, active low. Press to zero the count.
#define BTN_PIN 0

// ============================ Tunables ============================
// The decoder emits exactly one count per physical detent click, so this is 1.
// (If a click ever moves the on-screen number by more than 1, raise it to match.)
#define PULSES_PER_DETENT 1

#define VEL_WINDOW_MS 80      // how often speed is recomputed & UI redrawn
#define WHEEL_INVERT false    // flip if scroll direction feels backwards
#define VEL_FULL_SCALE 25.0f  // detents/sec that fills the speed bar (feel)

// ============================ Colours (RGB565) ============================
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
static const uint16_t C_BG = RGB565(0, 0, 0);           // black
static const uint16_t C_TITLE = RGB565(150, 150, 150);  // dim grey text
static const uint16_t C_IDLE = RGB565(90, 90, 90);      // grey  - not moving
static const uint16_t C_UP = RGB565(40, 200, 90);       // green - scrolling up
static const uint16_t C_DOWN = RGB565(240, 140, 30);    // orange- scrolling down
static const uint16_t C_TRACK = RGB565(35, 35, 35);     // speed-bar background

// ============================ Types ============================
// Declared up here (before any function) so the Arduino IDE's auto-generated
// function prototypes, which it inserts before the first function, can see it.
enum Dir { DIR_IDLE,
           DIR_UP,
           DIR_DOWN };

// ============================ Display objects ============================
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
// IMPORTANT: pass the "150" init sequence for the JC3636K518 panel. Without
// this, Arduino_ST77916 uses its default "180" sequence and the screen is
// garbled.
Arduino_GFX *gfx = new Arduino_ST77916(
  bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H,
  0, 0, 0, 0,
  st77916_150_init_operations, sizeof(st77916_150_init_operations));

// ============================ Encoder (debounced detent-excursion decoder) ============================
// This knob rests at A=B=1 and, per physical click, briefly drops ONE channel
// and returns WITHOUT completing a full quadrature cycle (it never reaches
// A=B=0), so a textbook quadrature/Buxton decoder counts nothing. We instead
// decode by excursion: the first channel to drop after rest sets the direction,
// and we commit one count when the knob returns to rest.
//
// The important part is bounce rejection. After a click, the contacts twitch
// and briefly tickle the OTHER channel, which used to register a false reverse
// step. So we refuse to arm a new count until the knob has sat quietly at rest
// for REST_QUIET_US - every stray edge pushes that deadline back, so a burst of
// bounce can never start a count; only a real, deliberate turn can.
#define REST_QUIET_US 5000  // require this much quiet-at-rest before a new count

volatile long g_encPos = 0;          // accumulated detents (signed)
volatile int8_t g_pendingDir = 0;    // direction of the click in progress (0 = none)
volatile uint32_t g_restSinceUs = 0; // micros() of the most recent rest sighting

static inline uint8_t readAB() {
  return (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
}

void IRAM_ATTR encISR() {
  uint8_t ab = readAB();
  uint32_t nowUs = micros();

  if (ab == 0b11) {              // at a detent (rest)
    if (g_pendingDir != 0) {     // a real click just finished -> commit it
      g_encPos += g_pendingDir;
      g_pendingDir = 0;
    }
    g_restSinceUs = nowUs;       // (re)start the quiet timer; bounce keeps resetting it
    return;
  }

  // A channel is low. Start a click only if we're idle AND have been quietly at
  // rest long enough that this can't be post-click bounce.
  if (g_pendingDir == 0 && (uint32_t)(nowUs - g_restSinceUs) > REST_QUIET_US) {
    if (ab == 0b01) g_pendingDir = +1;       // A dropped first  -> one way
    else if (ab == 0b10) g_pendingDir = -1;  // B dropped first  -> other way
    // ab == 0b00 (both low): ambiguous, wait for a single-channel state
  }
}

#if ENABLE_USB_HID
#include "USB.h"
#include "USBHIDMouse.h"
USBHIDMouse Mouse;
#endif

// ============================ State ============================
static long lastRaw = 0;      // last raw encoder count
static long scrollAccum = 0;  // raw counts waiting to become HID steps
static long velAccum = 0;     // raw counts inside the current speed window
static uint32_t lastVelMs = 0;
static float detentsPerSec = 0;  // signed: + one way, - the other

// what is currently drawn, so we only repaint what changed
static Dir drawnDir = (Dir)-1;
static long drawnPos = 0x7FFFFFFF;
static int drawnBar = -1;

// ============================ Small text helper ============================
static void drawCentered(const char *s, int cx, int topY, uint8_t size,
                         uint16_t fg, uint16_t bg) {
  int w = (int)strlen(s) * 6 * size;  // classic font cell is 6px wide * size
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(cx - w / 2, topY);
  gfx->print(s);
}

// ============================ UI ============================
static const int CX = LCD_W / 2;  // 180
static const int ARROW_CY = 150;  // centre of the direction arrow
static const int ARROW_HW = 62;   // arrow half-width / half-height
static const int POS_TOPY = 232;  // top of the position number
static const int BAR_X = 40;
static const int BAR_Y = 300;
static const int BAR_W = 280;
static const int BAR_H = 26;

static void drawStaticUI() {
  gfx->fillScreen(C_BG);
  drawCentered("SCROLL", CX, 46, 3, C_TITLE, C_BG);
  gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_TRACK);
}

static void drawArrow(Dir dir) {
  gfx->fillRect(CX - ARROW_HW - 4, ARROW_CY - ARROW_HW - 4,
                (ARROW_HW + 4) * 2, (ARROW_HW + 4) * 2, C_BG);

  if (dir == DIR_UP) {
    gfx->fillTriangle(CX - ARROW_HW, ARROW_CY + 8,
                      CX + ARROW_HW, ARROW_CY + 8,
                      CX, ARROW_CY - ARROW_HW, C_UP);
    gfx->fillRect(CX - 18, ARROW_CY + 8, 36, ARROW_HW - 12, C_UP);
  } else if (dir == DIR_DOWN) {
    gfx->fillTriangle(CX - ARROW_HW, ARROW_CY - 8,
                      CX + ARROW_HW, ARROW_CY - 8,
                      CX, ARROW_CY + ARROW_HW, C_DOWN);
    gfx->fillRect(CX - 18, ARROW_CY - (ARROW_HW - 12), 36, ARROW_HW - 12, C_DOWN);
  } else {
    gfx->fillRoundRect(CX - ARROW_HW, ARROW_CY - 10, ARROW_HW * 2, 20, 10, C_IDLE);
  }
}

static uint16_t dirColor(Dir dir) {
  return dir == DIR_UP ? C_UP : dir == DIR_DOWN ? C_DOWN
                                                : C_IDLE;
}

static void render(Dir dir, long pos, float speed) {
  if (dir != drawnDir) {
    drawArrow(dir);
    drawnDir = dir;
    drawnPos = 0x7FFFFFFF;
  }

  if (pos != drawnPos) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%+ld", pos);
    gfx->fillRect(0, POS_TOPY - 2, LCD_W, 8 * 5 + 4, C_BG);
    drawCentered(buf, CX, POS_TOPY, 5, dirColor(dir), C_BG);
    drawnPos = pos;
  }

  float mag = fabsf(speed);
  int fillW = (int)(fminf(mag / VEL_FULL_SCALE, 1.0f) * (BAR_W - 4));
  if (fillW != drawnBar) {
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
#if DIAG
  delay(4000);  // give the USB host time to re-attach so no early logs are lost
  Serial.println();
  Serial.println("=== DIAG: setup start ===");
#endif

  // --- Encoder: interrupt-driven quadrature on the two knob channels ---
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  g_restSinceUs = micros();  // start the quiet-at-rest timer
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encISR, CHANGE);
#if DIAG
  Serial.println("DIAG: encoder interrupts attached");
#endif

  // --- Button ---
  pinMode(BTN_PIN, INPUT_PULLUP);

#if ENABLE_USB_HID
  Mouse.begin();
  USB.begin();
#if DIAG
  Serial.println("DIAG: USB HID started");
#endif
#endif

  // --- Display ---
#if DIAG
  Serial.println("DIAG: calling gfx->begin() ...");
#endif
  bool gfxOk = gfx->begin();
#if DIAG
  Serial.printf("DIAG: gfx->begin() returned %d\n", gfxOk);
#endif
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);  // backlight on
  drawStaticUI();
#if DIAG
  Serial.println("DIAG: static UI drawn");
#endif

  lastVelMs = millis();
#if DIAG
  Serial.println("=== DIAG: setup DONE - turn the knob now ===");
#endif
}

void loop() {
  // --- Read the knob (updated in the ISR) ---
  long raw = g_encPos;

#if DIAG
  static uint32_t dbgMs = 0;
  if (millis() - dbgMs >= 250) {
    dbgMs = millis();
    Serial.printf("DIAG: raw=%ld A=%d B=%d btn=%d\n",
                  raw, (int)digitalRead(ENC_A_PIN),
                  (int)digitalRead(ENC_B_PIN), (int)digitalRead(BTN_PIN));
  }
#endif

  long delta = raw - lastRaw;
  lastRaw = raw;

  if (delta != 0) {
    velAccum += delta;
    scrollAccum += delta;

#if ENABLE_USB_HID
    while (labs(scrollAccum) >= PULSES_PER_DETENT) {
      int step = (scrollAccum > 0) ? 1 : -1;
      int8_t whl = WHEEL_INVERT ? -step : step;  // +ve wheel scrolls up
      Mouse.move(0, 0, whl);
      scrollAccum -= step * PULSES_PER_DETENT;
    }
#else
    scrollAccum = 0;
#endif
  }

  // --- Button: press to zero the counter ---
  static bool btnWas = false;
  bool btnNow = (digitalRead(BTN_PIN) == LOW);
  if (btnNow && !btnWas) {
    noInterrupts();
    g_encPos = 0;
    interrupts();
    lastRaw = velAccum = scrollAccum = 0;
    detentsPerSec = 0;
  }
  btnWas = btnNow;

  // --- Recompute speed & redraw on each window boundary ---
  uint32_t now = millis();
  if (now - lastVelMs >= VEL_WINDOW_MS) {
    float secs = (now - lastVelMs) / 1000.0f;
    detentsPerSec = ((float)velAccum / PULSES_PER_DETENT) / secs;  // signed
    velAccum = 0;
    lastVelMs = now;

    long pos = raw / PULSES_PER_DETENT;  // detents, signed
    Dir dir = DIR_IDLE;
    if (detentsPerSec > 0.4f) dir = DIR_UP;
    else if (detentsPerSec < -0.4f) dir = DIR_DOWN;

    render(dir, pos, detentsPerSec);
  }

  delay(2);
}
