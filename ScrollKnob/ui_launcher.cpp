// Launcher: radial arc menu. Rotate to move focus (the focused app animates to
// 12 o'clock); press or tap-focused to open.
#include "ui_internal.h"
#include <math.h>
#include <stdio.h>

#define DEG2RAD 0.017453292519943295
#define RING_R 128
#define ITEM_SZ 58
#define APP_N 5

struct AppDef { const char *name; const char *desc; ScreenId screen; };
static const AppDef APPS[APP_N] = {
  { "Scroll Wheel", "Laptop scroll over BLE HID", SCR_SCROLL },
  { "Countdown",    "Timer with haptic alarm",    SCR_TIMER  },
  { "Safe Cracker", "Crack the combo by ear",     SCR_GAME   },
  { "Dial of Doom", "One-dial FPS. Turn to aim",  SCR_FPS    },
  { "Settings",     "Display, haptics, network",  SCR_SETTINGS },
};

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_items[APP_N];
static lv_obj_t *s_centerIcon = nullptr;
static lv_obj_t *s_name = nullptr;
static lv_obj_t *s_desc = nullptr;
static lv_obj_t *s_meta = nullptr;
static int s_sel = 0;

// ---- Simple geometric app icons (recognizable, on-brand) ----
static lv_obj_t *iconContainer(lv_obj_t *parent, int sz) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, sz, sz);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
static lv_obj_t *strokeRect(lv_obj_t *p, int w, int h, int r, uint32_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
static void hline(lv_obj_t *p, int x, int y, int w, uint32_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, w, 2);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 1, 0);
  lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
}
static void vline(lv_obj_t *p, int x, int y, int h, uint32_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 2, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 1, 0);
  lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
}
static void dot(lv_obj_t *p, int x, int y, int d, uint32_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
}

// appIdx: 0 mouse, 1 stopwatch, 2 safe, 3 crosshair, 4 sliders. Index matches the
// APPS row. Drawn in `col`, fitted to `sz`.
static lv_obj_t *make_app_icon(lv_obj_t *parent, int appIdx, int sz, uint32_t col) {
  lv_obj_t *c = iconContainer(parent, sz);
  if (appIdx == 0) {  // mouse: body + wheel
    lv_obj_t *body = strokeRect(c, sz * 6 / 10, sz * 8 / 10, sz * 3 / 10, col);
    lv_obj_center(body);
    lv_obj_t *wheel = lv_obj_create(c);
    lv_obj_remove_style_all(wheel);
    lv_obj_set_size(wheel, 2, sz * 2 / 10);
    lv_obj_set_style_bg_color(wheel, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wheel, 1, 0);
    lv_obj_align(wheel, LV_ALIGN_TOP_MID, 0, sz * 15 / 100);
  } else if (appIdx == 1) {  // stopwatch: dial + stem + hand
    lv_obj_t *dial = strokeRect(c, sz * 7 / 10, sz * 7 / 10, LV_RADIUS_CIRCLE, col);
    lv_obj_align(dial, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *stem = lv_obj_create(c);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, sz * 2 / 10, 3);
    lv_obj_set_style_bg_color(stem, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *hand = lv_obj_create(c);
    lv_obj_remove_style_all(hand);
    lv_obj_set_size(hand, 2, sz * 22 / 100);
    lv_obj_set_style_bg_color(hand, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(hand, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hand, 1, 0);
    lv_obj_align(hand, LV_ALIGN_CENTER, 0, sz * 6 / 100);
  } else if (appIdx == 2) {  // safe: body + combo dial + handle
    lv_obj_t *body = strokeRect(c, sz * 8 / 10, sz * 8 / 10, sz * 15 / 100, col);
    lv_obj_center(body);
    lv_obj_t *dial = strokeRect(c, sz * 3 / 10, sz * 3 / 10, LV_RADIUS_CIRCLE, col);
    lv_obj_align(dial, LV_ALIGN_CENTER, -sz * 6 / 100, 0);
    lv_obj_t *handle = lv_obj_create(c);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, sz * 22 / 100, 2);
    lv_obj_set_style_bg_color(handle, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 1, 0);
    lv_obj_align(handle, LV_ALIGN_CENTER, sz * 22 / 100, 0);
  } else if (appIdx == 3) {  // crosshair: reticle ring + four ticks + pip
    lv_obj_t *ring = strokeRect(c, sz * 7 / 10, sz * 7 / 10, LV_RADIUS_CIRCLE, col);
    lv_obj_center(ring);
    int t = sz * 18 / 100;                 // tick length
    int m = sz / 2 - 1;                    // centre line offset (2px strokes)
    vline(c, m, 0, t, col);
    vline(c, m, sz - t, t, col);
    hline(c, 0, m, t, col);
    hline(c, sz - t, m, t, col);
    dot(c, m - 1, m - 1, 4, col);
  } else {  // sliders: three lines with knobs
    int y0 = sz * 22 / 100, gap = sz * 28 / 100, w = sz * 8 / 10, x0 = sz / 10;
    hline(c, x0, y0, w, col);            dot(c, x0 + w * 6 / 10, y0 - 2, 6, col);
    hline(c, x0, y0 + gap, w, col);      dot(c, x0 + w * 2 / 10, y0 + gap - 2, 6, col);
    hline(c, x0, y0 + 2 * gap, w, col);  dot(c, x0 + w * 7 / 10, y0 + 2 * gap - 2, 6, col);
  }
  return c;
}

// ---- Ring geometry ----
static void itemTarget(int i, int sel, int *x, int *y) {
  int k = ((i - sel) % APP_N + APP_N) % APP_N;   // 0 = focused (top)
  double a = (-90.0 + k * (360.0 / APP_N)) * DEG2RAD;
  *x = (int)(SCR_CX + RING_R * cos(a)) - ITEM_SZ / 2;
  *y = (int)(SCR_CY + RING_R * sin(a)) - ITEM_SZ / 2;
}

static void styleItem(lv_obj_t *o, bool focused) {
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(COL_ACCENT), 0);
  if (focused) {
    lv_obj_set_style_bg_opa(o, 33, 0);                 // ~13%
    lv_obj_set_style_border_color(o, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_shadow_width(o, 22, 0);
    lv_obj_set_style_shadow_spread(o, -6, 0);
    lv_obj_set_style_shadow_opa(o, 140, 0);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_zoom(o, 287, 0);        // 1.12x
  } else {
    lv_obj_set_style_bg_opa(o, 8, 0);                  // ~3%
    lv_obj_set_style_border_color(o, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(o, 20, 0);             // ~8%
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_opa(o, 130, 0);                   // ~50%
    lv_obj_set_style_transform_zoom(o, 220, 0);        // 0.86x
  }
}

// ---- Animations ----
static void anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void animItemTo(lv_obj_t *o, int tx, int ty) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_time(&a, 340);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_values(&a, lv_obj_get_x(o), tx);
  lv_anim_set_exec_cb(&a, anim_x_cb);
  lv_anim_start(&a);
  lv_anim_set_values(&a, lv_obj_get_y(o), ty);
  lv_anim_set_exec_cb(&a, anim_y_cb);
  lv_anim_start(&a);
}

static void refreshCenter(void) {
  if (s_centerIcon) lv_obj_del(s_centerIcon);
  s_centerIcon = make_app_icon(s_scr, s_sel, 40, COL_ACCENT);
  lv_obj_align(s_centerIcon, LV_ALIGN_CENTER, 0, -44);

  lv_label_set_text(s_name, APPS[s_sel].name);
  lv_label_set_text(s_desc, APPS[s_sel].desc);
  char meta[40];
  snprintf(meta, sizeof(meta), "%d / %d  ·  TAP TO OPEN", s_sel + 1, APP_N);
  lv_label_set_text(s_meta, meta);
}

static void applyFocus(bool animate) {
  for (int i = 0; i < APP_N; i++) {
    int tx, ty;
    itemTarget(i, s_sel, &tx, &ty);
    if (animate) animItemTo(s_items[i], tx, ty);
    else lv_obj_set_pos(s_items[i], tx, ty);
    styleItem(s_items[i], i == s_sel);
  }
  refreshCenter();
}

// ---- Touch: tap a ring item ----
static void item_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  appHapticPress();
  if (idx == s_sel) navGo(APPS[s_sel].screen);   // tap focused = open
  else { s_sel = idx; applyFocus(true); }
}
static void center_click_cb(lv_event_t *e) {
  (void)e;
  appHapticPress();
  navGo(APPS[s_sel].screen);
}

static void build(void) {
  s_scr = make_screen_base();
  make_marker(s_scr);

  for (int i = 0; i < APP_N; i++) {
    lv_obj_t *it = lv_obj_create(s_scr);
    lv_obj_remove_style_all(it);
    lv_obj_set_size(it, ITEM_SZ, ITEM_SZ);
    lv_obj_clear_flag(it, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(it, 28);  // enlarge the hit area beyond the 58px circle
    lv_obj_t *icon = make_app_icon(it, i, 26, COL_ACCENT);
    lv_obj_center(icon);
    lv_obj_add_event_cb(it, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s_items[i] = it;
  }

  // Center readout (does not rotate)
  s_name = make_label(s_scr, APPS[0].name, &font_sg_23, COL_TX);
  lv_obj_align(s_name, LV_ALIGN_CENTER, 0, 4);
  s_desc = make_label(s_scr, APPS[0].desc, &font_sg_12, COL_DIM);
  lv_label_set_long_mode(s_desc, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_desc, 180);
  lv_obj_set_style_text_align(s_desc, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_desc, LV_ALIGN_CENTER, 0, 30);
  s_meta = make_label(s_scr, "1 / 3  ·  TAP TO OPEN", &font_jbm_10, COL_FAINT);
  lv_obj_set_style_text_letter_space(s_meta, 2, 0);
  lv_obj_align(s_meta, LV_ALIGN_CENTER, 0, 56);

  // Big transparent centre "open" target: a tap anywhere in the middle opens the
  // focused app (far more forgiving than hitting the small icon/label). Sized to
  // clear the ring items at radius 128, so it never steals their taps.
  lv_obj_t *hit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(hit);
  lv_obj_set_size(hit, 176, 176);
  lv_obj_center(hit);
  lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hit, center_click_cb, LV_EVENT_CLICKED, NULL);

  applyFocus(false);
}

void launcher_show(void) {
  if (!s_scr) build();
  else applyFocus(false);
  lv_scr_load(s_scr);
}

void launcher_encoder(int dir) {
  s_sel = ((s_sel + dir) % APP_N + APP_N) % APP_N;
  applyFocus(true);
}

void launcher_press(void) {
  navGo(APPS[s_sel].screen);
}
