// Hardware services the main sketch (ScrollKnob.ino) provides to the UI layer.
// The UI never touches Arduino_GFX / NimBLE / Wire directly; it goes through
// these so the render layer stays hardware-agnostic.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// BLE HID mouse wheel: emit `ticks` scroll steps (+up / -down). No-op unless a
// host is connected.
void appEmitWheel(int8_t ticks);

// Forget every Bluetooth bond and drop the current link (host must re-pair).
void appForgetBonds(void);

// True while a BLE host is connected.
bool appConnected(void);

// Backlight brightness, 0..100 %. Driven by an LEDC PWM channel on LCD_BL.
void    appSetBrightness(uint8_t pct);
uint8_t appGetBrightness(void);

// Haptics on/off (Settings toggle) and the three feedback pulses. The pulses
// are silently ignored when haptics are off or no DRV2605 was detected.
void appSetHaptics(bool on);
bool appHapticsEnabled(void);
void appHapticTick(void);   // short click on each detent
void appHapticPress(void);  // stronger click on a press
void appHapticAlarm(void);  // long buzz at timer end

#ifdef __cplusplus
}
#endif
