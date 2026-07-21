// Countdown timer: full-bleed progress ring + MM:SS. Rotate (when not running)
// = +/-30 s; press = start/pause; press when finished = reset.
#include "ui_internal.h"
#include <Arduino.h>
#include <stdio.h>

#define TM_STEP 30
#define TM_MAX 3599

enum TState { TS_IDLE, TS_RUN, TS_PAUSE, TS_DONE };

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_arc = nullptr;
static lv_obj_t *s_time = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_anim_t s_flash;

static TState s_state = TS_IDLE;
static int s_set = 5 * 60;   // chosen duration (also the ring denominator)
static int s_left = 5 * 60;  // remaining seconds
static uint32_t s_endMs = 0;

static void flash_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_arc_opa((lv_obj_t *)obj, v, LV_PART_INDICATOR);
}
static void startFlash(void) {
  lv_anim_init(&s_flash);
  lv_anim_set_var(&s_flash, s_arc);
  lv_anim_set_exec_cb(&s_flash, flash_opa_cb);
  lv_anim_set_values(&s_flash, 38, 180);
  lv_anim_set_time(&s_flash, 300);
  lv_anim_set_playback_time(&s_flash, 300);
  lv_anim_set_repeat_count(&s_flash, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&s_flash);
}
static void stopFlash(void) {
  lv_anim_del(s_arc, flash_opa_cb);
  lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void redraw(void) {
  int denom = s_set > 0 ? s_set : 1;
  lv_arc_set_range(s_arc, 0, denom);
  lv_arc_set_value(s_arc, s_left);

  uint32_t ind = COL_FAINT;   // idle grey (#5f5c56 ~ design #63605a)
  if (s_state == TS_RUN || s_state == TS_PAUSE) ind = COL_ACCENT;
  else if (s_state == TS_DONE) ind = COL_RED;
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(ind), LV_PART_INDICATOR);

  char buf[24];
  snprintf(buf, sizeof(buf), "%02d#5f5c56 :#%02d", s_left / 60, s_left % 60);
  lv_label_set_text(s_time, buf);

  const char *st;
  uint32_t sc;
  if (s_state == TS_RUN)        { st = "RUNNING";      sc = COL_GREEN; }
  else if (s_state == TS_PAUSE) { st = "PAUSED";       sc = COL_DIM;   }
  else if (s_state == TS_DONE)  { st = "TIME'S UP";    sc = COL_RED;   }
  else if (s_left == 0)         { st = "TURN TO SET";  sc = COL_FAINT; }
  else                          { st = "TAP TO START"; sc = COL_DIM; }
  lv_label_set_text(s_status, st);
  lv_obj_set_style_text_color(s_status, lv_color_hex(sc), 0);
}

static void center_click_cb(lv_event_t *e) {
  (void)e;
  appHapticPress();
  timer_press();
}

static void build(void) {
  s_scr = make_screen_base();
  make_marker(s_scr);
  make_back_chevron(s_scr);

  s_arc = lv_arc_create(s_scr);
  lv_obj_set_size(s_arc, 322, 322);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 270);
  lv_arc_set_bg_angles(s_arc, 0, 360);
  lv_arc_set_range(s_arc, 0, s_set);
  lv_arc_set_value(s_arc, s_left);
  lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);  // hide the knob
  lv_obj_set_style_pad_all(s_arc, 0, LV_PART_KNOB);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, 30, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, 30, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

  s_time = lv_label_create(s_scr);
  lv_label_set_recolor(s_time, true);
  lv_obj_set_style_text_font(s_time, &font_jbm_52, 0);
  lv_obj_set_style_text_color(s_time, lv_color_hex(COL_TX), 0);
  lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -14);
  lv_obj_add_flag(s_time, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_time, center_click_cb, LV_EVENT_CLICKED, NULL);

  s_status = make_label(s_scr, "TAP TO START", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(s_status, 2, 0);
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 34);

  // Large transparent centre target: tap anywhere inside the ring to
  // start/pause/reset (the MM:SS label alone was too small to hit).
  lv_obj_t *hit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(hit);
  lv_obj_set_size(hit, 210, 210);
  lv_obj_center(hit);
  lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hit, center_click_cb, LV_EVENT_CLICKED, NULL);

  redraw();
}

void timer_show(void) {
  if (!s_scr) build();
  else redraw();
  lv_scr_load(s_scr);
}

void timer_encoder(int dir) {
  if (s_state == TS_RUN || s_state == TS_DONE) return;  // only settable when idle/paused
  int v = s_left + dir * TM_STEP;
  if (v < 0) v = 0;
  if (v > TM_MAX) v = TM_MAX;
  s_left = v;
  s_set = v;
  redraw();
}

void timer_press(void) {
  switch (s_state) {
    case TS_IDLE:
    case TS_PAUSE:
      if (s_left > 0) {
        s_endMs = millis() + (uint32_t)s_left * 1000;
        s_state = TS_RUN;
        redraw();
      }
      break;
    case TS_RUN: {
      long ms = (long)(s_endMs - millis());
      s_left = ms > 0 ? (int)((ms + 999) / 1000) : 0;
      s_state = TS_PAUSE;
      redraw();
      break;
    }
    case TS_DONE:
      stopFlash();
      s_left = s_set;
      s_state = TS_IDLE;
      redraw();
      break;
  }
}

void timer_tick(void) {
  if (s_state != TS_RUN) return;
  long ms = (long)(s_endMs - millis());
  if (ms <= 0) {
    s_left = 0;
    s_state = TS_DONE;
    redraw();
    startFlash();
    appHapticAlarm();
    return;
  }
  int sec = (int)((ms + 999) / 1000);
  if (sec != s_left) {
    s_left = sec;
    redraw();
  }
}
