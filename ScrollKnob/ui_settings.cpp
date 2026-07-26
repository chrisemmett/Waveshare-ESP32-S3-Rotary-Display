// Settings: Brightness / Haptics / Bluetooth / Always-On. Rotate = move focus
// (or adjust brightness in edit mode); press = activate the focused row.
// Haptics and Always-On are plain ON/OFF toggles backed by the .ino state.
#include "ui_internal.h"
#include <stdio.h>

#define ROW_N 4
enum { ROW_BRIGHT, ROW_HAPTIC, ROW_BT, ROW_ALWAYS };

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_rows[ROW_N];
static lv_obj_t *s_vals[ROW_N];
static lv_obj_t *s_bar = nullptr;
static lv_timer_t *s_btTimer = nullptr;

static int s_focus = 0;
static bool s_edit = false;  // brightness edit mode

static void setBtVal(const char *txt, uint32_t col) {
  lv_label_set_text(s_vals[ROW_BT], txt);
  lv_obj_set_style_text_color(s_vals[ROW_BT], lv_color_hex(col), 0);
}

static void refreshValues(void) {
  char b[8];
  snprintf(b, sizeof(b), "%d%%", appGetBrightness());
  lv_label_set_text(s_vals[ROW_BRIGHT], b);
  lv_bar_set_value(s_bar, appGetBrightness(), LV_ANIM_OFF);

  bool h = appHapticsEnabled();
  lv_label_set_text(s_vals[ROW_HAPTIC], h ? "ON" : "OFF");
  lv_obj_set_style_text_color(s_vals[ROW_HAPTIC], lv_color_hex(h ? COL_GREEN : COL_FAINT2), 0);

  bool a = appAlwaysOn();
  lv_label_set_text(s_vals[ROW_ALWAYS], a ? "ON" : "OFF");
  lv_obj_set_style_text_color(s_vals[ROW_ALWAYS], lv_color_hex(a ? COL_GREEN : COL_FAINT2), 0);
}

static void applyFocus(void) {
  for (int i = 0; i < ROW_N; i++) {
    bool f = (i == s_focus);
    lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_rows[i], f ? 33 : 0, 0);
    bool editing = (f && i == ROW_BRIGHT && s_edit);
    lv_obj_set_style_border_width(s_rows[i], f ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_rows[i], lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(s_rows[i], editing ? LV_OPA_COVER : (f ? 100 : 0), 0);
  }
}

static void makeRow(int i, const char *label, const char *val, uint32_t valCol) {
  lv_obj_t *r = lv_obj_create(s_scr);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, 216, i == ROW_BRIGHT ? 52 : 44);
  lv_obj_set_style_radius(r, 11, 0);
  lv_obj_set_style_pad_hor(r, 13, 0);
  lv_obj_set_style_pad_ver(r, 9, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(r, 6);  // small comfort margin (rows are already 8px apart)

  lv_obj_t *l = make_label(r, label, &font_sg_14, COL_TX2);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *v = make_label(r, val, &font_jbm_12, valCol);
  lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 1);
  s_vals[i] = v;
  s_rows[i] = r;
}

static void row_click_cb(lv_event_t *e) {
  appHapticPress();
  s_focus = (int)(intptr_t)lv_event_get_user_data(e);
  applyFocus();
  settings_press();
}

static void btRevert_cb(lv_timer_t *t) {
  setBtVal("DISCONNECT", COL_RED);
  lv_timer_del(t);
  s_btTimer = nullptr;
}

static void build(void) {
  s_scr = make_screen_base();
  make_marker(s_scr);
  make_back_chevron(s_scr);

  lv_obj_t *hdr = make_label(s_scr, "SETTINGS", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(hdr, 3, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 60);

  lv_obj_t *list = lv_obj_create(s_scr);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 220, 210);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

  // makeRow() builds into whatever s_scr points at, so temporarily aim it at
  // `list` to nest the rows, then restore the real screen pointer.
  lv_obj_t *saved = s_scr;
  s_scr = list;
  makeRow(ROW_BRIGHT, "Brightness", "80%", COL_TX2);
  // brightness progress bar
  s_bar = lv_bar_create(s_rows[ROW_BRIGHT]);
  lv_obj_set_size(s_bar, 190, 4);
  lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_bar_set_range(s_bar, 0, 100);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, 26, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);

  makeRow(ROW_HAPTIC, "Haptics", "ON", COL_GREEN);
  makeRow(ROW_BT, "Bluetooth", "DISCONNECT", COL_RED);
  makeRow(ROW_ALWAYS, "Always-On", "OFF", COL_FAINT2);
  s_scr = saved;

  for (int i = 0; i < ROW_N; i++)
    lv_obj_add_event_cb(s_rows[i], row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

  refreshValues();
  applyFocus();
}

void settings_show(void) {
  if (!s_scr) build();
  else { refreshValues(); applyFocus(); }
  lv_scr_load(s_scr);
}

void settings_encoder(int dir) {
  if (s_edit) {  // adjust brightness
    int p = (int)appGetBrightness() + dir * 5;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    appSetBrightness((uint8_t)p);
    refreshValues();
  } else {
    s_focus = ((s_focus + dir) % ROW_N + ROW_N) % ROW_N;
    applyFocus();
  }
}

void settings_press(void) {
  switch (s_focus) {
    case ROW_BRIGHT:
      s_edit = !s_edit;
      applyFocus();
      break;
    case ROW_HAPTIC:
      appSetHaptics(!appHapticsEnabled());
      refreshValues();
      break;
    case ROW_BT:
      appForgetBonds();
      setBtVal("DISCONNECTED", COL_DIM);
      if (s_btTimer) lv_timer_del(s_btTimer);
      s_btTimer = lv_timer_create(btRevert_cb, 1600, NULL);
      break;
    case ROW_ALWAYS:
      // ON = the screen never blanks. OFF = it blanks after the idle timeout.
      appSetAlwaysOn(!appAlwaysOn());
      refreshValues();
      break;
  }
}

void settings_conn_changed(void) {
  // BT row is an action, not a live status; nothing to repaint here.
}
