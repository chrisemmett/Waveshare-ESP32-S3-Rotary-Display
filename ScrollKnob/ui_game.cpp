// Safe Cracker game. Rotate the dial to hunt for each tumbler; the waveform +
// status strengthen near the target, and the detents firm up as you close in.
// Arrive from the required direction and hold still for 3 s to capture it.
// Turning the wrong way trips the lock: a red LOCKOUT flash resets the whole game
// (the combination stays the same). A countdown ring drains around the rim and
// the final seconds tick audibly -- run out and it's OUT OF TIME. Five tumblers
// -> SAFE OPEN. Touch: NEW starts a fresh game, MENU returns to the launcher.
#include "ui_internal.h"
#include <Arduino.h>
#include <stdio.h>

#define GAME_DEBUG 0        // 1 = print the secret combo to Serial
#define GAME_INVERT_DIR 0   // flip if physical clockwise feels reversed
#define GAME_TIMER 1        // 1 = "beat the clock" countdown (Hollywood pressure)
#define GAME_TIMER_MS 45000 // seconds on the clock before the guard is back
#define N_TUMBLERS 5
#define DIAL_N 50
#define HOLD_MS 3000
#define LOCKOUT_MS 900      // red "LOCKOUT" flash after a wrong-way turn
#define NBARS 9

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_num = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_dots[N_TUMBLERS];
static lv_obj_t *s_bars[NBARS];
static lv_obj_t *s_holdArc = nullptr;
static lv_obj_t *s_ring = nullptr;      // direction indicator ring
static lv_obj_t *s_headCW = nullptr;
static lv_obj_t *s_headACW = nullptr;
static lv_obj_t *s_openHit = nullptr;   // full-screen "tap to play again" (when open/busted)
static lv_obj_t *s_timeArc = nullptr;   // countdown ring around the rim

static int s_secret[N_TUMBLERS];
static int s_tumbler = 0;
static int s_pos = 0;
static int s_lastDir = 0;               // +1 = CW, -1 = ACW, 0 = none yet
static bool s_holdActive = false;
static uint32_t s_holdStartMs = 0;
static bool s_open = false;
static bool s_busted = false;           // ran out of time -> game over
static uint32_t s_flashUntil = 0;
static uint32_t s_lockoutUntil = 0;     // red lockout flash active until this ms
static uint32_t s_deadlineMs = 0;       // wall-clock time the countdown expires
static int s_lastTickSec = -1;          // last whole second we played a clock tick on
static bool s_built = false;

static bool lockoutActive(void) { return s_lockoutUntil != 0; }

static const int BAR_ENV[NBARS] = { 30, 50, 72, 90, 100, 90, 72, 50, 30 };

// ---- helpers ----
static int circDist(int a, int b) {
  int d = a - b;
  if (d < 0) d = -d;
  return d < DIAL_N - d ? d : DIAL_N - d;
}
static bool requiredCW(void) { return (s_tumbler % 2) == 0; }
static bool dirCorrect(int mv) { return mv != 0 && ((mv > 0) == requiredCW()); }

// Horizontal arrowhead on a small alpha canvas (points left or right).
static lv_obj_t *make_hArrow(lv_obj_t *parent, int w, int h, bool right, uint32_t color) {
  uint32_t bufsz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(w, h);
  void *buf = lv_mem_alloc(bufsz);
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, buf, w, h, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(color);
  d.bg_opa = LV_OPA_COVER;
  lv_point_t p[3];
  if (right) { p[0] = {0, 0}; p[1] = {0, (lv_coord_t)(h - 1)}; p[2] = {(lv_coord_t)(w - 1), (lv_coord_t)(h / 2)}; }
  else       { p[0] = {(lv_coord_t)(w - 1), 0}; p[1] = {(lv_coord_t)(w - 1), (lv_coord_t)(h - 1)}; p[2] = {0, (lv_coord_t)(h / 2)}; }
  lv_canvas_draw_polygon(cv, p, 3, &d);
  return cv;
}

static void updateDots(void) {
  for (int i = 0; i < N_TUMBLERS; i++) {
    bool done = i < s_tumbler;
    lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(COL_TX), 0);
    lv_obj_set_style_bg_opa(s_dots[i], done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots[i], done ? 0 : 1, 0);
    lv_obj_set_style_border_color(s_dots[i], lv_color_hex(COL_FAINT), 0);
  }
}

static void updateDir(void) {
  bool cw = requiredCW();
  lv_obj_add_flag(s_headCW, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_headACW, LV_OBJ_FLAG_HIDDEN);
  if (!s_open) lv_obj_clear_flag(cw ? s_headCW : s_headACW, LV_OBJ_FLAG_HIDDEN);
  if (s_open) lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
}

// distance -> waveform fill fraction (x100) + colour
static int levelFrac(int dist, uint32_t *col) {
  if (dist == 0) { *col = COL_GREEN;  return 100; }
  if (dist <= 2) { *col = COL_ACCENT; return 90; }
  if (dist <= 5) { *col = COL_ACCENT; return 72; }
  if (dist <= 9) { *col = COL_ACCENT; return 50; }
  if (dist <= 14) { *col = COL_FAINT; return 30; }
  *col = COL_FAINT; return 14;
}

static void redraw(void) {
  bool alarm = lockoutActive() || s_busted;  // red states

  // Current number
  char b[4];
  snprintf(b, sizeof(b), "%d", s_pos);
  lv_label_set_text(s_num, b);
  uint32_t numCol = alarm ? COL_RED : (s_flashUntil ? COL_GREEN : COL_TX);
  lv_obj_set_style_text_color(s_num, lv_color_hex(numCol), 0);

  updateDots();
  updateDir();

  int target = s_secret[s_tumbler];
  int dist = s_open ? 99 : circDist(s_pos, target);

  // Waveform
  uint32_t wcol;
  int frac = s_open ? 0 : levelFrac(dist, &wcol);
  if (s_open) wcol = COL_GREEN;
  if (alarm) wcol = COL_RED;
  for (int i = 0; i < NBARS; i++) {
    int h = 3 + (BAR_ENV[i] * 44 / 100) * frac / 100;
    lv_obj_set_height(s_bars[i], h);
    lv_obj_align(s_bars[i], LV_ALIGN_BOTTOM_LEFT, i * 17, 0);  // keep bottom-anchored
    lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(wcol), 0);
  }

  // Status text + colour
  const char *st;
  uint32_t sc;
  if (lockoutActive()) {
    st = "LOCKOUT"; sc = COL_RED;
  } else if (s_busted) {
    st = "OUT OF TIME"; sc = COL_RED;
  } else if (s_open) {
    st = "SAFE OPEN"; sc = COL_GREEN;
  } else if (dist == 0) {
    if (dirCorrect(s_lastDir)) { st = "HOLD STEADY"; sc = COL_GREEN; }
    else { st = requiredCW() ? "APPROACH CLOCKWISE" : "APPROACH ANTICLOCKWISE"; sc = COL_ACCENT; }
  } else if (s_lastDir == 0) {
    st = requiredCW() ? "TURN CLOCKWISE" : "TURN ANTICLOCKWISE"; sc = COL_DIM;
  } else if (dist <= 2) { st = "VERY STRONG";    sc = COL_ACCENT; }
  else if (dist <= 5)   { st = "GETTING WARMER"; sc = COL_ACCENT; }
  else if (dist <= 9)   { st = "FAINT MOVEMENT"; sc = COL_ACCENT; }
  else if (dist <= 14)  { st = "FAINT";          sc = COL_DIM; }
  else                  { st = "QUIET";          sc = COL_DIM; }
  lv_label_set_text(s_status, st);
  lv_obj_set_style_text_color(s_status, lv_color_hex(sc), 0);
}

static void cancelHold(void) {
  s_holdActive = false;
  lv_obj_add_flag(s_holdArc, LV_OBJ_FLAG_HIDDEN);
}
static void startHold(void) {
  s_holdActive = true;
  s_holdStartMs = millis();
  lv_arc_set_value(s_holdArc, 0);
  lv_obj_clear_flag(s_holdArc, LV_OBJ_FLAG_HIDDEN);
}

// Freeze the clock when the safe is open or the attempt is busted.
static void stopTimer(void) {
  if (s_timeArc) lv_obj_add_flag(s_timeArc, LV_OBJ_FLAG_HIDDEN);
}

static void openSafe(void) {
  s_open = true;
  cancelHold();
  appHapticAlarm();
  s_flashUntil = 0;
  stopTimer();
  lv_obj_clear_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_openHit);
  redraw();
}

#if GAME_TIMER
// The clock ran out before all five tumblers fell: game over.
static void bustGame(void) {
  s_busted = true;
  cancelHold();
  appHapticAlarm();
  s_flashUntil = 0;
  stopTimer();
  lv_obj_clear_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_openHit);
  redraw();
}
#endif

static void capture(void) {
  s_tumbler++;
  cancelHold();
  appHapticPress();
  s_flashUntil = millis() + 450;  // brief green flash on the number
  if (s_tumbler >= N_TUMBLERS) {
    openSafe();
  } else {
    s_lastDir = 0;  // must move again from the new required direction
    redraw();
  }
}

// Reset progress back to the first tumbler, keeping the current combination.
static void resetProgress(void) {
  s_tumbler = 0;
  s_pos = 0;
  s_lastDir = 0;
  s_open = false;
  s_flashUntil = 0;
  cancelHold();
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
}

// Turning the wrong way trips the lock: alarm buzz, a red LOCKOUT flash, and the
// dial resets to the first tumbler. The combination and the clock are unchanged.
static void wrongDirReset(void) {
  appHapticAlarm();
  resetProgress();
  s_lockoutUntil = millis() + LOCKOUT_MS;
  redraw();
}

static void newGame(void) {
  for (int i = 0; i < N_TUMBLERS; i++) s_secret[i] = appRandom() % DIAL_N;
  resetProgress();
  s_busted = false;
  s_lockoutUntil = 0;
  s_lastTickSec = -1;
#if GAME_TIMER
  s_deadlineMs = millis() + GAME_TIMER_MS;
  if (s_timeArc) {
    lv_arc_set_value(s_timeArc, 1000);
    lv_obj_set_style_arc_color(s_timeArc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_timeArc, LV_OBJ_FLAG_HIDDEN);
  }
#endif
#if GAME_DEBUG
  Serial.printf("Safe combo: %d %d %d %d %d\n", s_secret[0], s_secret[1],
                s_secret[2], s_secret[3], s_secret[4]);
#endif
  redraw();
}

// ---- touch callbacks ----
static void new_click_cb(lv_event_t *e) { (void)e; appHapticPress(); newGame(); }
static void open_click_cb(lv_event_t *e) { (void)e; appHapticPress(); newGame(); }

static void build(void) {
  s_scr = make_screen_base();
  make_marker(s_scr);

  // Direction indicator: ring + horizontal arrowhead (CW/ACW), near the top.
  s_ring = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_ring);
  lv_obj_set_size(s_ring, 40, 40);
  lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(s_ring, 3, 0);
  lv_obj_set_style_border_color(s_ring, lv_color_hex(COL_TX), 0);
  lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
  s_headCW = make_hArrow(s_scr, 12, 12, true, COL_TX);
  lv_obj_align(s_headCW, LV_ALIGN_TOP_MID, 12, 30);   // top-right of the ring
  s_headACW = make_hArrow(s_scr, 12, 12, false, COL_TX);
  lv_obj_align(s_headACW, LV_ALIGN_TOP_MID, -12, 30); // top-left of the ring

#if GAME_TIMER
  // "Beat the clock" countdown ring, hugging the rim; drains clockwise from 12.
  s_timeArc = lv_arc_create(s_scr);
  lv_obj_set_size(s_timeArc, SCR_W - 8, SCR_W - 8);
  lv_obj_center(s_timeArc);
  lv_arc_set_rotation(s_timeArc, 270);
  lv_arc_set_bg_angles(s_timeArc, 0, 360);
  lv_arc_set_range(s_timeArc, 0, 1000);
  lv_arc_set_value(s_timeArc, 1000);
  lv_obj_set_style_bg_opa(s_timeArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_timeArc, 0, LV_PART_KNOB);
  lv_obj_clear_flag(s_timeArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_timeArc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_timeArc, LV_OPA_TRANSP, LV_PART_MAIN);  // no track
  lv_obj_set_style_arc_width(s_timeArc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_timeArc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
#endif

  // Hold countdown ring around the number
  s_holdArc = lv_arc_create(s_scr);
  lv_obj_set_size(s_holdArc, 132, 132);
  lv_obj_align(s_holdArc, LV_ALIGN_CENTER, 0, -24);
  lv_arc_set_rotation(s_holdArc, 270);
  lv_arc_set_bg_angles(s_holdArc, 0, 360);
  lv_arc_set_range(s_holdArc, 0, 1000);
  lv_arc_set_value(s_holdArc, 0);
  lv_obj_set_style_bg_opa(s_holdArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_holdArc, 0, LV_PART_KNOB);
  lv_obj_clear_flag(s_holdArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_holdArc, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_holdArc, LV_OPA_TRANSP, LV_PART_MAIN);  // no track
  lv_obj_set_style_arc_width(s_holdArc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_holdArc, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
  lv_obj_add_flag(s_holdArc, LV_OBJ_FLAG_HIDDEN);

  // Current number
  s_num = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_num, &font_jbm_52, 0);
  lv_obj_set_style_text_color(s_num, lv_color_hex(COL_TX), 0);
  lv_label_set_text(s_num, "0");
  lv_obj_align(s_num, LV_ALIGN_CENTER, 0, -24);

  // Progress dots
  for (int i = 0; i < N_TUMBLERS; i++) {
    lv_obj_t *d = lv_obj_create(s_scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 11, 11);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(d, LV_ALIGN_CENTER, (i - 2) * 22, 22);
    s_dots[i] = d;
  }

  // Listening waveform (row of vertical bars, bottom-anchored)
  lv_obj_t *wave = lv_obj_create(s_scr);
  lv_obj_remove_style_all(wave);
  lv_obj_set_size(wave, NBARS * 17, 48);
  lv_obj_align(wave, LV_ALIGN_CENTER, 0, 74);
  lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < NBARS; i++) {
    lv_obj_t *bar = lv_obj_create(wave);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 8, 4);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_FAINT), 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 17, 0);
    s_bars[i] = bar;
  }

  // Status text
  s_status = make_label(s_scr, "TURN CLOCKWISE", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(s_status, 2, 0);
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 112);

  // NEW (top-left) + MENU back chevron (bottom-centre)
  lv_obj_t *nb = make_label(s_scr, "NEW", &font_jbm_10, COL_ACCENT);
  lv_obj_set_style_text_letter_space(nb, 2, 0);
  lv_obj_align(nb, LV_ALIGN_TOP_LEFT, 40, 74);
  lv_obj_add_flag(nb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(nb, 24);
  lv_obj_add_event_cb(nb, new_click_cb, LV_EVENT_CLICKED, NULL);

  make_back_chevron(s_scr);

  // "Tap to play again" target, only active on SAFE OPEN. A centre circle (not
  // full-screen) so the MENU chevron and NEW label stay tappable.
  s_openHit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_openHit);
  lv_obj_set_size(s_openHit, 240, 240);
  lv_obj_center(s_openHit);
  lv_obj_set_style_radius(s_openHit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_openHit, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(s_openHit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_openHit, open_click_cb, LV_EVENT_CLICKED, NULL);

  s_built = true;
}

void game_show(void) {
  if (!s_built) {
    build();
    newGame();
  }
  lv_scr_load(s_scr);
}

void game_encoder(int rawDir) {
  if (!s_built || s_open || s_busted || lockoutActive()) return;
  int mv = GAME_INVERT_DIR ? -rawDir : rawDir;
  if (mv != 0 && !dirCorrect(mv)) { wrongDirReset(); return; }  // wrong way -> reset
  s_pos = (s_pos + mv + DIAL_N) % DIAL_N;
  s_lastDir = mv;
  cancelHold();  // any movement cancels a hold
  // Feel the tumbler catch: the global per-detent tick firms into a press as the
  // correct-direction approach closes on the target (last hapticsPlay wins).
  int dist = circDist(s_pos, s_secret[s_tumbler]);
  if (dist <= 2) appHapticPress();
  if (dist == 0) startHold();  // on target, from the correct direction -> hold
  redraw();
}

void game_tick(void) {
  if (!s_built) return;
  uint32_t now = millis();

  // Lockout flash expiry -> restore normal colours
  if (lockoutActive() && now >= s_lockoutUntil) {
    s_lockoutUntil = 0;
    redraw();
  }

  if (s_flashUntil && now >= s_flashUntil) {
    s_flashUntil = 0;
    if (!s_open && !s_busted && !lockoutActive())
      lv_obj_set_style_text_color(s_num, lv_color_hex(COL_TX), 0);
  }

#if GAME_TIMER
  // Beat the clock: drain the rim arc, tick the final seconds, bust at zero.
  if (!s_open && !s_busted) {
    uint32_t rem = (now >= s_deadlineMs) ? 0 : (s_deadlineMs - now);
    if (s_timeArc) {
      lv_arc_set_value(s_timeArc, (int)(rem * 1000 / GAME_TIMER_MS));
      if (rem <= 10000)
        lv_obj_set_style_arc_color(s_timeArc, lv_color_hex(COL_RED), LV_PART_INDICATOR);
    }
    int sec = (int)((rem + 999) / 1000);  // whole seconds left (ceil)
    if (rem > 0 && rem <= 5000 && sec != s_lastTickSec) {
      s_lastTickSec = sec;
      appHapticTick();  // the clock, ticking down
    }
    if (rem == 0) { bustGame(); return; }
  }
#endif

  if (s_holdActive) {
    uint32_t el = now - s_holdStartMs;
    if (el >= HOLD_MS) { capture(); return; }
    lv_arc_set_value(s_holdArc, (int)(el * 1000 / HOLD_MS));
  }
}
