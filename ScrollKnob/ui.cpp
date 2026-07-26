// UI manager + shared widget helpers.
#include "ui.h"
#include "ui_internal.h"

// ============================ Screen manager ============================
static ScreenId s_cur = SCR_LAUNCHER;

ScreenId navCurrent(void) { return s_cur; }

void navGo(ScreenId id) {
  s_cur = id;
  switch (id) {
    case SCR_LAUNCHER: launcher_show(); break;
    case SCR_SCROLL:   scroll_show();   break;
    case SCR_TIMER:    timer_show();    break;
    case SCR_SETTINGS: settings_show(); break;
    case SCR_GAME:     game_show();     break;
    case SCR_FPS:      fps_show();      break;
  }
}

// ============================ ui.h implementation ============================
void uiInit(void) {
  navGo(SCR_LAUNCHER);
}

void uiEncoder(int32_t dir) {
  switch (s_cur) {
    case SCR_LAUNCHER: launcher_encoder(dir); break;
    case SCR_SCROLL:   scroll_encoder(dir);   break;
    case SCR_TIMER:    timer_encoder(dir);    break;
    case SCR_SETTINGS: settings_encoder(dir); break;
    case SCR_GAME:     game_encoder(dir);     break;
    case SCR_FPS:      fps_encoder(dir);      break;
  }
}

void uiPress(void) {
  switch (s_cur) {
    case SCR_LAUNCHER: launcher_press(); break;
    case SCR_SCROLL:   scroll_press();   break;
    case SCR_TIMER:    timer_press();    break;
    case SCR_SETTINGS: settings_press(); break;
  }
}

void uiBack(void) {
  if (s_cur != SCR_LAUNCHER) navGo(SCR_LAUNCHER);
}

void uiTick(void) {
  if (s_cur == SCR_SCROLL) scroll_tick();
  else if (s_cur == SCR_TIMER) timer_tick();
  else if (s_cur == SCR_GAME) game_tick();
  else if (s_cur == SCR_FPS) fps_tick();
}

void uiConnChanged(bool connected) {
  (void)connected;
  if (s_cur == SCR_SETTINGS) settings_conn_changed();
}

// ============================ Shared helpers ============================
lv_obj_t *make_screen_base(void) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, SCR_W, SCR_W);
  // Solid dark background. A gradient over this near-black range bands badly on
  // RGB565 (and reads green, since green has 6 bits vs 5) - COL_BG is neutral in
  // 565, and a flat fill can't band.
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

// A filled triangle rendered on a small alpha canvas. pointDown=true -> apex at
// bottom; false -> apex at top. Buffer is allocated from the LVGL heap and kept
// for the life of the (persistent) screen.
lv_obj_t *make_triangle(lv_obj_t *parent, int w, int h, bool pointDown, uint32_t color) {
  uint32_t bufsz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(w, h);
  void *buf = lv_mem_alloc(bufsz);
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, buf, w, h, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(color);
  d.bg_opa = LV_OPA_COVER;

  lv_point_t pts[3];
  if (pointDown) {
    pts[0].x = 0;     pts[0].y = 0;
    pts[1].x = w - 1; pts[1].y = 0;
    pts[2].x = w / 2; pts[2].y = h - 1;
  } else {
    pts[0].x = w / 2; pts[0].y = 0;
    pts[1].x = 0;     pts[1].y = h - 1;
    pts[2].x = w - 1; pts[2].y = h - 1;
  }
  lv_canvas_draw_polygon(cv, pts, 3, &d);
  return cv;
}

lv_obj_t *make_marker(lv_obj_t *parent) {
  lv_obj_t *m = make_triangle(parent, 16, 9, true, COL_ACCENT);
  lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 10);
  return m;
}

// MENU back affordance: a small down chevron + "MENU" label, tappable.
static void back_click_cb(lv_event_t *e) {
  (void)e;
  appHapticPress();
  navGo(SCR_LAUNCHER);
}

lv_obj_t *make_back_chevron(lv_obj_t *parent) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 110, 48);
  lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(box, 22);  // generous hit area around a small glyph
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *tri = make_triangle(box, 14, 7, true, COL_FAINT2);
  lv_obj_align(tri, LV_ALIGN_CENTER, 0, -9);

  lv_obj_t *lbl = make_label(box, "MENU", &font_jbm_10, COL_FAINT2);
  lv_obj_set_style_text_letter_space(lbl, 2, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 6);

  lv_obj_add_event_cb(box, back_click_cb, LV_EVENT_CLICKED, NULL);
  return box;
}
