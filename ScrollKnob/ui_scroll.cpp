// Scroll Wheel: BLE HID scroll controller. Rotate = one wheel tick (PAGE mode
// = larger delta); press = toggle LINE/PAGE. The viewport scrolls with rotation
// and the active-direction chevron highlights, fading after the last tick.
#include "ui_internal.h"
#include <Arduino.h>
#include <stdio.h>

#define VP_W 118
#define VP_H 150
#define LINE_GAP 22
#define FADE_MS 480

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_up = nullptr;
static lv_obj_t *s_down = nullptr;
static lv_obj_t *s_inner = nullptr;
static lv_obj_t *s_pill = nullptr;

static int s_mode = 0;   // 0 = LINE, 1 = PAGE
static long s_count = 0;
static long s_offset = 0;
static int s_dir = 0;
static uint32_t s_lastTickMs = 0;

static void updatePill(void) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s MODE  \xC2\xB7  %ld", s_mode ? "PAGE" : "LINE", s_count);
  lv_label_set_text(s_pill, buf);
}

static void setChevrons(void) {
  lv_obj_set_style_opa(s_up, s_dir > 0 ? LV_OPA_COVER : 56, 0);
  lv_obj_set_style_opa(s_down, s_dir < 0 ? LV_OPA_COVER : 56, 0);
}

static void scrollInner(void) {
  long m = ((s_offset % (2 * LINE_GAP)) + (2 * LINE_GAP)) % (2 * LINE_GAP);
  lv_obj_set_y(s_inner, (lv_coord_t)(-(2 * LINE_GAP) - m));
}

static void pill_click_cb(lv_event_t *e) { (void)e; appHapticPress(); scroll_press(); }

static void build(void) {
  s_scr = make_screen_base();
  make_back_chevron(s_scr);

  lv_obj_t *title = make_label(s_scr, "SCROLL WHEEL", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 66);

  // Direction chevrons (left of the viewport)
  s_up = make_triangle(s_scr, 22, 14, false, COL_ACCENT);
  lv_obj_align(s_up, LV_ALIGN_CENTER, -78, -12);
  s_down = make_triangle(s_scr, 22, 14, true, COL_ACCENT);
  lv_obj_align(s_down, LV_ALIGN_CENTER, -78, 12);
  setChevrons();

  // Viewport
  lv_obj_t *vp = lv_obj_create(s_scr);
  lv_obj_remove_style_all(vp);
  lv_obj_set_size(vp, VP_W, VP_H);
  lv_obj_align(vp, LV_ALIGN_CENTER, 24, -6);
  lv_obj_set_style_radius(vp, 16, 0);
  lv_obj_set_style_bg_color(vp, lv_color_hex(0x0f0f11), 0);
  lv_obj_set_style_bg_opa(vp, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(vp, 1, 0);
  lv_obj_set_style_border_color(vp, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_opa(vp, 16, 0);
  lv_obj_clear_flag(vp, LV_OBJ_FLAG_SCROLLABLE);

  // Inner "document": repeating horizontal lines, translated by rotation.
  s_inner = lv_obj_create(vp);
  lv_obj_remove_style_all(s_inner);
  lv_obj_set_size(s_inner, VP_W, VP_H + 4 * LINE_GAP);
  lv_obj_set_x(s_inner, 0);
  lv_obj_clear_flag(s_inner, LV_OBJ_FLAG_SCROLLABLE);
  for (int y = 0; y < VP_H + 4 * LINE_GAP; y += LINE_GAP) {
    lv_obj_t *ln = lv_obj_create(s_inner);
    lv_obj_remove_style_all(ln);
    lv_obj_set_size(ln, VP_W - 24, 2);
    lv_obj_set_style_bg_color(ln, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(ln, 22, 0);
    lv_obj_align(ln, LV_ALIGN_TOP_MID, 0, y);
  }
  scrollInner();

  // Mode pill
  s_pill = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_pill, &font_jbm_11, 0);
  lv_obj_set_style_text_color(s_pill, lv_color_hex(COL_PILLTX), 0);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_bg_opa(s_pill, 30, 0);
  lv_obj_set_style_radius(s_pill, 20, 0);
  lv_obj_set_style_pad_hor(s_pill, 14, 0);
  lv_obj_set_style_pad_ver(s_pill, 7, 0);
  lv_obj_set_style_border_width(s_pill, 1, 0);
  lv_obj_set_style_border_color(s_pill, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_style_border_opa(s_pill, 90, 0);
  lv_obj_align(s_pill, LV_ALIGN_BOTTOM_MID, 0, -44);
  lv_obj_add_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_pill, 24);  // easier to tap the mode toggle
  lv_obj_add_event_cb(s_pill, pill_click_cb, LV_EVENT_CLICKED, NULL);
  updatePill();
}

void scroll_show(void) {
  if (!s_scr) build();
  lv_scr_load(s_scr);
}

void scroll_encoder(int dir) {
  int8_t step = (int8_t)(s_mode ? dir * 4 : dir);  // PAGE = larger delta
  appEmitWheel(step);
  s_count++;
  s_offset += (long)LINE_GAP * dir;
  s_dir = dir;
  s_lastTickMs = millis();
  scrollInner();
  setChevrons();
  updatePill();
}

void scroll_press(void) {
  s_mode = !s_mode;
  updatePill();
}

void scroll_tick(void) {
  if (s_dir != 0 && (millis() - s_lastTickMs) >= FADE_MS) {
    s_dir = 0;
    setChevrons();
  }
}
