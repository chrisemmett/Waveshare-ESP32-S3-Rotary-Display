// Bluetooth: link control + the paired-device list. Reached from Settings.
// Rotate moves focus and tapping a row activates it - this knob has no shaft
// press, so tap is the only activation (same as Settings).
//
// Rows are a fixed pool of slots that are shown/hidden per refresh rather than
// created and destroyed - the bond list changes shape (0..APP_BT_MAX_BONDS
// rows), and rebuilding the flex list on every repaint would churn the LVGL
// heap for no reason.
//
// The two destructive actions (forget one / forget all) are two-tap: the first
// tap arms the row, a second within CONFIRM_MS commits, and rotating away or
// waiting it out cancels. Unpairing a laptop shouldn't be one stray tap away,
// and there is no keyboard here to put a real dialog on.
#include "ui_internal.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#define LIST_W 224
#define LIST_H 190
#define ROW_W 216
#define ROW_H 48   // two lines: a 14px title over a 10px status line
#define CONFIRM_MS 2600
#define TICK_MS 400

// Slot layout, top to bottom.
enum { SLOT_DISCOVER, SLOT_LINK, SLOT_BOND0 };
#define SLOT_FORGET_ALL (SLOT_BOND0 + APP_BT_MAX_BONDS)
#define SLOT_N (SLOT_FORGET_ALL + 1)

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_row[SLOT_N];
static lv_obj_t *s_title[SLOT_N];
static lv_obj_t *s_val[SLOT_N];
static lv_obj_t *s_sub[SLOT_N];

static bool s_active[SLOT_N];      // focusable / pressable this refresh
static int  s_focus = SLOT_DISCOVER;
static int  s_confirm = -1;        // slot armed for a destructive press
static uint32_t s_confirmMs = 0;
static uint32_t s_lastTickMs = 0;
static uint32_t s_hash = 0xffffffff;  // last painted state; forces a first paint

// ---- Small guards so an unchanged refresh doesn't invalidate the screen ----
static void setText(lv_obj_t *l, const char *txt) {
  if (strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

static void rowShow(int slot, bool show) {
  if (show) lv_obj_clear_flag(s_row[slot], LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_row[slot], LV_OBJ_FLAG_HIDDEN);
}

// Fill one row. `active` rows take focus and respond to a press; inactive ones
// are pure status (the empty-list placeholder, the link row with no host).
static void rowSet(int slot, bool active,
                   const char *title, const lv_font_t *font, uint32_t titleCol,
                   const char *val, uint32_t valCol,
                   const char *sub, uint32_t subCol) {
  s_active[slot] = active;
  rowShow(slot, true);
  setText(s_title[slot], title);
  lv_obj_set_style_text_font(s_title[slot], font, 0);
  lv_obj_set_style_text_color(s_title[slot], lv_color_hex(titleCol), 0);
  setText(s_val[slot], val);
  lv_obj_set_style_text_color(s_val[slot], lv_color_hex(valCol), 0);
  setText(s_sub[slot], sub);
  lv_obj_set_style_text_color(s_sub[slot], lv_color_hex(subCol), 0);
  if (active) lv_obj_add_flag(s_row[slot], LV_OBJ_FLAG_CLICKABLE);
  else lv_obj_clear_flag(s_row[slot], LV_OBJ_FLAG_CLICKABLE);
}

// ---- Focus ----
static bool slotVisible(int slot) {
  return !lv_obj_has_flag(s_row[slot], LV_OBJ_FLAG_HIDDEN);
}
static bool slotFocusable(int slot) { return slotVisible(slot) && s_active[slot]; }

// Move focus `dir` steps through the focusable slots, wrapping. Returns false
// if nothing at all is focusable (no bonds, no host, and that is fine).
static bool focusStep(int dir) {
  for (int i = 1; i <= SLOT_N; i++) {
    int cand = ((s_focus + dir * i) % SLOT_N + SLOT_N) % SLOT_N;
    if (slotFocusable(cand)) { s_focus = cand; return true; }
  }
  return false;
}

static void applyFocus(void) {
  if (!slotFocusable(s_focus)) focusStep(1);  // the focused row just went away
  for (int i = 0; i < SLOT_N; i++) {
    bool f = slotVisible(i) && (i == s_focus) && s_active[i];
    lv_obj_set_style_bg_color(s_row[i], lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_row[i], f ? 33 : 0, 0);
    lv_obj_set_style_border_width(s_row[i], f ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_row[i],
        lv_color_hex(i == s_confirm ? COL_RED : COL_ACCENT), 0);
    lv_obj_set_style_border_opa(s_row[i], f ? 120 : 0, 0);
  }
  if (slotFocusable(s_focus)) lv_obj_scroll_to_view(s_row[s_focus], LV_ANIM_ON);
}

// ---- Repaint ----
// Everything the screen draws, folded into one FNV-1a value so the periodic
// tick can skip the repaint while it holds still. LVGL invalidates on every
// set_text / set_style call whether or not the value moved, so an unguarded
// refresh() would redraw the whole list several times a second for nothing.
// Addresses go in as well as the bond count: a re-pair can swap a bond out
// without the count ever changing.
static void hashStr(uint32_t *h, const char *s) {
  for (; *s; s++) *h = (*h ^ (uint8_t)*s) * 16777619u;
}
static void hashU8(uint32_t *h, uint8_t v) { *h = (*h ^ v) * 16777619u; }

static uint32_t stateHash(void) {
  uint32_t h = 2166136261u;
  hashU8(&h, appConnected() ? 1 : 0);
  hashU8(&h, appBtDiscoverable() ? 1 : 0);
  hashU8(&h, appBtAdvertising() ? 1 : 0);
  hashU8(&h, (uint8_t)s_focus);
  hashU8(&h, (uint8_t)(s_confirm + 1));

  char addr[APP_BT_ADDR_LEN];
  appBtPeerAddr(addr, sizeof(addr));
  hashStr(&h, addr);

  const uint8_t n = appBtBondCount();
  hashU8(&h, n);
  for (uint8_t i = 0; i < n; i++) {
    appBtBondAddr(i, addr, sizeof(addr));
    hashStr(&h, addr);
  }
  return h;
}

static void refresh(void) {
  uint32_t h = stateHash();
  if (h == s_hash) return;
  s_hash = h;

  char addr[APP_BT_ADDR_LEN];
  char buf[40];
  const uint8_t n = appBtBondCount();
  const bool conn = appConnected();
  const bool disc = appBtDiscoverable();
  const bool adv = appBtAdvertising();

  // Discoverable: the intent. Advertising is the live consequence, and it goes
  // quiet on its own while a host holds the link - say so, or "ON" with nothing
  // broadcasting looks broken.
  rowSet(SLOT_DISCOVER, true,
         "Discoverable", &font_sg_14, COL_TX2,
         disc ? "ON" : "OFF", disc ? COL_GREEN : COL_FAINT2,
         adv ? "broadcasting now"
             : (disc ? "paused while connected" : "hidden - no host can link"),
         COL_FAINT2);

  // Connection: status when idle, the disconnect button when a host is on.
  appBtPeerAddr(addr, sizeof(addr));
  rowSet(SLOT_LINK, conn,
         "Connection", &font_sg_14, conn ? COL_TX2 : COL_DIM,
         conn ? "DISCONNECT" : "NO HOST", conn ? COL_RED : COL_FAINT2,
         conn ? addr : (adv ? "waiting for a host" : "not advertising"),
         conn ? COL_DIM : COL_FAINT2);

  // Paired devices. Bond indices shift when one is deleted, so they are read
  // fresh every repaint and never cached across a forget.
  for (int i = 0; i < APP_BT_MAX_BONDS; i++) {
    const int slot = SLOT_BOND0 + i;
    if (i >= n) {
      if (i == 0) {  // placeholder keeps the list from looking broken at zero
        rowSet(slot, false, "No paired devices", &font_sg_14, COL_DIM,
               "", COL_FAINT2, "pair from the host's BT menu", COL_FAINT2);
      } else {
        rowShow(slot, false);
        s_active[slot] = false;
      }
      continue;
    }
    appBtBondAddr((uint8_t)i, addr, sizeof(addr));
    const bool linked = appBtBondIsConnected((uint8_t)i);
    const bool armed = (s_confirm == slot);
    rowSet(slot, true,
           addr, &font_jbm_12, armed ? COL_RED : COL_TX2,
           armed ? "FORGET?" : (linked ? "LINKED" : "PAIRED"),
           armed ? COL_RED : (linked ? COL_GREEN : COL_FAINT2),
           armed ? "press again to forget"
                 : (linked ? "this host" : "press to forget"),
           COL_FAINT2);
  }

  // Forget-all only earns a row once it does something a single row can't.
  if (n >= 2) {
    const bool armed = (s_confirm == SLOT_FORGET_ALL);
    snprintf(buf, sizeof(buf), "%u devices", (unsigned)n);
    rowSet(SLOT_FORGET_ALL, true,
           "Forget all", &font_sg_14, armed ? COL_RED : COL_TX2,
           armed ? "CONFIRM?" : "CLEAR", armed ? COL_RED : COL_DIM,
           armed ? "press again to forget all" : buf, COL_FAINT2);
  } else {
    rowShow(SLOT_FORGET_ALL, false);
    s_active[SLOT_FORGET_ALL] = false;
  }

  applyFocus();
  s_hash = stateHash();  // applyFocus() may have moved focus off a gone row
}

// ---- Actions ----
static void disarm(void) { s_confirm = -1; }

// First press arms, second one (within the window) commits.
static bool confirmed(int slot) {
  if (s_confirm == slot && (millis() - s_confirmMs) < CONFIRM_MS) {
    disarm();
    return true;
  }
  s_confirm = slot;
  s_confirmMs = millis();
  return false;
}

static void activate(int slot) {
  if (!slotFocusable(slot)) return;
  if (slot == SLOT_DISCOVER) {
    disarm();
    appBtSetDiscoverable(!appBtDiscoverable());
  } else if (slot == SLOT_LINK) {
    disarm();
    // The bond stays: the host is free to walk back in, which is the point of
    // "disconnect" as opposed to "forget".
    appBtDisconnect();
  } else if (slot == SLOT_FORGET_ALL) {
    if (confirmed(slot)) appBtForgetAll();
  } else {
    if (confirmed(slot)) appBtForgetBond((uint8_t)(slot - SLOT_BOND0));
  }
  refresh();
}

static void row_click_cb(lv_event_t *e) {
  appHapticPress();
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  s_focus = slot;
  activate(slot);
}

// ---- Build ----
static void build(void) {
  s_scr = make_screen_base();
  make_marker(s_scr);
  make_back_chevron_to(s_scr, SCR_SETTINGS, "BACK");

  lv_obj_t *hdr = make_label(s_scr, "BLUETOOTH", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(hdr, 3, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 52);

  // The advertised name, so it can be matched against the host's picker.
  lv_obj_t *name = make_label(s_scr, appBtName(), &font_sg_14, COL_TX);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 68);

  s_list = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_size(s_list, LIST_W, LIST_H);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

  for (int i = 0; i < SLOT_N; i++) {
    lv_obj_t *r = lv_obj_create(s_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_set_style_radius(r, 11, 0);
    lv_obj_set_style_pad_hor(r, 13, 0);
    lv_obj_set_style_pad_ver(r, 7, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *t = make_label(r, "", &font_sg_14, COL_TX2);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = make_label(r, "", &font_jbm_10, COL_FAINT2);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 3);
    lv_obj_t *sb = make_label(r, "", &font_jbm_10, COL_FAINT2);
    lv_obj_align(sb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_row[i] = r;
    s_title[i] = t;
    s_val[i] = v;
    s_sub[i] = sb;
    s_active[i] = false;
    rowShow(i, false);
  }

  refresh();
}

// ---- Screen entry points ----
void bt_show(void) {
  if (!s_scr) build();
  else {
    disarm();
    refresh();
  }
  lv_scr_load(s_scr);
}

void bt_encoder(int dir) {
  disarm();  // rotating away from an armed row cancels it
  focusStep(dir);
  refresh();
}

void bt_press(void) { activate(s_focus); }

void bt_conn_changed(void) {
  if (s_scr) refresh();
}

void bt_tick(void) {
  uint32_t now = millis();
  if ((now - s_lastTickMs) < TICK_MS) return;
  s_lastTickMs = now;
  if (s_confirm >= 0 && (now - s_confirmMs) >= CONFIRM_MS) disarm();
  refresh();
}
