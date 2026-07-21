// Minimal DRV2605 haptic driver (inline I2C over the shared Wire bus).
// The chip sits on the same I2C bus as the CST816 touch controller
// (SDA=11, SCL=12), at its default address 0x5A.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// DRV2605 built-in effect ids (ERM library 1). Tune to taste.
#define HAPTIC_FX_TICK   7    // "Soft Bump" - light detent feedback
#define HAPTIC_FX_PRESS  1    // "Strong Click" - press confirm
#define HAPTIC_FX_ALARM  47   // "Buzz 1 100%" - timer alarm

#ifdef __cplusplus
extern "C" {
#endif

// Probe + configure the DRV2605. Returns false if no chip answers at 0x5A
// (in which case the app degrades to silent). Call after Wire.begin().
bool hapticsInit(void);

// Fire one built-in effect (see HAPTIC_FX_* above). No-op if init failed.
void hapticsPlay(uint8_t effect);

#ifdef __cplusplus
}
#endif
