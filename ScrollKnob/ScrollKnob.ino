/*
 * ScrollKnob - LVGL multi-function knob UI.
 *
 * Target board : Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 *                (panel module "JC3636K518": ST77916 QSPI LCD, 360x360,
 *                 CST816 capacitive touch, rotary encoder + push knob,
 *                 DRV2605 haptic driver)
 *
 * This sketch is the HARDWARE + GLUE layer. It brings up the panel, decodes the
 * knob, reads touch, runs the NimBLE HID mouse, drives backlight PWM + haptics,
 * and wires all of that to LVGL. The actual screens live in ui_*.cpp; this file
 * only exposes hardware services (app.h) and forwards input to the UI (ui.h).
 *
 * See README for libraries, board settings, the flashing dance, and the
 * board-specific quirks (ST77916 "150" init, the excursion encoder decoder).
 *
 * NOTE: this LVGL build is BLE-only. The older USB-HID + Arduino_GFX build is in
 * the git history (pre-LVGL) if you need the plain USB scroll wheel.
 */

// PIN-REMAP GUARD: build_opt.h defines BOARD_USES_HW_GPIO_NUMBERS globally so the
// ESP32 core doesn't turn pinMode/digitalRead/Write into remap macros that clash
// with Arduino_GFX. It also injects the LVGL config flags (LV_CONF_SKIP, etc.).
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>
#include <esp_random.h>     // esp_random() for the Safe Cracker combo generator
#include <esp_heap_caps.h>  // DMA-capable alloc for the FPS blit buffer

#include "app.h"
#include "ui.h"
#include "haptics.h"

// ============================ Feature toggles ============================
// Print I2C scan + setup progress over Serial (and a 3 s boot delay).
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
#define LCD_BL 47  // backlight (PWM)
#define LCD_W 360
#define LCD_H 360
// 0 = normal, 2 = flipped 180 (upside-down mount). Drives both drawing and the
// touch mapping (tpRead mirrors raw coords when this is 2).
#define LCD_ROTATION 2

// Touch (CST816 capacitive controller, I2C) - shares the bus with the DRV2605.
#define TP_SDA 11
#define TP_SCL 12
#define TP_INT 9
#define TP_RST 10
#define CST816_ADDR 0x15

// Rotary encoder (the knob)
#define ENC_A_PIN 8
#define ENC_B_PIN 7

// NOTE: this knob has NO shaft press. GPIO0 is only the BOOT button (used for
// flashing), not a usable UI button - so the UI is driven entirely by rotate +
// touch. No BTN_PIN is read at runtime.

// ============================ Tunables ============================
#define PULSES_PER_DETENT 1
#define WHEEL_INVERT false            // flip if scroll direction feels backwards
#define BL_PWM_FREQ 5000
#define BL_PWM_RES 8                  // 8-bit duty (0..255)

// ============================ Display objects ============================
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
// IMPORTANT: the JC3636K518 panel needs the "150" init sequence (see README).
Arduino_GFX *gfx = new Arduino_ST77916(
  bus, LCD_RST, LCD_ROTATION, true /* IPS */, LCD_W, LCD_H,
  0, 0, 0, 0,
  st77916_150_init_operations, sizeof(st77916_150_init_operations));

// ============================ Encoder (debounced detent-excursion decoder) ====
// This knob rests at A=B=1 and drops ONE channel per click without completing a
// full quadrature cycle. See README "Lesson 4".
#define REST_QUIET_US 5000

volatile long g_encPos = 0;
volatile int8_t g_pendingDir = 0;
volatile uint32_t g_restSinceUs = 0;

static inline uint8_t readAB() {
  return (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
}

void IRAM_ATTR encISR() {
  uint8_t ab = readAB();
  uint32_t nowUs = micros();
  if (ab == 0b11) {
    if (g_pendingDir != 0) {
      g_encPos += g_pendingDir;
      g_pendingDir = 0;
    }
    g_restSinceUs = nowUs;
    return;
  }
  if (g_pendingDir == 0 && (uint32_t)(nowUs - g_restSinceUs) > REST_QUIET_US) {
    if (ab == 0b01) g_pendingDir = +1;
    else if (ab == 0b10) g_pendingDir = -1;
  }
}

// ============================ BLE HID mouse ============================
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
  0x09, 0x01, 0xA1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
  0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
  0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xC0, 0xC0
};

static NimBLEServer *bleServer = nullptr;
static NimBLEHIDDevice *bleHid = nullptr;
static NimBLECharacteristic *bleInput = nullptr;
volatile bool g_connected = false;
volatile uint16_t g_connHandle = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    g_connHandle = connInfo.getConnHandle();
    g_connected = true;
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    g_connected = false;
    NimBLEDevice::startAdvertising();
  }
};

static void bleSetup() {
  NimBLEDevice::init("ScrollKnob");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  bleHid = new NimBLEHIDDevice(bleServer);
  bleInput = bleHid->getInputReport(1);
  bleHid->setManufacturer("DIY");
  bleHid->setPnp(0x02, 0xe502, 0xa111, 0x0210);
  bleHid->setHidInfo(0x00, 0x01);
  bleHid->setReportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  bleHid->setBatteryLevel(100);
  bleHid->startServices();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C2);
  adv->addServiceUUID(bleHid->getHidService()->getUUID());
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

// ============================ Touch (CST816) ============================
static void touchInit() {
  Wire.begin(TP_SDA, TP_SCL, 400000);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  pinMode(TP_INT, INPUT_PULLUP);
}

// Reads the current touch point. Returns true + panel-pixel x/y (0..359).
static bool tpRead(int *x, int *y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  uint8_t fingers = d[1] & 0x0F;
  if (fingers == 0) return false;
  int rx = ((d[2] & 0x0F) << 8) | d[3];
  int ry = ((d[4] & 0x0F) << 8) | d[5];
#if LCD_ROTATION == 2
  rx = (LCD_W - 1) - rx;
  ry = (LCD_H - 1) - ry;
#endif
  if (x) *x = rx;
  if (y) *y = ry;
  return true;
}

// ============================ Backlight PWM ============================
static uint8_t g_brightness = 80;  // percent

static void backlightInit() {
  pinMode(LCD_BL, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LCD_BL, BL_PWM_FREQ, BL_PWM_RES);
#else
  ledcSetup(0, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(LCD_BL, 0);
#endif
}

static void backlightWrite(uint8_t pct) {
  if (pct > 100) pct = 100;
  uint32_t duty = (uint32_t)pct * 255 / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LCD_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ============================ Screen blanking (idle timeout) ============================
// Blank the panel (backlight off) after SCREEN_TIMEOUT_MS with no dial/touch
// input. Turning the dial or tapping the screen wakes it; while blank, that
// wake input is swallowed so it never doubles as a UI action.
//
// The Always-On setting (Settings > Always-On) disables the whole thing: when
// it is ON the panel stays lit indefinitely; when it is OFF the timeout below
// applies. There are only those two states - no "auto".
#define SCREEN_TIMEOUT_MS 10000
static uint32_t g_lastActivityMs = 0;
static bool g_screenBlank = false;
static bool g_consumeTouch = false;  // swallow the waking touch until the finger lifts
static bool g_alwaysOn = false;      // Settings > Always-On

static void screenNoteActivity() { g_lastActivityMs = millis(); }

static void screenWake() {
  if (g_screenBlank) {
    g_screenBlank = false;
    backlightWrite(g_brightness);
    // Whatever woke us (dial, alarm, Always-On) can coincide with a finger on
    // the glass: swallow that touch until it lifts so the wake never lands as a
    // UI tap. Cleared by the first sample with no finger down.
    g_consumeTouch = true;
  }
  g_lastActivityMs = millis();
}

static void screenBlank() {
  if (!g_screenBlank) {
    g_screenBlank = true;
    backlightWrite(0);
  }
}

// Called once per loop(): blank the panel when the idle timeout has run out.
// Always-On short-circuits it and also keeps the countdown pinned to "now", so
// switching Always-On off gives a full fresh SCREEN_TIMEOUT_MS before blanking.
static void screenIdleTick(uint32_t now) {
  if (g_alwaysOn) {
    g_lastActivityMs = now;
    return;
  }
  if (!g_screenBlank && (uint32_t)(now - g_lastActivityMs) >= SCREEN_TIMEOUT_MS)
    screenBlank();
}

// ============================ Haptics gate ============================
static bool g_hapticsOn = true;
static bool g_hapticsPresent = false;

// ============================ app.h implementation ============================
void appEmitWheel(int8_t ticks) {
  if (!g_connected || bleInput == nullptr || ticks == 0) return;
  int8_t whl = WHEEL_INVERT ? -ticks : ticks;
  uint8_t report[4] = { 0, 0, 0, (uint8_t)whl };
  bleInput->setValue(report, sizeof(report));
  bleInput->notify();
}
void appForgetBonds(void) {
  NimBLEDevice::deleteAllBonds();
  if (g_connected) bleServer->disconnect(g_connHandle);
}
bool appConnected(void) { return g_connected; }
void appSetBrightness(uint8_t pct) {
  g_brightness = pct;
  // Don't fight the blank: while asleep the new level is applied on wake.
  if (!g_screenBlank) backlightWrite(pct);
}
uint8_t appGetBrightness(void) { return g_brightness; }
void appSetAlwaysOn(bool on) {
  g_alwaysOn = on;
  screenWake();  // on: light it and keep it lit / off: start a fresh countdown
}
bool appAlwaysOn(void) { return g_alwaysOn; }
void appWakeScreen(void) { screenWake(); }
void appSetHaptics(bool on) { g_hapticsOn = on; }
bool appHapticsEnabled(void) { return g_hapticsOn; }
void appHapticTick(void)  { if (g_hapticsOn && g_hapticsPresent) hapticsPlay(HAPTIC_FX_TICK); }
void appHapticPress(void) { if (g_hapticsOn && g_hapticsPresent) hapticsPlay(HAPTIC_FX_PRESS); }
void appHapticAlarm(void) { if (g_hapticsOn && g_hapticsPresent) hapticsPlay(HAPTIC_FX_ALARM); }
uint32_t appRandom(void)  { return esp_random(); }

void appBlit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  gfx->draw16bitBeRGBBitmap(x, y, (uint16_t *)px, w, h);
}
void *appDmaAlloc(uint32_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void appKeepAwake(void) { screenNoteActivity(); }

// ============================ LVGL display + input ============================
#define LV_BUF_LINES 40
static lv_disp_draw_buf_t lv_draw_buf;
static lv_color_t *lv_buf1 = nullptr;
static lv_color_t *lv_buf2 = nullptr;
static lv_disp_drv_t lv_disp_drv;
static lv_indev_drv_t lv_touch_drv;

static void lvFlush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
  lv_disp_flush_ready(drv);
}

static void lvTouchRead(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static int lx = 0, ly = 0;
  static bool wasDown = false;
  int x, y;
  bool down = tpRead(&x, &y);

  // Touch only counts as activity on a press/release edge or when the finger
  // actually moves. A CST816 that latches a stale "finger down" report (it does
  // happen) would otherwise pin the idle countdown at zero forever - the screen
  // would never blank, and once blank it would re-wake on its own every frame.
  bool moved = down && wasDown && (x != lx || y != ly);
  bool edge = (down != wasDown);
  bool active = edge || moved;
  if (down) { lx = x; ly = y; }
  wasDown = down;

  data->point.x = lx;
  data->point.y = ly;

  if (!down) {
    if (active) screenNoteActivity();  // countdown restarts when the finger lifts
    g_consumeTouch = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (g_screenBlank) {
    // A real tap wakes the screen; screenWake() then swallows it (here and on
    // every sample until the finger lifts) so waking never doubles as a UI tap.
    // A stale held report is not a tap, so it must not wake anything.
    if (active) screenWake();
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (g_consumeTouch) {  // still swallowing the tap that woke the screen
    if (active) screenNoteActivity();
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (active) screenNoteActivity();
  data->state = LV_INDEV_STATE_PRESSED;
}

static void lvglInit() {
  lv_init();
  size_t px = LCD_W * LV_BUF_LINES;
  Serial.printf("PSRAM: size=%u free=%u\n", (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());
  // Prefer PSRAM for the LVGL draw buffers; fall back to internal RAM (smaller)
  // if PSRAM is missing/disabled, so the UI still renders instead of drawing
  // into a NULL pointer (which shows up as full-screen random-pixel noise).
  lv_buf1 = (lv_color_t *)ps_malloc(px * sizeof(lv_color_t));
  lv_buf2 = (lv_color_t *)ps_malloc(px * sizeof(lv_color_t));
  if (!lv_buf1 || !lv_buf2) {
    Serial.println("LVGL: PSRAM buffer alloc FAILED -> internal RAM fallback "
                   "(check Tools > PSRAM = OPI PSRAM)");
    if (lv_buf1) free(lv_buf1);
    if (lv_buf2) free(lv_buf2);
    px = LCD_W * 16;
    lv_buf1 = (lv_color_t *)malloc(px * sizeof(lv_color_t));
    lv_buf2 = (lv_color_t *)malloc(px * sizeof(lv_color_t));
  }
  Serial.printf("LVGL buffers: buf1=%p buf2=%p px=%u\n", lv_buf1, lv_buf2,
                (unsigned)px);
  lv_disp_draw_buf_init(&lv_draw_buf, lv_buf1, lv_buf2, px);

  lv_disp_drv_init(&lv_disp_drv);
  lv_disp_drv.hor_res = LCD_W;
  lv_disp_drv.ver_res = LCD_H;
  lv_disp_drv.flush_cb = lvFlush;
  lv_disp_drv.draw_buf = &lv_draw_buf;
  lv_disp_drv_register(&lv_disp_drv);

  lv_indev_drv_init(&lv_touch_drv);
  lv_touch_drv.type = LV_INDEV_TYPE_POINTER;
  lv_touch_drv.read_cb = lvTouchRead;
  lv_indev_drv_register(&lv_touch_drv);
}

// ============================ I2C scan (diag/bring-up) ============================
static void i2cScan() {
  Serial.println("I2C scan:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf("  found 0x%02X\n", a);
  }
}

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);
  delay(600);  // let USB-CDC re-attach so early logs aren't lost
  Serial.println("\n=== ScrollKnob (LVGL) setup ===");

  // Encoder
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  g_restSinceUs = micros();
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encISR, CHANGE);

  // Backlight (start dim-ish; Settings can change it)
  backlightInit();
  backlightWrite(g_brightness);

  // Display + I2C peripherals
  gfx->begin();

  // Panel self-test: solid R/G/B flashes BEFORE LVGL. If you see three clean
  // full-screen colours, the panel + flush path work and any later noise is an
  // LVGL buffer problem (see Serial). If this is already noise, it's the panel
  // bring-up itself. (Blue must look blue; if it looks yellow the byte order is
  // reversed -> flip LV_COLOR_16_SWAP.)
  Serial.println("panel self-test: R/G/B");
  gfx->fillScreen(0xF800); delay(400);  // red
  gfx->fillScreen(0x07E0); delay(400);  // green
  gfx->fillScreen(0x001F); delay(400);  // blue
  gfx->fillScreen(0x0000);              // black

  touchInit();
  g_hapticsPresent = hapticsInit();

  i2cScan();
  Serial.printf("DRV2605 %s\n", g_hapticsPresent ? "present" : "MISSING");

  // BLE (advertises in the background regardless of the active app)
  bleSetup();

  // LVGL + UI
  lvglInit();
  uiInit();

  screenNoteActivity();  // start the idle-blank countdown from here

  Serial.println("=== setup done ===");
}

void loop() {
  // LVGL tick
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  lv_tick_inc(now - lastMs);
  lastMs = now;

  // Encoder -> one uiEncoder(+/-1) + one haptic per detent
  static long lastRaw = 0;
  long raw = g_encPos;
  long delta = raw - lastRaw;
  lastRaw = raw;
  if (delta != 0) {
    if (g_screenBlank) {
      // Dial wakes the screen; swallow the motion so it doesn't also scroll.
      screenWake();
    } else {
      screenNoteActivity();
      int dir = (delta > 0) ? 1 : -1;
      long n = labs(delta);
      for (long i = 0; i < n; i++) {
        appHapticTick();
        uiEncoder(dir);
      }
    }
  }

  // BLE connection edges
  static bool prevConn = false;
  bool conn = g_connected;
  if (conn != prevConn) {
    uiConnChanged(conn);
    prevConn = conn;
  }

  // Blank the panel after SCREEN_TIMEOUT_MS of no dial/touch input (unless
  // Always-On is set).
  screenIdleTick(now);

  // LVGL render, then per-frame UI logic. The order matters: the FPS screen
  // blits its viewport straight to the panel from uiTick(), so LVGL has to have
  // finished painting for the frame before that happens - otherwise a tap (which
  // invalidates the transparent hit target) would repaint background over the
  // freshly drawn 3D view. Other screens only touch LVGL objects here, so
  // running after the render just defers them by one loop.
  lv_timer_handler();
  uiTick();

  delay(3);
}
