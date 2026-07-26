// Shared internals for the ui_*.cpp render layer. Not seen by the hardware .ino
// (which only uses ui.h / app.h).
#pragma once
#include <lvgl.h>
#include "app.h"
#include "fonts_knob.h"

// ---- Design tokens (from the handoff) ----
#define COL_ACCENT  0xe9a44c
#define COL_ACCENT2 0xf2bd76
#define COL_PILLTX  0xf0c384
#define COL_GREEN   0x8ec8a0
#define COL_RED     0xe0705e
#define COL_TX      0xf4efe6
#define COL_TX2     0xe6e2db
#define COL_DIM     0x8b877f
#define COL_FAINT   0x5f5c56
#define COL_FAINT2  0x6b675f
#define COL_BG      0x0b0b0d
#define COL_TRACK   0x232326  // approx rgba(255,255,255,.07) over bg

#define SCR_W 360
#define SCR_CX 180
#define SCR_CY 180

// ---- Screens ----
// SCR_BT hangs off Settings rather than the launcher ring - it is a control
// panel for the BLE link, not an app.
enum ScreenId { SCR_LAUNCHER, SCR_SCROLL, SCR_TIMER, SCR_SETTINGS, SCR_GAME, SCR_FPS, SCR_BT };
void navGo(ScreenId id);
ScreenId navCurrent(void);

// ---- Shared helpers (ui.cpp) ----
lv_obj_t *make_screen_base(void);
lv_obj_t *make_triangle(lv_obj_t *parent, int w, int h, bool pointDown, uint32_t color);
lv_obj_t *make_marker(lv_obj_t *parent);        // accent triangle at 12 o'clock
lv_obj_t *make_back_chevron(lv_obj_t *parent);  // MENU chevron, bottom-centre
// Same affordance pointing somewhere other than the launcher (nested screens).
lv_obj_t *make_back_chevron_to(lv_obj_t *parent, ScreenId dest, const char *label);
lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);

// ---- Per-screen entry points (each ui_*.cpp) ----
void launcher_show(void);   void launcher_encoder(int dir); void launcher_press(void);
void scroll_show(void);     void scroll_encoder(int dir);   void scroll_press(void);   void scroll_tick(void);
void timer_show(void);      void timer_encoder(int dir);    void timer_press(void);    void timer_tick(void);
void settings_show(void);   void settings_encoder(int dir); void settings_press(void); void settings_conn_changed(void);
void bt_show(void);         void bt_encoder(int dir);       void bt_press(void);       void bt_conn_changed(void);  void bt_tick(void);
void game_show(void);       void game_encoder(int dir);     void game_tick(void);
void fps_show(void);        void fps_encoder(int dir);      void fps_tick(void);
