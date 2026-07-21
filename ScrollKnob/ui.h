// UI services the render layer (ui_*.cpp) provides back to the main sketch.
// The main loop feeds hardware input in through these; the UI owns all LVGL.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void uiInit(void);              // build all screens, show the launcher
void uiTick(void);              // per-frame logic (timer 1 Hz, chevron fade)
void uiEncoder(int32_t dir);    // one detent: +1 / -1, routed to the active screen
void uiPress(void);             // knob press -> active screen's confirm action
void uiBack(void);              // long-press / MENU chevron -> launcher
void uiConnChanged(bool connected);  // BLE link edge (repaints BT-aware screens)

#ifdef __cplusplus
}
#endif
