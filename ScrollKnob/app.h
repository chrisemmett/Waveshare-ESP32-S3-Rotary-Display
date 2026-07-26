// Hardware services the main sketch (ScrollKnob.ino) provides to the UI layer.
// The UI never touches Arduino_GFX / NimBLE / Wire directly; it goes through
// these so the render layer stays hardware-agnostic.
//
// The user settings behind these calls (brightness, haptics, always-on,
// discoverable) are persisted to NVS by the sketch and restored at boot. The UI
// just calls the setter; saving is deferred and automatic.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BLE HID mouse wheel: emit `ticks` scroll steps (+up / -down). No-op unless a
// host is connected.
void appEmitWheel(int8_t ticks);

// ---- Bluetooth ----
// Addresses are formatted "aa:bb:cc:dd:ee:ff": 17 chars plus the NUL.
#define APP_BT_ADDR_LEN 18
// Most bonds the UI will enumerate. NimBLE's own store is normally smaller
// (CONFIG_BT_NIMBLE_MAX_BONDS, 3 by default), so this is just a safe ceiling.
#define APP_BT_MAX_BONDS 8

// True while a BLE host is connected.
bool appConnected(void);

// The advertised name - what a host sees in its Bluetooth picker.
const char *appBtName(void);

// Drop the current link but keep the bond, so the host can come back. Most
// hosts reconnect within seconds if they are still in range and we are
// discoverable, so this is "hand the wheel to another machine", not "off".
void appBtDisconnect(void);

// Discoverable = we advertise, which is how a new host finds us AND how a
// bonded host reconnects. Turning it off stops advertising outright: nothing
// can connect until it goes back on. Advertising also pauses by itself while a
// host is connected (nothing else could connect anyway) and resumes when that
// host drops.
void appBtSetDiscoverable(bool on);
bool appBtDiscoverable(void);
bool appBtAdvertising(void);  // really broadcasting right now

// Identity address of the connected host; writes "" when there is no link.
void appBtPeerAddr(char *out, uint32_t len);

// Bonded (paired) hosts. This is NimBLE's bond store, so it survives reboots.
// Indices are only valid until the list changes - re-read after a forget.
uint8_t appBtBondCount(void);
bool    appBtBondAddr(uint8_t idx, char *out, uint32_t len);
bool    appBtBondIsConnected(uint8_t idx);
void    appBtForgetBond(uint8_t idx);  // drops the link too if it is that host
void    appBtForgetAll(void);          // forget every bond + drop the link

// Backlight brightness, 0..100 %. Driven by an LEDC PWM channel on LCD_BL.
void    appSetBrightness(uint8_t pct);
uint8_t appGetBrightness(void);

// Always-On display (Settings toggle). OFF: the screen blanks after
// SCREEN_TIMEOUT_MS of no dial/touch input, and the next input wakes it without
// reaching the UI. ON: the screen never blanks (and lights up immediately if it
// was already blank).
void appSetAlwaysOn(bool on);
bool appAlwaysOn(void);

// Light the screen and restart the idle countdown - for events the user must
// see even if they weren't touching the device (e.g. the timer alarm).
void appWakeScreen(void);

// Haptics on/off (Settings toggle) and the three feedback pulses. The pulses
// are silently ignored when haptics are off or no DRV2605 was detected.
void appSetHaptics(bool on);
bool appHapticsEnabled(void);
void appHapticTick(void);   // short click on each detent
void appHapticPress(void);  // stronger click on a press
void appHapticAlarm(void);  // long buzz at timer end

// Hardware RNG (esp_random) - used by the Safe Cracker combo generator.
uint32_t appRandom(void);

// ---- Direct-to-panel rendering (Dial of Doom) ----
// The raycaster renders its own framebuffer bands and pushes them straight at
// the panel; going through LVGL would mean an extra full-screen copy per frame.
// `px` is RGB565 in the panel's byte order (big-endian) - the same format
// LVGL's flush callback hands over on this board.
void appBlit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px);

// DMA-capable internal-RAM allocation, for buffers handed to appBlit(). Returns
// NULL if the request can't be satisfied.
void *appDmaAlloc(uint32_t bytes);

// Reset the idle-blank countdown. For screens that animate without input and
// must not blank mid-frame (the FPS runs itself; only the dial is an input).
void appKeepAwake(void);

#ifdef __cplusplus
}
#endif
