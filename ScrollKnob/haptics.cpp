// DRV2605 haptic driver - minimal register-level control over Wire.
// Datasheet registers used: MODE(0x01), LIBRARY(0x03), WAVESEQ0/1(0x04/0x05),
// GO(0x0C), FEEDBACK(0x1A).
#include "haptics.h"
#include <Arduino.h>
#include <Wire.h>

#define DRV2605_ADDR 0x5A

static bool s_present = false;

static void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DRV2605_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool hapticsInit(void) {
  // Presence check: does anything ACK at 0x5A?
  Wire.beginTransmission(DRV2605_ADDR);
  if (Wire.endTransmission() != 0) {
    s_present = false;
    return false;
  }
  wr(0x01, 0x00);  // MODE: exit standby, internal trigger
  // Default to the ERM effect library (1). If this board carries an LRA motor,
  // switch to library 6 and set the LRA bit in FEEDBACK (0x1A) - see README.
  wr(0x1A, 0x36);  // FEEDBACK_CTRL: ERM open-loop defaults
  wr(0x03, 0x01);  // LIBRARY: 1 (ERM)
  wr(0x04, HAPTIC_FX_PRESS);
  wr(0x05, 0x00);  // end of sequence
  s_present = true;
  return true;
}

void hapticsPlay(uint8_t effect) {
  if (!s_present) return;
  wr(0x04, effect);  // waveform slot 0 = effect
  wr(0x05, 0x00);    // slot 1 = end
  wr(0x0C, 0x01);    // GO
}
