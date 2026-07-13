/*
 * ScrollKnob - turn the knob into a scroll wheel.
 *
 * Target board : Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 *                (panel module "JC3636K518": ST77916 QSPI LCD, 360x360,
 *                 CST816 capacitive touch)
 *
 * Two build modes, selected by USE_BLE below:
 *
 *   USE_BLE 0  (USB)  - acts as a USB mouse wheel and shows the scroll
 *                       direction / speed on the round display (original behaviour).
 *
 *   USE_BLE 1  (BLE)  - acts as a *Bluetooth Low Energy* mouse wheel:
 *                        - While unpaired it advertises and the screen shows
 *                          "DISCOVERABLE".
 *                        - Once a host connects, the display + backlight turn OFF
 *                          (the knob still scrolls).
 *                        - Tapping the dark screen brings up a "DISCONNECT" button.
 *                        - Tapping DISCONNECT forgets ALL Bluetooth bonds and drops
 *                          the link (the host must re-pair).
 *                        - If no button is tapped within 10 s, the screen turns off
 *                          again.
 *
 * FIXES CARRIED FROM BOARD BRING-UP (see README "Lessons learned")
 *   1. DISPLAY: the JC3636K518 panel needs the ST77916 "150" init sequence
 *      (Arduino_ST77916 defaults to "180", which produced a garbled screen).
 *   2. ENCODER: a debounced excursion decoder, not a textbook quadrature decoder,
 *      because this knob rests at A=B=1 and drops a single channel per detent.
 *
 * ---------------------------------------------------------------------------
 * LIBRARIES (Arduino IDE -> Tools -> Manage Libraries...):
 *   1. "GFX Library for Arduino"  by moononournation   (ST77916 + QSPI panel)
 *   2. (BLE mode only) "NimBLE-Arduino" by h2zero, version 2.x
 *                                          (BLE HID mouse; small footprint)
 *   USB HID (USB mode only) needs no extra library - it ships with the core.
 *
 * BOARD SETTINGS (Arduino IDE -> Tools):
 *   Board:            "ESP32S3 Dev Module" (or the Waveshare profile)
 *   PSRAM:            "OPI PSRAM" / "Enabled"
 *   USB CDC On Boot:  "Enabled"            (keeps Serial working over USB)
 *   USB Mode:         "USB-OTG (TinyUSB)"  (required for USB mode; harmless in BLE mode)
 *   Flash Size:       "16MB (128Mb)"
 *   Partition Scheme: any WITHOUT an "ESP SR ... MODEL" partition
 *                     (e.g. "16M Flash (3MB APP/9.9MB FATFS)") - the 3MB app
 *                     partition has room for the NimBLE stack.
 * ---------------------------------------------------------------------------
 */

// PIN-REMAP GUARD: this sketch ships a build_opt.h (same folder) that globally
// defines BOARD_USES_HW_GPIO_NUMBERS. That stops the ESP32 core from turning
// pinMode/digitalRead/digitalWrite into "pin remap" macros on board profiles
// that define BOARD_HAS_PIN_REMAP. Without it, those macros collide with
// Arduino_GFX's I/O-expander headers and the build dies with
// "'digitalPinToGPIONumber' is not a type". build_opt.h is applied to EVERY
// translation unit including Arduino_GFX's own .cpp, which a sketch-level #undef
// cannot reach. See the README if your toolchain ignores build_opt.h.
#include <Arduino_GFX_Library.h>

// ============================ Feature toggles ============================
// Master transport switch.
//   1 = Bluetooth Low Energy mouse wheel (requires NimBLE-Arduino)
//   0 = USB mouse wheel (original behaviour)
#define USE_BLE 1

#if USE_BLE
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>  // NimBLEDevice.h does not pull this in (NimBLE 2.x)
#include <Wire.h>
#else
// USB HID scroll wheel (only meaningful when USE_BLE == 0).
#define ENABLE_USB_HID 1
#endif

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
// Panel orientation passed to Arduino_GFX: 0 = normal, 1/2/3 = 90/180/270 deg.
// Set to 2 to mount the board upside-down and still read right-side-up. NB: the
// touch controller reports RAW panel coordinates, so tpRead() mirrors them to
// match whenever this is 2 (see the guard there).
#define LCD_ROTATION 2

// Touch (CST816 capacitive controller, I2C)
#define TP_SDA 11
#define TP_SCL 12
#define TP_INT 9
#define TP_RST 10
#define CST816_ADDR 0x15

// Rotary encoder (the knob)
#define ENC_A_PIN 8
#define ENC_B_PIN 7

// Push button (pressing the knob) - GPIO0, active low.
#define BTN_PIN 0

// ============================ Tunables ============================
// The decoder emits exactly one count per physical detent click, so this is 1.
#define PULSES_PER_DETENT 1

#define VEL_WINDOW_MS 80      // how often speed is recomputed & UI redrawn (USB mode)
#define WHEEL_INVERT false    // flip if scroll direction feels backwards
#define VEL_FULL_SCALE 25.0f  // detents/sec that fills the speed bar (USB mode feel)

// How long the DISCONNECT button stays up before the screen goes dark again.
#define DISCONNECT_PROMPT_MS 10000

// ============================ Colours (RGB565) ============================
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
static const uint16_t C_BG = RGB565(0, 0, 0);           // black
static const uint16_t C_TITLE = RGB565(150, 150, 150);  // dim grey text
static const uint16_t C_IDLE = RGB565(90, 90, 90);      // grey  - not moving
static const uint16_t C_UP = RGB565(40, 200, 90);       // green - scrolling up / discoverable
static const uint16_t C_DOWN = RGB565(240, 140, 30);    // orange- scrolling down / disconnect
static const uint16_t C_TRACK = RGB565(35, 35, 35);     // speed-bar background
static const uint16_t C_WHITE = RGB565(255, 255, 255);  // button text

// ============================ Types ============================
// Declared up here (before any function) so the Arduino IDE's auto-generated
// function prototypes can see them.
enum Dir { DIR_IDLE,
           DIR_UP,
           DIR_DOWN };

#if USE_BLE
// BLE-mode display states.
enum BleUi { BLE_UI_DISCOVERABLE,  // unpaired, advertising, "DISCOVERABLE" on screen
             BLE_UI_OFF,           // connected, screen + backlight dark
             BLE_UI_PROMPT };      // connected, showing the DISCONNECT button
#endif

// ============================ Display objects ============================
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
// IMPORTANT: pass the "150" init sequence for the JC3636K518 panel (see README).
Arduino_GFX *gfx = new Arduino_ST77916(
  bus, LCD_RST, LCD_ROTATION /* rotation */, true /* IPS */, LCD_W, LCD_H,
  0, 0, 0, 0,
  st77916_150_init_operations, sizeof(st77916_150_init_operations));

// ============================ Encoder (debounced detent-excursion decoder) ============================
// This knob rests at A=B=1 and, per physical click, briefly drops ONE channel
// and returns WITHOUT completing a full quadrature cycle. See README "Lesson 4".
#define REST_QUIET_US 5000  // require this much quiet-at-rest before a new count

volatile long g_encPos = 0;           // accumulated detents (signed)
volatile int8_t g_pendingDir = 0;     // direction of the click in progress (0 = none)
volatile uint32_t g_restSinceUs = 0;  // micros() of the most recent rest sighting

static inline uint8_t readAB() {
  return (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
}

void IRAM_ATTR encISR() {
  uint8_t ab = readAB();
  uint32_t nowUs = micros();

  if (ab == 0b11) {           // at a detent (rest)
    if (g_pendingDir != 0) {  // a real click just finished -> commit it
      g_encPos += g_pendingDir;
      g_pendingDir = 0;
    }
    g_restSinceUs = nowUs;  // (re)start the quiet timer; bounce keeps resetting it
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

// ============================ BLE HID mouse ============================
#if USE_BLE
// Standard HID report map: a 3-button mouse with X / Y / Wheel, Report ID 1.
// The input report we send is 4 bytes: {buttons, dX, dY, wheel}. For scrolling
// we only ever touch the wheel byte.
static const uint8_t HID_REPORT_MAP[] = {
  0x05, 0x01,  // Usage Page (Generic Desktop)
  0x09, 0x02,  // Usage (Mouse)
  0xA1, 0x01,  // Collection (Application)
  0x85, 0x01,  //   Report ID (1)
  0x09, 0x01,  //   Usage (Pointer)
  0xA1, 0x00,  //   Collection (Physical)
  0x05, 0x09,  //     Usage Page (Buttons)
  0x19, 0x01,  //     Usage Minimum (1)
  0x29, 0x03,  //     Usage Maximum (3)
  0x15, 0x00,  //     Logical Minimum (0)
  0x25, 0x01,  //     Logical Maximum (1)
  0x95, 0x03,  //     Report Count (3)
  0x75, 0x01,  //     Report Size (1)
  0x81, 0x02,  //     Input (Data,Var,Abs) - 3 buttons
  0x95, 0x01,  //     Report Count (1)
  0x75, 0x05,  //     Report Size (5)
  0x81, 0x03,  //     Input (Const)        - 5 bits padding
  0x05, 0x01,  //     Usage Page (Generic Desktop)
  0x09, 0x30,  //     Usage (X)
  0x09, 0x31,  //     Usage (Y)
  0x09, 0x38,  //     Usage (Wheel)
  0x15, 0x81,  //     Logical Minimum (-127)
  0x25, 0x7F,  //     Logical Maximum (127)
  0x75, 0x08,  //     Report Size (8)
  0x95, 0x03,  //     Report Count (3)
  0x81, 0x06,  //     Input (Data,Var,Rel) - X, Y, Wheel
  0xC0,        //   End Collection
  0xC0         // End Collection
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
    // Go back to being discoverable so a host (this one again, or a new one)
    // can pair.
    NimBLEDevice::startAdvertising();
  }
};

static void bleSetup() {
  NimBLEDevice::init("ScrollKnob");
  // Just-Works bonding (bond, no MITM, secure connections). A mouse has no
  // display/keypad, so we advertise NoInputNoOutput and pair without a PIN.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  bleHid = new NimBLEHIDDevice(bleServer);
  bleInput = bleHid->getInputReport(1);  // report ID 1
  bleHid->setManufacturer("DIY");
  bleHid->setPnp(0x02, 0xe502, 0xa111, 0x0210);
  bleHid->setHidInfo(0x00, 0x01);
  bleHid->setReportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  bleHid->setBatteryLevel(100);
  bleHid->startServices();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C2);  // HID mouse
  adv->addServiceUUID(bleHid->getHidService()->getUUID());
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

static void bleSendWheel(int8_t whl) {
  if (!g_connected || bleInput == nullptr) return;
  uint8_t report[4] = { 0, 0, 0, (uint8_t)whl };  // buttons, dX, dY, wheel
  bleInput->setValue(report, sizeof(report));
  bleInput->notify();
}

// Drop the current link AND forget every stored bond, so the host has to
// re-pair from scratch. onDisconnect() then restarts advertising.
static void bleForgetAll() {
  NimBLEDevice::deleteAllBonds();
  if (g_connected) {
    bleServer->disconnect(g_connHandle);
  }
}

// ============================ Touch (CST816) ============================
static void touchInit() {
  Wire.begin(TP_SDA, TP_SCL, 400000);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);  // controller boot time
  pinMode(TP_INT, INPUT_PULLUP);
}

// Reads the current touch point. Returns true if a finger is down and fills
// x/y with panel-pixel coordinates (0..359).
// NB: named tpRead(), not touchRead() - the ESP32 core defines a touchRead(pin)
// macro for its built-in capacitive-touch pins, which would clash.
static bool tpRead(int *x, int *y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x01);  // start at the gesture register
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  uint8_t fingers = d[1] & 0x0F;  // reg 0x02: number of touch points
  if (fingers == 0) return false;
  if (x) *x = ((d[2] & 0x0F) << 8) | d[3];  // reg 0x03/0x04: X
  if (y) *y = ((d[4] & 0x0F) << 8) | d[5];  // reg 0x05/0x06: Y
#if LCD_ROTATION == 2
  // The CST816 reports raw panel coordinates that never pass through the GFX
  // rotation, so with the display flipped 180 the axes must be mirrored here to
  // stay aligned with what's drawn (a 180 flip mirrors both axes, no swap).
  if (x) *x = (LCD_W - 1) - *x;
  if (y) *y = (LCD_H - 1) - *y;
#endif
  return true;
}

// Detects a fresh finger-down (a "tap"), throttled and edge-triggered so one
// press yields exactly one event. Fills x/y with where the finger landed.
static bool pollTap(int *x, int *y) {
  static uint32_t lastMs = 0;
  static bool wasDown = false;
  uint32_t now = millis();
  if (now - lastMs < 20) return false;  // poll ~50 Hz
  lastMs = now;

  int tx = 0, ty = 0;
  bool down = tpRead(&tx, &ty);
  bool tap = down && !wasDown;
  wasDown = down;
  if (tap) {
    if (x) *x = tx;
    if (y) *y = ty;
  }
  return tap;
}
#endif  // USE_BLE

#if !USE_BLE && ENABLE_USB_HID
#include "USB.h"
#include "USBHIDMouse.h"
USBHIDMouse Mouse;
#endif

// ============================ State ============================
static long lastRaw = 0;      // last raw encoder count
static long scrollAccum = 0;  // raw counts waiting to become HID steps

#if !USE_BLE
static long velAccum = 0;  // raw counts inside the current speed window
static uint32_t lastVelMs = 0;
static float detentsPerSec = 0;  // signed: + one way, - the other

// what is currently drawn, so we only repaint what changed
static Dir drawnDir = (Dir)-1;
static long drawnPos = 0x7FFFFFFF;
static int drawnBar = -1;
#endif

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
static const int CY = LCD_H / 2;  // 180

#if !USE_BLE
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
#endif  // !USE_BLE

// ============================ BLE-mode UI ============================
#if USE_BLE
#include "sprites.h"  // running-man run cycle (RUN_FRAMES, drawSprite)

static BleUi bleUi = BLE_UI_DISCOVERABLE;
static uint32_t promptSinceMs = 0;

// ---- Idle running-man animation ----
// Shown while the screen is dark (BLE_UI_OFF, i.e. connected) AND the knob is
// being turned. Frames advance with the wheel (faster turning = faster running)
// and the runner mirrors to the scroll direction. After RUN_IDLE_MS of no
// motion the screen blanks again. bleUi stays BLE_UI_OFF the whole time so the
// existing tap-to-DISCONNECT path keeps working.
static bool g_running = false;         // runner currently on screen
static uint32_t lastRunMotionMs = 0;   // millis() of the last wheel movement
static uint16_t runFrameAccum = 0;     // |delta| accumulated toward next frame
static uint8_t runFrame = 0;           // index into RUN_FRAMES
static bool g_runFacing = false;       // flipH: true = running left
#define RUN_IDLE_MS 1500               // blank after this long with no turning
#define RUN_STEP_DETENTS 1             // detents per frame step (higher = slower legs)

// The DISCONNECT button, centred on the round screen.
#define DBTN_W 240
#define DBTN_H 90
static const int DBTN_X = (LCD_W - DBTN_W) / 2;  // 60
static const int DBTN_Y = (LCD_H - DBTN_H) / 2;  // 135

static bool inDisconnectBtn(int x, int y) {
  return x >= DBTN_X && x < DBTN_X + DBTN_W && y >= DBTN_Y && y < DBTN_Y + DBTN_H;
}

static void drawDiscoverable() {
  gfx->fillScreen(C_BG);
  drawCentered("BLUETOOTH", CX, 110, 2, C_TITLE, C_BG);
  drawCentered("DISCOVERABLE", CX, 150, 3, C_UP, C_BG);
  drawCentered("pair from your PC", CX, 210, 1, C_TITLE, C_BG);
}

static void drawDisconnectPrompt() {
  gfx->fillScreen(C_BG);
  gfx->fillRoundRect(DBTN_X, DBTN_Y, DBTN_W, DBTN_H, 16, C_DOWN);
  // vertically centre size-3 text (8*3 = 24px tall) inside the button
  drawCentered("DISCONNECT", CX, DBTN_Y + (DBTN_H - 24) / 2, 3, C_WHITE, C_DOWN);
}

// State transitions. Each one both updates bleUi and drives the panel/backlight.
static void bleShowDiscoverable() {
  bleUi = BLE_UI_DISCOVERABLE;
  drawDiscoverable();
  digitalWrite(LCD_BL, HIGH);
}

static void bleGoDark() {
  bleUi = BLE_UI_OFF;
  g_running = false;          // cancel any running-man animation
  runFrameAccum = 0;
  gfx->fillScreen(C_BG);      // clear content so nothing shows if the LED glows
  digitalWrite(LCD_BL, LOW);  // backlight off
}

// Wake the dark screen into the running-man animation (first frame).
static void runnerStart() {
  g_running = true;
  runFrame = 0;
  runFrameAccum = 0;
  gfx->fillScreen(C_BG);  // clean slate under the runner
  digitalWrite(LCD_BL, HIGH);
  drawSprite(runFrame, RUN_SCALE, CX, CY, C_BG, g_runFacing);
}

static void bleShowPrompt() {
  bleUi = BLE_UI_PROMPT;
  promptSinceMs = millis();
  drawDisconnectPrompt();
  digitalWrite(LCD_BL, HIGH);
}
#endif  // USE_BLE

// ============================ setup / loop ============================
void setup() {
  Serial.begin(115200);
#if DIAG
  delay(4000);  // give the USB host time to re-attach so no early logs are lost
  Serial.println();
  Serial.println("=== DIAG: setup start ===");
#endif

  // --- Encoder: interrupt-driven excursion decoder on the two knob channels ---
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  g_restSinceUs = micros();  // start the quiet-at-rest timer
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encISR, CHANGE);

  // --- Button ---
  pinMode(BTN_PIN, INPUT_PULLUP);

  // --- Backlight (kept off until we decide what to show) ---
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);

  // --- Display ---
  gfx->begin();

#if USE_BLE
  touchInit();
  bleSetup();
  bleShowDiscoverable();  // advertising -> screen shows "DISCOVERABLE"
#else
#if ENABLE_USB_HID
  Mouse.begin();
  USB.begin();
#endif
  digitalWrite(LCD_BL, HIGH);
  drawStaticUI();
  lastVelMs = millis();
#endif

#if DIAG
  Serial.println("=== DIAG: setup DONE ===");
#endif
}

void loop() {
  // --- Read the knob (updated in the ISR) ---
  long raw = g_encPos;
  long delta = raw - lastRaw;
  lastRaw = raw;

#if USE_BLE
  // ---- Bluetooth mode ----
  // Scroll only counts once a host is connected; drop motion while unpaired so
  // it doesn't jump the moment we connect.
  if (delta != 0) {
    if (g_connected) {
      scrollAccum += delta;
      while (labs(scrollAccum) >= PULSES_PER_DETENT) {
        int step = (scrollAccum > 0) ? 1 : -1;
        int8_t whl = WHEEL_INVERT ? -step : step;  // +ve wheel scrolls up
        bleSendWheel(whl);
        scrollAccum -= step * PULSES_PER_DETENT;
      }
    } else {
      scrollAccum = 0;
    }
  }

  // Connection edges: connect -> screen off; disconnect -> discoverable.
  static bool prevConn = false;
  bool conn = g_connected;
  if (conn && !prevConn) bleGoDark();
  if (!conn && prevConn) bleShowDiscoverable();
  prevConn = conn;

  // Idle running-man: while the screen is dark and the wheel is turning, wake it
  // and run the cycle; blank again after a short pause. Runs before the touch
  // handler so a tap can still override the animation with the DISCONNECT button.
  if (bleUi == BLE_UI_OFF) {
    if (delta != 0) {  // BLE_UI_OFF implies g_connected
      g_runFacing = (delta < 0);  // mirror the runner to the scroll direction
      if (!g_running) runnerStart();
      lastRunMotionMs = millis();
      runFrameAccum += (uint16_t)labs(delta);
      while (runFrameAccum >= RUN_STEP_DETENTS) {
        runFrame = (runFrame + 1) % RUN_FRAME_COUNT;
        runFrameAccum -= RUN_STEP_DETENTS;
        drawSprite(runFrame, RUN_SCALE, CX, CY, C_BG, g_runFacing);
      }
    } else if (g_running && (millis() - lastRunMotionMs >= RUN_IDLE_MS)) {
      bleGoDark();  // no motion for a while -> back to blank (clears g_running)
    }
  }

  // Touch: wake to the DISCONNECT button, or act on a tap of it.
  int tx = 0, ty = 0;
  if (pollTap(&tx, &ty)) {
    if (bleUi == BLE_UI_OFF) {
      g_running = false;  // runner (if any) yields to the DISCONNECT prompt
      bleShowPrompt();    // dark screen tapped -> reveal the button
    } else if (bleUi == BLE_UI_PROMPT && inDisconnectBtn(tx, ty)) {
      bleForgetAll();  // onDisconnect() -> bleShowDiscoverable() on the next loop
    }
  }

  // If the button sat untouched for the timeout, go dark again.
  if (bleUi == BLE_UI_PROMPT && (millis() - promptSinceMs >= DISCONNECT_PROMPT_MS)) {
    bleGoDark();
  }

  delay(2);
  return;
#else
  // ---- USB mode (original behaviour) ----
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
#endif
}
