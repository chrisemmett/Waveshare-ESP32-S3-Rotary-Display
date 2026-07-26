// DIAL OF DOOM - a one-input first-person shooter for the knob.
//
// The dial is the whole game: you auto-run forward and auto-fire at anything
// your crosshair crosses, so turning is the only thing you actually do. Touch is
// limited to tap-to-start / tap-to-retry and the MENU chevron, exactly as the
// brief asked.
//
// Rendering. The 3D view is a textured raycaster that does NOT go through LVGL -
// pushing 100k pixels a frame into an LVGL canvas would cost a full-screen copy
// on top of the panel transfer. Instead it renders horizontal BANDs into one
// small DMA-capable buffer and pushes each straight at the panel via appBlit().
// LVGL still owns the bottom status bar, the MENU chevron, and the title /
// game-over panel; those live outside the viewport (or are only shown while the
// viewport is frozen), so the two renderers never fight over the same pixels.
//
// The panel is round, so every row is clipped to the display circle and the
// out-of-circle pixels are left black - about a fifth of the fill work saved.
#include "ui_internal.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

// ---- Tunables ----
#define FPS_INVERT_DIR 0      // flip if turning feels mirrored on your unit
#define TURN_PER_DETENT 0.13f // radians added to the aim target per dial click
#define TURN_LERP 14.0f       // how fast the view chases the dial (1/s)
#define MOVE_SPEED 2.1f       // cells/second, always forward
#define PLAYER_R 0.22f
#define FIRE_MS 260           // shot cooldown
#define SHOT_DAMAGE 34
#define SHOT_RANGE 14.0f
#define ENEMY_HP 60
#define ENEMY_R 0.28f
#define ENEMY_REACH 1.30f
#define ENEMY_HIT_MS 1250
#define ENEMY_DAMAGE 7
// Chase speed as a fraction of MOVE_SPEED, flat across every wave. Anything near
// 1.0 lets an imp match you exactly - and since nothing stops an imp and the
// player sharing a cell, a matched-speed imp just rides inside you and neither
// can break away. Half your pace guarantees you can always peel off.
#define ENEMY_SPEED_FRAC 0.50f
#define STRAGGLER_DIST 8.0f     // beyond this an imp hurries to rejoin the fight
#define STRAGGLER_BOOST 1.6f    // still only 0.8x your pace, so it stays escapable
#define STUCK_MS 7000           // no progress for this long -> relocate the imp
#define SLIDE_MS 1200           // wall-hug commitment, long enough to round a corner
#define WAVE_GAP_MS 60000       // breather between waves - go find the medkit
#define PACK_HEAL 100           // the pack is all-or-nothing: back to full
#define PACK_MIN_D 5.0f         // spawn distance from the player, so it must be found
#define PACK_MAX_D 16.0f
#define PACK_PICKUP_R 0.55f     // you auto-run, so the grab radius is generous
#define HURT_FLASH_MS 110       // enemy white-flash after taking a hit
#define DIE_MS 380              // melt-into-the-floor time
#define MAX_ENEMY 8

// ---- Viewport geometry ----
#define VP_W 360
#define VP_H 280              // the rest of the 360px screen is the LVGL status bar
#define BAND_H 28
#define NBAND (VP_H / BAND_H)
#define HORIZON0 145
#define SCR_R 179             // display circle radius (mask)
#define VIG_R 138             // damage vignette starts here

// ---- Map ----
// '.' floor, '#' brick, '=' plating, '%' tech panel. An arena rather than a
// maze: you can't stop moving, so blind corners are punishing and open ground
// with cover to weave around is what makes auto-run readable.
#define MAPW 24
#define MAPH 24
static const char *const MAP[MAPH] = {
  "########################",
  "#......................#",
  "#..==..............==..#",
  "#..==..............==..#",
  "#......................#",
  "#.....%%%......%%%.....#",
  "#.....%%%......%%%.....#",
  "#......................#",
  "#..==....########....==#",
  "#..==....#......#....==#",
  "#........#......#......#",
  "#........#......#......#",
  "#........#......#......#",
  "#........###..###......#",
  "#..==................==#",
  "#..==................==#",
  "#......................#",
  "#.....%%%......%%%.....#",
  "#.....%%%......%%%.....#",
  "#......................#",
  "#..==..............==..#",
  "#..==..............==..#",
  "#......................#",
  "########################",
};

// ---- Sprite art ----
// Hand-drawn indexed bitmaps. ' ' is transparent; every other char indexes a
// palette built at init (see buildPalettes). Kept as text so they stay editable.
#define IMP_W 16
#define IMP_H 22
static const char *const IMP[IMP_H] = {
  "  y          y  ",
  "  yy        yy  ",
  "   yy      yy   ",
  "   yrrrrrrrry   ",
  "   rrrrrrrrrr   ",
  "  rrrrrrrrrrrr  ",
  "  rrkeerreekrr  ",
  "  rrkeerreekrr  ",
  "  rrrrrrrrrrrr  ",
  "   rrkkkkkkrr   ",
  "   rrkwkwkwrr   ",
  "   rrrrrrrrrr   ",
  "    rrrrrrrr    ",
  "  RRrrrrrrrrRR  ",
  " RRrrrrrrrrrrRR ",
  "RRrrrrrrrrrrrrRR",
  "RR  rrrrrrrr  RR",
  "    rrrrrrrr    ",
  "    rrrrrrrr    ",
  "   rrrr  rrrr   ",
  "   rrr    rrr   ",
  "   ddd    ddd   ",
};

// Shotgun from behind: barrel up the middle, receiver, gloved hands gripping the
// stock. At GUN_SCALE it fills 140x90 - about a third of the view height, which
// is roughly where Doom puts the weapon.
#define GUN_W 28
#define GUN_H 18
#define GUN_SCALE 5
static const char *const GUN[GUN_H] = {
  "           kbbbbk           ",
  "           kbGGbk           ",
  "           kbGGbk           ",
  "          kbbGGbbk          ",
  "         kbGGGGGGbk         ",
  "         kbGGggGGbk         ",
  "        kbbGGggGGbbk        ",
  "        kbGGGGGGGGbk        ",
  "      kbbgGGGGGGGGgbbk      ",
  "      kbggGGGGGGGGggbk      ",
  "      kggggGGGGGGggggk      ",
  "     kwwggggGGGGggggwwk     ",
  "    kwWWwwggggggggwwWWwk    ",
  "   khhwWWwwggggggwwWWwhhk   ",
  "   khhhwWWWwwwwwwWWWwhhhk   ",
  "  khhhhwWWWWWWWWWWWWwhhhhk  ",
  "  kkhhhhwWWWWWWWWWWwhhhhkk  ",
  "  kkkhhhhhwwwwwwwwhhhhhkkk  ",
};

// Medkit: a white case with a red cross and a carry handle. Drawn through the
// same billboard path as the imps, just shorter and pinned to the floor.
#define PACK_W 16
#define PACK_H 14
#define PACK_SCALE 0.34f  // fraction of a full cell height, so it reads as a box
#define PACK_HOVER 0.10f  // bob amplitude, also as a fraction of a cell
static const char *const PACK[PACK_H] = {
  "     gggggg     ",
  "     gg  gg     ",
  "  kkkkkkkkkkkk  ",
  " kwwwwwwwwwwwwk ",
  " kwwwwwwwwwwwwk ",
  " kwwwwwRRwwwwwk ",
  " kwwwwwRRwwwwwk ",
  " kwwRRRRRRRRwwk ",
  " kwwRRRRRRRRwwk ",
  " kwwwwwRRwwwwwk ",
  " kwwwwwRRwwwwwk ",
  " kWWWWWWWWWWWWk ",
  " kWWWWWWWWWWWWk ",
  "  kkkkkkkkkkkk  ",
};

// ============================ State ============================
enum FpsState { FPS_READY, FPS_PLAY, FPS_DEAD };

struct Enemy {
  float x, y;
  int hp;
  bool alive;
  uint32_t dieAt;       // non-zero while melting away
  uint32_t hurtAt;      // white-flash expiry
  uint32_t swingAt;     // next melee allowed
  int8_t slide;         // committed wall-hug direction, +1/-1, 0 = going direct
  uint32_t slideUntil;  // wall-hug commitment expiry
  float bestD;          // closest it has ever got to you - the progress watchdog
  uint32_t progressAt;  // last time it made progress, for the stuck watchdog
};

static FpsState s_state = FPS_READY;
static float s_px, s_py;          // player position (cells)
static float s_ang, s_angTarget;  // view angle / dial target (radians)
static float s_bob;               // walk-cycle phase
static int s_hp, s_kills, s_wave;
static uint32_t s_lastMs, s_fireAt, s_flashAt;
static Enemy s_en[MAX_ENEMY];

// Between-waves breather. The arena is empty for WAVE_GAP_MS and a single medkit
// is out there somewhere; walk over it and you're back to full.
static bool s_gap;
static uint32_t s_gapUntil;
static bool s_packAlive;
static float s_packX, s_packY;
static int s_hudSec;  // last countdown second painted into the status bar

// Radial screen tint (damage = red, new wave = gold), dithered over the rim.
static uint32_t s_tintFrom, s_tintTo;
static uint16_t s_tintCol;

// ---- Render scratch ----
static uint16_t *s_band = nullptr;  // BAND_H rows of the viewport, DMA-capable
static int32_t s_colTop[VP_W];      // unclipped wall top / bottom, per column
static int32_t s_colBot[VP_W];
static int32_t s_colStep[VP_W];     // 16.16 texture rows per screen row
static uint8_t s_colTexX[VP_W];
static uint16_t s_colLit[VP_W], s_colDark[VP_W];
static float s_zbuf[VP_W];
static int16_t s_rowA[VP_H], s_rowB[VP_H];  // display-circle span, per row
static uint16_t s_ceil[VP_H], s_floor[VP_H];
static uint8_t s_tex[256];          // 16x16 running-bond mask (1 = brick face)
static float s_dirX, s_dirY, s_planeX, s_planeY;

// Palettes: [shade 0..7][colour index]; row 8 is the hit flash.
#define NSHADE 8
static uint16_t s_impPal[NSHADE + 1][8];
static uint16_t s_gunPal[8];
// The medkit ignores distance shading and pulses instead - a pickup you have to
// find has to stay legible at the far end of the arena.
#define NPULSE 3
static uint16_t s_packPal[NPULSE][8];
static uint8_t s_impIdx[128], s_gunIdx[128], s_packIdx[128];

// Ordered dither, so the rim tint costs a compare instead of a blend.
static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

// ---- LVGL bits ----
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_panel = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_sub = nullptr;
static lv_obj_t *s_hint = nullptr;
static lv_obj_t *s_hpBar = nullptr;
static lv_obj_t *s_stat = nullptr;
static bool s_built = false;

// ============================ Colour helpers ============================
// The panel wants big-endian RGB565 (same as LVGL's LV_COLOR_16_SWAP flush), so
// every colour is byte-swapped once at build time and only copied afterwards.
static inline uint16_t be565(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}
// `s` is a 0..256 brightness scale applied to a 0xRRGGBB literal.
static inline uint16_t shade(uint32_t rgb, int s) {
  return be565((int)(((rgb >> 16) & 0xFF) * s) >> 8,
               (int)(((rgb >> 8) & 0xFF) * s) >> 8,
               (int)((rgb & 0xFF) * s) >> 8);
}

static inline bool cellSolid(int mx, int my) {
  if (mx < 0 || my < 0 || mx >= MAPW || my >= MAPH) return true;
  return MAP[my][mx] != '.';
}
// Axis-aligned box test, so the player slides along walls instead of sticking.
static bool boxBlocked(float x, float y, float r) {
  return cellSolid((int)(x - r), (int)(y - r)) || cellSolid((int)(x + r), (int)(y - r)) ||
         cellSolid((int)(x - r), (int)(y + r)) || cellSolid((int)(x + r), (int)(y + r));
}
static bool losClear(float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0;
  int n = (int)(sqrtf(dx * dx + dy * dy) * 4.0f);
  for (int i = 1; i < n; i++) {
    float t = (float)i / (float)n;
    if (cellSolid((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
  }
  return true;
}
static inline float wrapPi(float a) {
  while (a > (float)M_PI) a -= 2.0f * (float)M_PI;
  while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
  return a;
}

// ============================ One-time tables ============================
static void buildPalettes(void) {
  static const uint32_t IMP_BASE[8] = {
    0x000000,  // 0 = transparent, never read
    0x1a0e0e,  // k outline
    0x4a1810,  // d hoof
    0x8f2a1c,  // r hide
    0xc94a2c,  // R highlight
    0xd8c48a,  // y horn
    0xefe6dc,  // w tooth
    0xffd24a,  // e eye
  };
  for (int s = 0; s < NSHADE; s++)
    for (int i = 1; i < 8; i++) s_impPal[s][i] = shade(IMP_BASE[i], 40 + s * 31);
  for (int i = 1; i < 8; i++) s_impPal[NSHADE][i] = be565(255, 236, 214);  // hit flash

  memset(s_impIdx, 0, sizeof(s_impIdx));
  s_impIdx[(uint8_t)'k'] = 1; s_impIdx[(uint8_t)'d'] = 2; s_impIdx[(uint8_t)'r'] = 3;
  s_impIdx[(uint8_t)'R'] = 4; s_impIdx[(uint8_t)'y'] = 5; s_impIdx[(uint8_t)'w'] = 6;
  s_impIdx[(uint8_t)'e'] = 7;

  static const uint32_t GUN_BASE[8] = {
    0x000000,  // 0 = transparent
    0x14161a,  // k outline
    0x24282e,  // b barrel
    0x4d5158,  // g receiver
    0x7b818a,  // G receiver highlight
    0x5e3d22,  // w stock
    0x82552f,  // W stock highlight
    0x55663c,  // h glove - olive, so the hands read apart from the wood
  };
  for (int i = 1; i < 8; i++) s_gunPal[i] = shade(GUN_BASE[i], 256);
  memset(s_gunIdx, 0, sizeof(s_gunIdx));
  s_gunIdx[(uint8_t)'k'] = 1; s_gunIdx[(uint8_t)'b'] = 2; s_gunIdx[(uint8_t)'g'] = 3;
  s_gunIdx[(uint8_t)'G'] = 4; s_gunIdx[(uint8_t)'w'] = 5; s_gunIdx[(uint8_t)'W'] = 6;
  s_gunIdx[(uint8_t)'h'] = 7;

  static const uint32_t PACK_BASE[8] = {
    0x000000,  // 0 = transparent
    0x161a1c,  // k outline
    0xf2f0ea,  // w case
    0xc3bfb4,  // W case shadow
    0xd8342a,  // R cross
    0x000000,  // (unused)
    0x000000,  // (unused)
    0x8f959b,  // g handle
  };
  static const int PACK_LEVEL[NPULSE] = { 190, 224, 256 };
  for (int p = 0; p < NPULSE; p++)
    for (int i = 1; i < 8; i++) s_packPal[p][i] = shade(PACK_BASE[i], PACK_LEVEL[p]);
  memset(s_packIdx, 0, sizeof(s_packIdx));
  s_packIdx[(uint8_t)'k'] = 1; s_packIdx[(uint8_t)'w'] = 2; s_packIdx[(uint8_t)'W'] = 3;
  s_packIdx[(uint8_t)'R'] = 4; s_packIdx[(uint8_t)'g'] = 7;
}

static void buildTables(void) {
  // Running-bond brick mask: mortar on rows 0/8, staggered vertical joints.
  for (int ty = 0; ty < 16; ty++)
    for (int tx = 0; tx < 16; tx++) {
      bool mortar = (ty % 8) == 0 || (ty < 8 ? tx == 0 : tx == 8);
      s_tex[(ty << 4) | tx] = mortar ? 0 : 1;
    }

  // Round-display mask + the floor/ceiling gradients, both per screen row.
  for (int y = 0; y < VP_H; y++) {
    int dy = y - SCR_CY;
    int h2 = SCR_R * SCR_R - dy * dy;
    if (h2 <= 0) { s_rowA[y] = 1; s_rowB[y] = 0; }
    else {
      int hw = (int)sqrtf((float)h2);
      s_rowA[y] = (int16_t)(SCR_CX - hw);
      s_rowB[y] = (int16_t)(SCR_CX + hw - 1);
    }
    int d = y - HORIZON0;
    if (d < 0) d = -d;
    int s = 48 + d * 200 / HORIZON0;
    if (s > 248) s = 248;
    s_ceil[y] = shade(0x39404f, s);
    s_floor[y] = shade(0x554330, s);
  }
}

// ============================ Game logic ============================
// Find a random floor cell whose distance from the player is within [minD, maxD].
// Cell CENTRES only: on a cell corner a collision box straddles four cells, and
// an imp born touching a wall never moves again.
static bool pickCell(float *ox, float *oy, float minD, float maxD) {
  for (int tries = 0; tries < 80; tries++) {
    float x = (float)(1 + (int)(appRandom() % (MAPW - 2))) + 0.5f;
    float y = (float)(1 + (int)(appRandom() % (MAPH - 2))) + 0.5f;
    if (cellSolid((int)x, (int)y)) continue;
    float dx = x - s_px, dy = y - s_py;
    float d2 = dx * dx + dy * dy;
    if (d2 < minD * minD || d2 > maxD * maxD) continue;
    *ox = x; *oy = y;
    return true;
  }
  return false;
}

static bool placeEnemy(Enemy *e, float minD, float maxD) {
  if (!pickCell(&e->x, &e->y, minD, maxD)) return false;
  e->slide = 0;
  e->slideUntil = 0;
  e->bestD = 1e9f;
  e->progressAt = millis();
  return true;
}

// The medkit goes anywhere on the floor that isn't within arm's reach, so the
// breather is actually spent looking for it.
static void spawnPack(void) {
  s_packAlive = pickCell(&s_packX, &s_packY, PACK_MIN_D, PACK_MAX_D);
}

static void spawnWave(void) {
  int n = 3 + s_wave;
  if (n > MAX_ENEMY) n = MAX_ENEMY;
  for (int i = 0; i < MAX_ENEMY; i++) s_en[i].alive = false;
  for (int i = 0; i < n; i++) {
    if (!placeEnemy(&s_en[i], 6.0f, 999.0f)) continue;  // never spawn in your lap
    s_en[i].hp = ENEMY_HP;
    s_en[i].alive = true;
    s_en[i].dieAt = 0; s_en[i].hurtAt = 0; s_en[i].swingAt = 0;
  }
}

static void newGame(void) {
  s_px = 2.5f; s_py = 11.5f;
  s_ang = s_angTarget = 0.0f;
  s_bob = 0.0f;
  s_hp = 100; s_kills = 0; s_wave = 1;
  s_lastMs = millis();
  s_fireAt = 0; s_flashAt = 0;
  s_tintFrom = s_tintTo = 0;
  s_gap = false; s_gapUntil = 0;
  s_packAlive = false;
  s_hudSec = -1;
  spawnWave();
}

static void setTint(uint16_t col, uint32_t ms) {
  s_tintFrom = millis();
  s_tintTo = s_tintFrom + ms;
  s_tintCol = col;
}

// Auto-fire: pick the closest live enemy actually covered by the crosshair. The
// tolerance is the target's own angular half-width, so it reads as "shoot what
// you can see" rather than a magnet.
static void autoFire(uint32_t now) {
  if (now < s_fireAt) return;
  int best = -1;
  float bestD = SHOT_RANGE;
  for (int i = 0; i < MAX_ENEMY; i++) {
    Enemy *e = &s_en[i];
    if (!e->alive || e->dieAt) continue;
    float dx = e->x - s_px, dy = e->y - s_py;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > bestD || d < 0.05f) continue;
    float da = wrapPi(atan2f(dy, dx) - s_ang);
    if (da < 0) da = -da;
    if (da > atan2f(0.42f, d) + 0.02f) continue;
    if (!losClear(s_px, s_py, e->x, e->y)) continue;
    best = i; bestD = d;
  }
  if (best < 0) return;

  s_fireAt = now + FIRE_MS;
  s_flashAt = now + 60;
  appHapticTick();  // the trigger is the only thing you can feel

  Enemy *e = &s_en[best];
  e->hp -= SHOT_DAMAGE;
  e->hurtAt = now + HURT_FLASH_MS;
  if (e->hp <= 0) {
    e->dieAt = now + DIE_MS;
    s_kills++;
    appHapticPress();
  }
}

static void stepEnemies(uint32_t now, float dt) {
  // Half your pace, every wave. Escalation comes from the head-count (3 + wave,
  // up to 8) and from being surrounded, not from foot speed - an imp that keeps
  // up with you can never be shaken off, and shares your cell while it swings.
  // Raise ENEMY_SPEED_FRAC to make the chase tighter; at 1.0 they glue to you.
  float speed = MOVE_SPEED * ENEMY_SPEED_FRAC;
  int liveCount = 0;

  for (int i = 0; i < MAX_ENEMY; i++) {
    Enemy *e = &s_en[i];
    if (!e->alive) continue;
    // A corpse still counts as live, so the wave doesn't flip (and wipe the
    // sprite) before the last imp has finished melting.
    liveCount++;
    if (e->dieAt) {
      if (now >= e->dieAt) e->alive = false;
      continue;
    }

    float dx = s_px - e->x, dy = s_py - e->y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.001f) continue;
    dx /= d; dy /= d;

    if (d > ENEMY_REACH * 0.8f) {
      // Stragglers hustle. Without this, one imp you happened to outrun trails
      // you at a fixed distance forever and the wave never ends.
      float sp = speed * (d > STRAGGLER_DIST ? STRAGGLER_BOOST : 1.0f);
      float mx, my;
      if (e->slide != 0 && now < e->slideUntil) {
        // Walking around an obstacle: mostly tangential, with enough pull toward
        // you that it still closes in as it rounds the corner.
        mx = -dy * (float)e->slide * 0.85f + dx * 0.15f;
        my = dx * (float)e->slide * 0.85f + dy * 0.15f;
      } else {
        e->slide = 0;
        // Drift slightly off the direct line so a pack doesn't collapse into one
        // sprite - and so they arrive from a spread of angles.
        float wob = sinf((float)now * 0.0016f + (float)i) * 0.35f;
        mx = dx - dy * wob;
        my = dy + dx * wob;
      }

      float ox = e->x, oy = e->y;
      float nx = e->x + mx * sp * dt;
      float ny = e->y + my * sp * dt;
      if (!boxBlocked(nx, e->y, ENEMY_R)) e->x = nx;
      if (!boxBlocked(e->x, ny, ENEMY_R)) e->y = ny;

      // Steering straight at you is no plan at all against a wall. If the move
      // went nowhere, pick a side and commit to it for SLIDE_MS - long enough to
      // actually round the corner. Re-deciding every frame (which is what
      // clearing this on any movement does) just jitters against the wall forever.
      if (e->slide == 0 && fabsf(e->x - ox) + fabsf(e->y - oy) < sp * dt * 0.4f) {
        e->slide = (i & 1) ? 1 : -1;
        e->slideUntil = now + SLIDE_MS;
      }
    }

    bool canSee = losClear(e->x, e->y, s_px, s_py);

    // Watchdog. Wall-hugging handles corners; concave pockets and stand-offs
    // through a wall it can't. If an imp hasn't got meaningfully closer in
    // STUCK_MS, quietly relocate it.
    //
    // "Engaged" counts as progress, because an imp on top of you deliberately
    // stops moving to swing - but it has to be able to SEE you. A pack pressed
    // against the far side of a wall is 1.5 cells away and perfectly stuck: it
    // can't swing and you can't shoot it, so that must not read as progress.
    if (d < e->bestD - 0.5f || (canSee && d <= ENEMY_REACH * 1.5f)) {
      e->bestD = d;
      e->progressAt = now;
    } else if (now - e->progressAt > STUCK_MS && placeEnemy(e, 7.0f, 12.0f)) {
      continue;
    }

    if (canSee && d <= ENEMY_REACH && now >= e->swingAt) {
      e->swingAt = now + ENEMY_HIT_MS;
      s_hp -= ENEMY_DAMAGE;
      setTint(be565(220, 30, 24), 220);
      appHapticPress();
      if (s_hp <= 0) s_hp = 0;
    }
  }

  if (liveCount == 0 && !s_gap && s_hp > 0) {
    // Wave down. Nothing chases you for the next minute; the only thing in the
    // arena is one medkit, and finding it is the whole point of the breather.
    s_gap = true;
    s_gapUntil = now + WAVE_GAP_MS;
    setTint(be565(230, 168, 70), 420);
    spawnPack();
  }
}

// The gap: run the clock, and see whether you've run over the medkit yet.
static void stepGap(uint32_t now) {
  if (s_packAlive) {
    float dx = s_packX - s_px, dy = s_packY - s_py;
    if (dx * dx + dy * dy <= PACK_PICKUP_R * PACK_PICKUP_R) {
      s_packAlive = false;
      s_hp = PACK_HEAL;
      setTint(be565(70, 210, 120), 420);
      appHapticPress();
    }
  }
  if ((int32_t)(now - s_gapUntil) >= 0) {
    s_gap = false;
    s_packAlive = false;  // anything left behind goes with the wave
    s_wave++;
    spawnWave();
  }
}

// ============================ Raycaster ============================
static void castRays(int horizon) {
  s_dirX = cosf(s_ang); s_dirY = sinf(s_ang);
  s_planeX = -s_dirY * 0.66f; s_planeY = s_dirX * 0.66f;

  for (int x = 0; x < VP_W; x++) {
    float camX = 2.0f * (float)x / (float)VP_W - 1.0f;
    float rdx = s_dirX + s_planeX * camX;
    float rdy = s_dirY + s_planeY * camX;
    int mapX = (int)s_px, mapY = (int)s_py;
    float ddx = (rdx == 0.0f) ? 1e30f : fabsf(1.0f / rdx);
    float ddy = (rdy == 0.0f) ? 1e30f : fabsf(1.0f / rdy);
    int stepX, stepY;
    float sdx, sdy;
    if (rdx < 0) { stepX = -1; sdx = (s_px - mapX) * ddx; }
    else         { stepX = 1;  sdx = (mapX + 1.0f - s_px) * ddx; }
    if (rdy < 0) { stepY = -1; sdy = (s_py - mapY) * ddy; }
    else         { stepY = 1;  sdy = (mapY + 1.0f - s_py) * ddy; }

    int side = 0;
    for (int guard = 0; guard < 64; guard++) {
      if (sdx < sdy) { sdx += ddx; mapX += stepX; side = 0; }
      else           { sdy += ddy; mapY += stepY; side = 1; }
      if (cellSolid(mapX, mapY)) break;
    }

    float perp = (side == 0) ? (sdx - ddx) : (sdy - ddy);
    if (perp < 0.05f) perp = 0.05f;
    s_zbuf[x] = perp;

    // Where along the wall face did we land? -> texture column.
    float wall = (side == 0) ? (s_py + perp * rdy) : (s_px + perp * rdx);
    wall -= floorf(wall);
    int texX = (int)(wall * 16.0f);
    if (texX < 0) texX = 0; else if (texX > 15) texX = 15;
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) texX = 15 - texX;
    s_colTexX[x] = (uint8_t)texX;

    int lh = (int)((float)VP_H / perp);
    if (lh < 1) lh = 1;
    s_colTop[x] = horizon - lh / 2;
    s_colBot[x] = s_colTop[x] + lh - 1;
    s_colStep[x] = (int32_t)((16 << 16) / lh);

    uint32_t lit, drk;
    char cell = (mapX >= 0 && mapY >= 0 && mapX < MAPW && mapY < MAPH) ? MAP[mapY][mapX] : '#';
    if (cell == '=')      { lit = 0x5f7490; drk = 0x36445a; }
    else if (cell == '%') { lit = 0x4f8a5c; drk = 0x2c5236; }
    else                  { lit = 0x9c7b52; drk = 0x5a4530; }

    int sh = 256 - (int)(perp * 13.0f);
    if (sh < 45) sh = 45;
    if (side == 1) sh = sh * 7 / 10;  // N/S faces darker, the classic depth cue
    s_colLit[x] = shade(lit, sh);
    s_colDark[x] = shade(drk, sh);
  }
}

static void drawWalls(int y0) {
  for (int yy = 0; yy < BAND_H; yy++) {
    int Y = y0 + yy;
    int xa = s_rowA[Y], xb = s_rowB[Y];
    if (xa > xb) continue;
    uint16_t *row = s_band + yy * VP_W;
    uint16_t cc = s_ceil[Y], cf = s_floor[Y];
    for (int x = xa; x <= xb; x++) {
      int32_t top = s_colTop[x];
      if (Y < top) { row[x] = cc; continue; }
      if (Y > s_colBot[x]) { row[x] = cf; continue; }
      int32_t ty = ((Y - top) * s_colStep[x]) >> 16;
      if (ty < 0) ty = 0; else if (ty > 15) ty = 15;
      row[x] = s_tex[(ty << 4) | s_colTexX[x]] ? s_colLit[x] : s_colDark[x];
    }
  }
}

// Billboarded sprites, resolved once per frame and then rasterised per band.
struct Spr {
  int left, top, w, h, srcW, srcH;
  float depth;
  const uint16_t *pal;
  const char *const *art;  // indexed bitmap rows
  const uint8_t *idx;      // char -> palette index
};
static Spr s_spr[MAX_ENEMY + 1];  // + the medkit
static int s_nspr;

static void prepSprites(uint32_t now, int horizon) {
  s_nspr = 0;
  float invDet = s_planeX * s_dirY - s_dirX * s_planeY;
  if (fabsf(invDet) < 1e-6f) return;
  invDet = 1.0f / invDet;

  for (int i = 0; i < MAX_ENEMY; i++) {
    Enemy *e = &s_en[i];
    if (!e->alive) continue;
    float sx = e->x - s_px, sy = e->y - s_py;
    float tx = invDet * (s_dirY * sx - s_dirX * sy);
    float tz = invDet * (-s_planeY * sx + s_planeX * sy);
    if (tz < 0.25f) continue;

    int h = (int)((float)VP_H / tz);
    int w = h * IMP_W / IMP_H;
    int cx = (int)((float)(VP_W / 2) * (1.0f + tx / tz));
    int bottom = horizon + h / 2;  // feet on the same floor plane as the walls
    int srcH = IMP_H;

    // Dying: melt down into the floor, bottom pinned.
    if (e->dieAt) {
      float k = (float)(e->dieAt - now) / (float)DIE_MS;
      if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
      h = (int)(h * (0.25f + 0.75f * k));
      if (h < 2) continue;
    }
    if (cx + w / 2 < 0 || cx - w / 2 >= VP_W || h < 2 || w < 1) continue;

    Spr *s = &s_spr[s_nspr++];
    s->left = cx - w / 2;
    s->top = bottom - h;
    s->w = w; s->h = h; s->srcW = IMP_W; s->srcH = srcH;
    s->depth = tz;
    s->art = IMP; s->idx = s_impIdx;
    if (e->hurtAt && now < e->hurtAt) s->pal = s_impPal[NSHADE];
    else if (e->dieAt)               s->pal = s_impPal[0];
    else {
      int sh = NSHADE - 1 - (int)(tz * 0.55f);
      if (sh < 0) sh = 0; else if (sh > NSHADE - 1) sh = NSHADE - 1;
      s->pal = s_impPal[sh];
    }
  }

  // The medkit. Same billboard maths, but a third of a cell tall, hovering just
  // off the floor and pulsing rather than distance-shaded so it stays findable.
  if (s_packAlive) {
    float sx = s_packX - s_px, sy = s_packY - s_py;
    float tx = invDet * (s_dirY * sx - s_dirX * sy);
    float tz = invDet * (-s_planeY * sx + s_planeX * sy);
    if (tz >= 0.25f) {
      float full = (float)VP_H / tz;                    // a 1-cell-tall object
      int h = (int)(full * PACK_SCALE);
      int w = h * PACK_W / PACK_H;
      int cx = (int)((float)(VP_W / 2) * (1.0f + tx / tz));
      float hover = (0.5f + 0.5f * sinf((float)now * 0.0035f)) * PACK_HOVER;
      int bottom = horizon + (int)(full * (0.5f - hover));
      if (cx + w / 2 >= 0 && cx - w / 2 < VP_W && h >= 2 && w >= 1) {
        Spr *s = &s_spr[s_nspr++];
        s->left = cx - w / 2;
        s->top = bottom - h;
        s->w = w; s->h = h; s->srcW = PACK_W; s->srcH = PACK_H;
        s->depth = tz;
        s->art = PACK; s->idx = s_packIdx;
        int k = (int)(now / 200) % 4;                   // 0,1,2,1 - a slow throb
        s->pal = s_packPal[k < NPULSE ? k : NPULSE - 2];
      }
    }
  }

  // Painter's order: furthest first, so nearer imps overlap correctly.
  for (int i = 1; i < s_nspr; i++) {
    Spr tmp = s_spr[i];
    int j = i - 1;
    while (j >= 0 && s_spr[j].depth < tmp.depth) { s_spr[j + 1] = s_spr[j]; j--; }
    s_spr[j + 1] = tmp;
  }
}

static void drawSprites(int y0) {
  int y1 = y0 + BAND_H - 1;
  for (int i = 0; i < s_nspr; i++) {
    Spr *s = &s_spr[i];
    int ta = s->top > y0 ? s->top : y0;
    int tb = (s->top + s->h - 1) < y1 ? (s->top + s->h - 1) : y1;
    if (ta > tb) continue;
    int32_t stepX = ((int32_t)s->srcW << 16) / s->w;
    int32_t stepY = ((int32_t)s->srcH << 16) / s->h;

    for (int Y = ta; Y <= tb; Y++) {
      int xa = s->left > s_rowA[Y] ? s->left : s_rowA[Y];
      int xb = (s->left + s->w - 1) < s_rowB[Y] ? (s->left + s->w - 1) : s_rowB[Y];
      if (xa > xb) continue;
      int32_t sy = ((Y - s->top) * stepY) >> 16;
      if (sy < 0) sy = 0; else if (sy >= s->srcH) sy = s->srcH - 1;
      const char *srow = s->art[sy];
      uint16_t *row = s_band + (Y - y0) * VP_W;
      int32_t acc = (xa - s->left) * stepX;
      for (int x = xa; x <= xb; x++, acc += stepX) {
        if (s_zbuf[x] <= s->depth) continue;  // behind a wall
        int sxi = acc >> 16;
        if (sxi < 0 || sxi >= s->srcW) continue;
        uint8_t pi = s->idx[(uint8_t)srow[sxi]];
        if (pi) row[x] = s->pal[pi];
      }
    }
  }
}

// The weapon sits in front of everything, bobbing with the walk cycle.
static void drawGun(int y0, uint32_t now) {
  int gw = GUN_W * GUN_SCALE, gh = GUN_H * GUN_SCALE;
  int bx = (VP_W - gw) / 2 + (int)(sinf(s_bob) * 6.0f);
  int by = VP_H - gh + (int)(fabsf(cosf(s_bob)) * 5.0f);
  int y1 = y0 + BAND_H - 1;

  // Muzzle flash first, so the barrel draws back over its base and the bloom
  // reads as coming out of the gun rather than sitting on top of it.
  if (s_flashAt && now < s_flashAt) {
    int fx = bx + gw / 2, fy = by - 2;
    for (int Y = (fy - 20 > y0 ? fy - 20 : y0); Y <= (fy + 20 < y1 ? fy + 20 : y1); Y++) {
      if (Y < 0 || Y >= VP_H) continue;
      uint16_t *row = s_band + (Y - y0) * VP_W;
      int dy = Y - fy;
      for (int x = fx - 20; x <= fx + 20; x++) {
        if (x < s_rowA[Y] || x > s_rowB[Y]) continue;
        int dx = x - fx;
        int r2 = dx * dx + dy * dy;
        if (r2 < 90) row[x] = be565(255, 250, 220);
        else if (r2 < 320) row[x] = be565(255, 196, 72);
      }
    }
  }

  int ta = by > y0 ? by : y0;
  int tb = (by + gh - 1) < y1 ? (by + gh - 1) : y1;
  if (ta > tb) return;
  for (int Y = ta; Y <= tb; Y++) {
    if (Y >= VP_H) break;
    const char *srow = GUN[(Y - by) / GUN_SCALE];
    uint16_t *row = s_band + (Y - y0) * VP_W;
    int xa = bx > s_rowA[Y] ? bx : s_rowA[Y];
    int xb = (bx + gw - 1) < s_rowB[Y] ? (bx + gw - 1) : s_rowB[Y];
    for (int x = xa; x <= xb; x++) {
      uint8_t pi = s_gunIdx[(uint8_t)srow[(x - bx) / GUN_SCALE]];
      if (pi) row[x] = s_gunPal[pi];
    }
  }
}

static void drawCrosshair(int y0, int horizon) {
  uint16_t c = be565(233, 164, 76);
  int y1 = y0 + BAND_H - 1;
  for (int k = 4; k <= 10; k++) {
    int ys[2] = { horizon - k, horizon + k };
    for (int n = 0; n < 2; n++) {
      int Y = ys[n];
      if (Y < y0 || Y > y1 || Y < 0 || Y >= VP_H) continue;
      s_band[(Y - y0) * VP_W + SCR_CX] = c;
    }
    if (horizon >= y0 && horizon <= y1 && horizon >= 0 && horizon < VP_H) {
      uint16_t *row = s_band + (horizon - y0) * VP_W;
      row[SCR_CX - k] = c;
      row[SCR_CX + k] = c;
    }
  }
}

// Radial damage / wave flash, dithered so it costs a compare per pixel instead
// of an alpha blend - and reads as a proper CRT-ish vignette on a round panel.
static void drawTint(int y0, uint32_t now) {
  if (!s_tintTo || now >= s_tintTo) return;
  int span = (int)(s_tintTo - s_tintFrom);
  if (span <= 0) return;
  int inten = (int)((s_tintTo - now) * 16 / (uint32_t)span);  // 0..16
  if (inten <= 0) return;

  for (int yy = 0; yy < BAND_H; yy++) {
    int Y = y0 + yy;
    int xa = s_rowA[Y], xb = s_rowB[Y];
    if (xa > xb) continue;
    int dy = Y - SCR_CY;
    int dy2 = dy * dy;
    uint16_t *row = s_band + yy * VP_W;
    const uint8_t *bay = &BAYER[(Y & 3) << 2];
    for (int x = xa; x <= xb; x++) {
      int dx = x - SCR_CX;
      int r2 = dx * dx + dy2;
      if (r2 <= VIG_R * VIG_R) continue;
      int a = (r2 - VIG_R * VIG_R) * inten / (SCR_R * SCR_R - VIG_R * VIG_R);
      if (a > 16) a = 16;
      if (bay[x & 3] < a) row[x] = s_tintCol;
    }
  }
}

static void renderFrame(uint32_t now) {
  int horizon = HORIZON0 + (int)(sinf(s_bob * 2.0f) * 3.0f);
  castRays(horizon);
  prepSprites(now, horizon);
  for (int b = 0; b < NBAND; b++) {
    int y0 = b * BAND_H;
    memset(s_band, 0, (size_t)VP_W * BAND_H * sizeof(uint16_t));
    drawWalls(y0);
    drawSprites(y0);
    drawGun(y0, now);
    drawCrosshair(y0, horizon);
    drawTint(y0, now);
    appBlit(0, (int16_t)y0, VP_W, BAND_H, s_band);
  }
}

// ============================ LVGL chrome ============================
// Seconds left in the breather, rounded up, 0 when a wave is running.
static int gapSecs(uint32_t now) {
  if (!s_gap) return 0;
  int32_t left = (int32_t)(s_gapUntil - now);
  if (left < 0) left = 0;
  return (int)((left + 999) / 1000);
}

static void updateHud(void) {
  lv_bar_set_value(s_hpBar, s_hp, LV_ANIM_OFF);
  uint32_t c = s_hp > 60 ? COL_GREEN : (s_hp > 25 ? COL_ACCENT : COL_RED);
  lv_obj_set_style_bg_color(s_hpBar, lv_color_hex(c), LV_PART_INDICATOR);
  char b[48];
  if (s_gap) {
    int secs = gapSecs(millis());
    s_hudSec = secs;
    // While the medkit is still out there, say so - the countdown alone doesn't
    // tell you whether there's anything left to look for.
    if (s_packAlive) snprintf(b, sizeof(b), "HP %d   MEDKIT OUT   %ds", s_hp, secs);
    else             snprintf(b, sizeof(b), "HP %d   WAVE %d IN %ds", s_hp, s_wave + 1, secs);
  } else {
    s_hudSec = -1;
    snprintf(b, sizeof(b), "HP %d   WAVE %d   KILLS %d", s_hp, s_wave, s_kills);
  }
  lv_label_set_text(s_stat, b);
}

static void showPanel(bool dead) {
  char b[48];
  if (dead) {
    lv_label_set_text(s_title, "YOU DIED");
    lv_obj_set_style_text_color(s_title, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(COL_RED), 0);
    lv_label_set_text(s_sub, "TAP TO RETRY");
    snprintf(b, sizeof(b), "%d KILLS  ·  REACHED WAVE %d", s_kills, s_wave);
    lv_label_set_text(s_hint, b);
  } else {
    lv_label_set_text(s_title, "DIAL OF DOOM");
    lv_obj_set_style_text_color(s_title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(COL_ACCENT), 0);
    lv_label_set_text(s_sub, "TAP TO START");
    lv_label_set_text(s_hint, "TURN THE DIAL TO AIM\nYOU RUN AND FIRE ON YOUR OWN\nGRAB THE MEDKIT BETWEEN WAVES");
  }
  lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_panel);
  // The raycaster has been scribbling straight onto the panel behind LVGL's
  // back, so make LVGL repaint the whole screen from its own object tree.
  lv_obj_invalidate(s_scr);
}

static void startGame(void) {
  if (!s_band) return;  // no blit buffer -> stay on the title card
  newGame();
  updateHud();
  s_state = FPS_PLAY;
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

static void die(void) {
  s_state = FPS_DEAD;
  s_hp = 0;
  updateHud();
  appHapticAlarm();
  showPanel(true);
}

static void tap_cb(lv_event_t *e) {
  (void)e;
  if (s_state == FPS_PLAY) return;  // no accidental restarts mid-run
  appHapticPress();
  startGame();
}

static void build(void) {
  s_scr = make_screen_base();

  // Tap target over the viewport only, so it never steals the MENU chevron.
  lv_obj_t *hit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(hit);
  lv_obj_set_size(hit, VP_W, VP_H);
  lv_obj_align(hit, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hit, tap_cb, LV_EVENT_CLICKED, NULL);

  // ---- Status bar (LVGL owns everything below the viewport) ----
  s_hpBar = lv_bar_create(s_scr);
  lv_obj_set_size(s_hpBar, 170, 8);
  lv_obj_align(s_hpBar, LV_ALIGN_TOP_MID, 0, VP_H + 6);
  lv_bar_set_range(s_hpBar, 0, 100);
  lv_obj_set_style_radius(s_hpBar, 4, 0);
  lv_obj_set_style_bg_color(s_hpBar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_hpBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_hpBar, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_hpBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_hpBar, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);

  s_stat = make_label(s_scr, "HP 100   WAVE 1   KILLS 0", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(s_stat, 1, 0);
  lv_obj_align(s_stat, LV_ALIGN_TOP_MID, 0, VP_H + 20);

  make_back_chevron(s_scr);

  // ---- Title / game-over card ----
  s_panel = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_panel);
  lv_obj_set_size(s_panel, 284, 172);
  lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_panel, 22, 0);
  lv_obj_set_style_bg_color(s_panel, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_panel, 2, 0);
  lv_obj_set_style_border_color(s_panel, lv_color_hex(COL_ACCENT), 0);

  s_title = make_label(s_panel, "DIAL OF DOOM", &font_sg_23, COL_ACCENT);
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 26);
  s_sub = make_label(s_panel, "TAP TO START", &font_jbm_11, COL_TX);
  lv_obj_set_style_text_letter_space(s_sub, 2, 0);
  lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 66);
  s_hint = make_label(s_panel, "TURN THE DIAL TO AIM\nYOU RUN AND FIRE ON YOUR OWN\nGRAB THE MEDKIT BETWEEN WAVES",
                      &font_sg_12, COL_DIM);
  lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 100);

  s_built = true;
}

// ============================ ui_internal.h entry points ============================
void fps_show(void) {
  if (!s_built) {
    buildPalettes();
    buildTables();
    // ~20 KB of DMA-capable internal RAM: one viewport band, reused NBAND times
    // a frame. A full 360x280 framebuffer would be 200 KB and would have to live
    // in PSRAM, which is both slower to fill and slower to DMA out of.
    s_band = (uint16_t *)appDmaAlloc((uint32_t)VP_W * BAND_H * sizeof(uint16_t));
    build();
    newGame();
    updateHud();
  }
  // Leaving the app abandons the run, so the title card must not sit above a
  // status bar still showing the last game's health.
  s_state = FPS_READY;
  newGame();
  updateHud();
  lv_scr_load(s_scr);
  showPanel(false);
  if (!s_band) lv_label_set_text(s_hint, "OUT OF MEMORY\nNO FRAME BUFFER AVAILABLE");
}

void fps_encoder(int rawDir) {
  if (s_state != FPS_PLAY) return;
  int dir = FPS_INVERT_DIR ? -rawDir : rawDir;
  s_angTarget += (float)dir * TURN_PER_DETENT;
}

void fps_tick(void) {
  if (!s_built || s_state != FPS_PLAY || !s_band) return;

  uint32_t now = millis();
  float dt = (float)(now - s_lastMs) * 0.001f;
  s_lastMs = now;
  if (dt <= 0.0f) return;
  if (dt > 0.10f) dt = 0.10f;  // a long stall must not teleport you into a wall

  appKeepAwake();  // the game runs without input; don't let the idle blank hit

  // Aim chases the dial rather than snapping, so a fast spin still reads as a
  // sweep instead of a jump cut.
  float k = dt * TURN_LERP;
  if (k > 1.0f) k = 1.0f;
  s_ang += wrapPi(s_angTarget - s_ang) * k;
  s_ang = wrapPi(s_ang);
  s_angTarget = wrapPi(s_angTarget);

  // Always forward. Axes resolve separately so walls are slid along, not stuck to.
  float nx = s_px + cosf(s_ang) * MOVE_SPEED * dt;
  float ny = s_py + sinf(s_ang) * MOVE_SPEED * dt;
  if (!boxBlocked(nx, s_py, PLAYER_R)) s_px = nx;
  if (!boxBlocked(s_px, ny, PLAYER_R)) s_py = ny;
  s_bob += dt * 7.5f;
  if (s_bob > 2.0f * (float)M_PI) s_bob -= 2.0f * (float)M_PI;

  int hpWas = s_hp, killsWas = s_kills, waveWas = s_wave;
  bool gapWas = s_gap, packWas = s_packAlive;
  autoFire(now);
  stepEnemies(now, dt);
  if (s_gap) stepGap(now);
  // The countdown ticks with no input behind it, so the status bar also has to
  // repaint on the second, not just when something happens to you.
  if (s_hp != hpWas || s_kills != killsWas || s_wave != waveWas ||
      s_gap != gapWas || s_packAlive != packWas ||
      (s_gap && gapSecs(now) != s_hudSec))
    updateHud();

  renderFrame(now);

  if (s_hp <= 0) die();
}
