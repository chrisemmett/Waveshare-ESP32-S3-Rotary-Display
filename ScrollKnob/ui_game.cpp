// Safe Cracker game. Rotate the dial to hunt for each tumbler; the ring's inner
// halo strengthens near the target and the detents firm up as you close in.
// Arrive from the required direction and hold still for 1.5 s to capture it.
// Turning the wrong way trips the lock: the dial judders red and the whole game
// resets (the combination stays the same). Five tumblers -> SAFE OPEN. Touch:
// NEW starts a fresh game, MENU returns to the launcher.
//
// Rendering. The outer ring is a real engraved dial face - 50 graduations,
// numerals every 5, brushed metal, a bevelled bezel - and it does NOT go through
// LVGL. It is baked once into a POLAR texture (angle x radius) and then sampled
// per screen pixel through a lookup table, so rotating it is just an offset into
// the angle axis. That is what lets the dial turn smoothly between detents
// instead of snapping, and lets the whole ring change colour (lockout red, vault
// green) by rebuilding a 320-entry palette rather than redrawing anything.
//
// The two renderers split the screen by radius: we own r >= RING_IN, LVGL owns
// the disc inside it (number, tumbler dots, status, NEW, MENU). Bands that
// straddle the top or bottom of that hole are blitted full-width for simplicity,
// which paints background over a sliver of the hole's rim - hence LVGL content
// is kept inside RING_GUARD rather than right up to RING_IN.
#include "ui_internal.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#define GAME_DEBUG 0        // 1 = print the secret combo to Serial
#define GAME_INVERT_DIR 0   // flip if physical clockwise feels reversed
#define N_TUMBLERS 5
#define DIAL_N 50
#define HOLD_MS 1500
#define LOCKOUT_MS 900      // red judder after a wrong-way turn
#define FLASH_MS 450        // gold pulse when a tumbler is captured

// ============================ Dial geometry ============================
// Angular resolution of the polar texture. 24 units per dial number keeps a
// graduation about 2.5 px wide at the tick radius; coarser than this and the
// ticks visibly crawl as the dial turns.
#define NA 1200
#define ASTEP (NA / DIAL_N)   // 24 texture units per number
#define RING_IN 132           // inner edge of the dial face
#define RING_OUT 178          // outer edge (display circle is 179)
#define NR (RING_OUT - RING_IN + 1)   // 47 radial samples
// Two materials per byte. Internal RAM is the scarce resource on this board -
// the FPS holds a blit buffer of its own and NimBLE is always up - so halving
// the texture is worth a shift and a mask in the render loop. 24 is also a
// shift-pair, so indexing stays multiply-free.
#define RBYTES 24             // bytes per angle column, covers NR radii
#define RING_GUARD 112        // LVGL content must stay inside this radius

// Radial zones, as rIdx (0 = RING_IN ... NR-1 = RING_OUT).
#define Z_HALO_R1  6          // 0..6   proximity halo
#define Z_ARC_R1   4          // 0..4   hold-to-capture progress
#define Z_NUM_TOP  19         // 7..19  numerals (DIG_H rows, top = outermost)
#define Z_TICK_MAJ 22         // 22..32 major graduation (every 5)
#define Z_TICK_MIN 27         // 27..32 minor graduation
#define Z_TICK_OUT 32
#define Z_MARK_R0  30         // 30..46 fixed index marker at 12 o'clock
#define Z_DIR_R0   33         // 33..36 "turn this way" arc
#define Z_DIR_R1   36
#define Z_BEZ_R0   37         // 37..44 brushed bezel
#define Z_RIM_R0   45         // 45..46 dark outer rim

// ============================ Viewport banding ============================
#define BAND_H 20
#define NBAND (SCR_W / BAND_H)

// ============================ Numerals ============================
// Hand-drawn so they stay editable, same as the FPS sprite art. Drawn into the
// polar grid directly, which is why they fan slightly wider at the outer edge -
// exactly how a real dial's engraving reads.
#define DIG_W 8
#define DIG_H 13
static const char *const DIG[10][DIG_H] = {
  { " XXXXXX ", "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX",
    "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX", " XXXXXX " },
  { "   XXX  ", "  XXXX  ", " XX XX  ", "    XX  ", "    XX  ", "    XX  ", "    XX  ",
    "    XX  ", "    XX  ", "    XX  ", "    XX  ", "    XX  ", "  XXXXXX" },
  { " XXXXXX ", "XX    XX", "XX    XX", "      XX", "      XX", "     XX ", "    XX  ",
    "   XX   ", "  XX    ", " XX     ", "XX      ", "XX      ", "XXXXXXXX" },
  { " XXXXXX ", "XX    XX", "      XX", "      XX", "     XX ", "  XXXX  ", "     XX ",
    "      XX", "      XX", "      XX", "XX    XX", "XX    XX", " XXXXXX " },
  { "     XX ", "    XXX ", "   XXXX ", "  XX XX ", " XX  XX ", "XX   XX ", "XX   XX ",
    "XXXXXXXX", "XXXXXXXX", "     XX ", "     XX ", "     XX ", "     XX " },
  { "XXXXXXXX", "XX      ", "XX      ", "XX      ", "XXXXXX  ", "XX   XX ", "      XX",
    "      XX", "      XX", "      XX", "XX    XX", "XX    XX", " XXXXXX " },
  { "  XXXXX ", " XX    X", "XX      ", "XX      ", "XX      ", "XXXXXX  ", "XX   XX ",
    "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX", " XXXXXX " },
  { "XXXXXXXX", "XX    XX", "      XX", "     XX ", "     XX ", "    XX  ", "    XX  ",
    "   XX   ", "   XX   ", "  XX    ", "  XX    ", " XX     ", " XX     " },
  { " XXXXXX ", "XX    XX", "XX    XX", "XX    XX", " XXXXXX ", "XX    XX", "XX    XX",
    "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX", " XXXXXX " },
  { " XXXXXX ", "XX    XX", "XX    XX", "XX    XX", "XX    XX", "XX    XX", " XXXXXXX",
    "      XX", "      XX", "      XX", "      XX", " XX  XX ", " XXXXX  " },
};

// ============================ Materials ============================
// The polar texture stores a material index, never a colour - so the whole dial
// can be re-tinted by rebuilding the palette instead of rebuilding the texture.
enum {
  M_BG = 0, M_FACE, M_FACE_L, M_FACE_D, M_TICK, M_TICKM, M_NUM, M_BEZEL, M_BEZEL_L, M_RIM,
  N_MAT
};
#define MSTRIDE 16   // palette row stride, so PAL[lit] is a plain pointer

// Fully-lit colours; the per-pixel light level scales them down from here.
static const uint32_t MAT_BASE[N_MAT] = {
  COL_BG,      // M_BG      outside the ring / inside the hole
  0x2a2724,    // M_FACE    dial face
  0x343029,    // M_FACE_L  brushed streak, lighter
  0x211f1d,    // M_FACE_D  brushed streak, darker
  0xc9c1b3,    // M_TICK    minor graduation
  0xf2ebdc,    // M_TICKM   major graduation
  0xece5d6,    // M_NUM     numerals
  0x8b8279,    // M_BEZEL   machined bezel
  0xa79d92,    // M_BEZEL_L bezel brush highlight
  0x111011,    // M_RIM     dark outer rim
};

// 32 shades rather than 16: once the ring takes a strong tint the brushed
// streaks stop hiding the quantisation, and the light contours show up as
// facets. The map build also dithers the level, which kills the rest of it.
#define NLIGHT 32

// ============================ State ============================
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_num = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_dots[N_TUMBLERS];
static lv_obj_t *s_openHit = nullptr;   // "tap to play again" (when open)

static int s_secret[N_TUMBLERS];
static int s_tumbler = 0;
static int s_pos = 0;
static int s_lastDir = 0;               // +1 = CW, -1 = ACW, 0 = none yet
static bool s_holdActive = false;
static uint32_t s_holdStartMs = 0;
static bool s_open = false;
static uint32_t s_flashUntil = 0;
static uint32_t s_lockoutUntil = 0;
static bool s_built = false;

// Dial animation. The face chases the detent on a spring rather than snapping,
// so a fast spin reads as a sweep and a single click lands with a little settle.
#define SPRING 420.0f
#define DAMP 26.0f
static float s_rotCur = 0.0f, s_rotVel = 0.0f;
static bool s_freeSpin = false;         // SAFE OPEN: let it coast to a stop
static uint32_t s_lastTickMs = 0;

// ============================ Render tables ============================
static bool s_ringOK = false;
static uint8_t *s_pt = nullptr;         // polar texture, 4-bit materials
static uint32_t *s_map = nullptr;       // per ring pixel: screenA | rIdx | light
static uint16_t *s_band = nullptr;      // one blit chunk, DMA-capable

struct RowSpan { int16_t a0, a1, b0, b1; uint32_t off; };
static RowSpan s_row[SCR_W];
struct BandChunks { uint8_t n; int16_t x0[2], x1[2]; };
static BandChunks s_bandc[NBAND];

static uint16_t s_pal[NLIGHT][MSTRIDE];   // normal
static uint16_t s_palG[NLIGHT][MSTRIDE];  // tinted toward the halo colour

// Per-radius overlay strengths, recomputed only when the game state moves.
static uint8_t s_haloT[NR];   // ordered-dither threshold, 0..16
static uint8_t s_markHW[NR];  // index marker half-width in angle units, 0 = none
static int s_arcTo = 0;       // hold progress, in screen angle units
static uint16_t s_arcCol, s_markCol, s_dirCol;
static int s_dirFrom = 0, s_dirTo = 0;    // "turn this way" arc, screen angle units

static int s_rotOff = 0;      // texture angle under the 12 o'clock index
static uint32_t s_palGen = 0;
static int s_lastRot = -1, s_lastArc = -1, s_lastDirFrom = -1;
static uint32_t s_lastPalGen = 0xffffffffu;
static int s_forceFrames = 0;

// Tint currently applied to the whole ring (lockout red / vault green / capture
// gold), and the halo colour + strength driven by how close the dial is.
static uint32_t s_tintRGB = 0;
static int s_tintAmt = 0;     // 0..256
static uint32_t s_haloRGB = COL_FAINT;
static uint32_t s_palTint = 1;  // last-built (tint, halo) signature

static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

// ============================ Colour helpers ============================
// The panel wants big-endian RGB565, the same order LVGL's flush hands over.
static inline uint16_t be565(int r, int g, int b) {
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}
// `s` is a 0..256 brightness scale applied to a 0xRRGGBB literal.
static inline uint16_t shade(uint32_t rgb, int s) {
  return be565((int)((((rgb >> 16) & 0xFF) * s) >> 8),
               (int)((((rgb >> 8) & 0xFF) * s) >> 8),
               (int)(((rgb & 0xFF) * s) >> 8));
}
static inline uint32_t lerpRGB(uint32_t a, uint32_t b, int t) {  // t 0..256
  int r = (int)((a >> 16) & 0xFF) + ((((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t) >> 8);
  int g = (int)((a >> 8) & 0xFF) + ((((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t) >> 8);
  int bl = (int)(a & 0xFF) + ((((int)(b & 0xFF) - (int)(a & 0xFF)) * t) >> 8);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

// ============================ Palettes ============================
// Two rows per light level: the plain dial, and the same dial pulled toward the
// halo colour. Which one a pixel uses is an ordered-dither compare, so the halo
// costs a threshold test rather than an alpha blend.
static void buildPalettes(void) {
  for (int m = 0; m < N_MAT; m++) {
    uint32_t base = MAT_BASE[m];
    if (s_tintAmt > 0) base = lerpRGB(base, s_tintRGB, s_tintAmt);
    uint32_t glow = lerpRGB(base, s_haloRGB, 150);
    for (int l = 0; l < NLIGHT; l++) {
      int s = 40 + l * 216 / (NLIGHT - 1);
      if (s > 256) s = 256;
      s_pal[l][m] = shade(base, s);
      s_palG[l][m] = shade(glow, s);
    }
  }
  // M_BG is the LVGL screen background and must match it exactly, or the seam
  // where our chunks meet LVGL's fill would show as a faint ring.
  for (int l = 0; l < NLIGHT; l++) {
    s_pal[l][M_BG] = shade(COL_BG, 256);
    s_palG[l][M_BG] = s_pal[l][M_BG];
  }
  // Bright cream, not green: the halo is already green once you are on the
  // number, and a progress sweep you cannot pick out of it is no progress bar.
  s_arcCol = shade(COL_TX, 256);
  s_markCol = shade(COL_ACCENT, 256);
  s_dirCol = shade(COL_ACCENT, 130);
  s_palGen++;
}

// ============================ Polar texture ============================
static inline int ptOff(int a) { return (a << 4) + (a << 3); }  // a * RBYTES

static inline void ptSet(int a, int r, uint8_t mat) {
  if (r < 0 || r >= NR) return;
  while (a < 0) a += NA;
  while (a >= NA) a -= NA;
  uint8_t *b = &s_pt[ptOff(a) + (r >> 1)];
  *b = (r & 1) ? (uint8_t)((*b & 0x0F) | (mat << 4)) : (uint8_t)((*b & 0xF0) | mat);
}

static void buildTexture(void) {
  // Face + bezel + rim, with a per-angle brushed streak. The streak lives in the
  // texture (it is on the metal, so it turns with the dial); the bevel shading
  // does not, because it is rotationally symmetric and comes from the light LUT.
  for (int a = 0; a < NA; a++) {
    uint32_t h = (uint32_t)a * 2654435761u;
    h ^= h >> 13;
    uint8_t face = (h & 3) == 0 ? M_FACE_L : ((h & 3) == 1 ? M_FACE_D : M_FACE);
    uint8_t bez = ((h >> 5) & 3) == 0 ? M_BEZEL_L : M_BEZEL;
    for (int r = 0; r < NR; r++) {
      uint8_t m = (r >= Z_RIM_R0) ? (uint8_t)M_RIM : (r >= Z_BEZ_R0 ? bez : face);
      ptSet(a, r, m);
    }
  }

  // Graduations. Number n is engraved at -n*ASTEP so that numbers increase
  // anticlockwise around the face - which is what makes a clockwise turn of the
  // knob rotate the face clockwise, the way a real dial behaves.
  for (int n = 0; n < DIAL_N; n++) {
    int c = (NA - n * ASTEP) % NA;
    bool major = (n % 5) == 0;
    int hw = major ? 2 : 1;
    int r0 = major ? Z_TICK_MAJ : Z_TICK_MIN;
    for (int du = -hw; du <= hw; du++)
      for (int r = r0; r <= Z_TICK_OUT; r++) ptSet(c + du, r, major ? M_TICKM : M_TICK);
  }

  // Numerals every 5. Iterating over destination angle units (rather than source
  // pixels) is what keeps the glyph solid: units are finer than pixels here, so
  // no column of the glyph can fall between two units and vanish.
  const float rMid = (float)(RING_IN + Z_NUM_TOP - DIG_H / 2);
  const float upp = (float)NA / (2.0f * (float)M_PI * rMid);  // angle units per pixel
  for (int n = 0; n < DIAL_N; n += 5) {
    int d1 = n / 10, d0 = n % 10;
    int nd = d1 ? 2 : 1;
    int w = nd * DIG_W + (nd - 1);            // 1 px between digits
    int c = (NA - n * ASTEP) % NA;
    int halfU = (int)(w * 0.5f * upp) + 1;
    for (int du = -halfU; du <= halfU; du++) {
      int gx = (int)floorf((float)du / upp + w * 0.5f);
      if (gx < 0 || gx >= w) continue;
      const char *const *glyph;
      int col;
      if (nd == 1) { glyph = DIG[d0]; col = gx; }
      else if (gx < DIG_W) { glyph = DIG[d1]; col = gx; }
      else if (gx == DIG_W) { continue; }     // inter-digit gap
      else { glyph = DIG[d0]; col = gx - DIG_W - 1; }
      if (col < 0 || col >= DIG_W) continue;
      for (int gy = 0; gy < DIG_H; gy++)
        if (glyph[gy][col] != ' ') ptSet(c + du, Z_NUM_TOP - gy, M_NUM);
    }
  }
}

// ============================ Pixel map ============================
// One entry per ring pixel: the screen angle it sits at (used both to index the
// texture and to place the screen-fixed overlays), its radius index, and its
// baked light level. Packed into a single word so the inner loop streams it.
static bool buildMap(void) {
  const int rin2 = RING_IN * RING_IN, rout2 = RING_OUT * RING_OUT;

  uint32_t n = 0;
  for (int y = 0; y < SCR_W; y++) {
    int dy = y - SCR_CY;
    for (int x = 0; x < SCR_W; x++) {
      int dx = x - SCR_CX;
      int r2 = dx * dx + dy * dy;
      if (r2 >= rin2 && r2 <= rout2) n++;
    }
  }
  s_map = (uint32_t *)appBulkAlloc(n * sizeof(uint32_t));
  if (!s_map) return false;

  // Radial brightness profile: a rounded bevel across the bezel, a near-flat
  // face, and a small catch-light on the inner lip.
  float prof[NR];
  for (int r = 0; r < NR; r++) {
    if (r >= Z_RIM_R0) prof[r] = 60.0f;
    else if (r >= Z_BEZ_R0) {
      float t = (float)(r - Z_BEZ_R0) / (float)(Z_RIM_R0 - Z_BEZ_R0);
      prof[r] = 120.0f + 135.0f * sinf((float)M_PI * t);
    } else if (r <= 2) prof[r] = 235.0f;
    else prof[r] = 185.0f + 15.0f * (float)r / (float)Z_BEZ_R0;
  }

  uint32_t off = 0;
  for (int y = 0; y < SCR_W; y++) {
    int dy = y - SCR_CY;
    RowSpan *rs = &s_row[y];
    rs->a0 = 1; rs->a1 = 0; rs->b0 = 1; rs->b1 = 0;
    rs->off = off;
    int span = 0, runStart = -1;
    for (int x = 0; x <= SCR_W; x++) {
      int dx = x - SCR_CX;
      bool ring = false;
      if (x < SCR_W) {
        int r2 = dx * dx + dy * dy;
        ring = (r2 >= rin2 && r2 <= rout2);
      }
      if (ring && runStart < 0) runStart = x;
      if (!ring && runStart >= 0) {
        if (span == 0) { rs->a0 = (int16_t)runStart; rs->a1 = (int16_t)(x - 1); }
        else if (span == 1) { rs->b0 = (int16_t)runStart; rs->b1 = (int16_t)(x - 1); }
        span++;
        runStart = -1;
      }
      if (!ring || x >= SCR_W) continue;
      float fr = sqrtf((float)(dx * dx + dy * dy));
      int rI = (int)(fr + 0.5f) - RING_IN;
      if (rI < 0) rI = 0; else if (rI >= NR) rI = NR - 1;
      // Screen angle, 0 at 12 o'clock and increasing clockwise.
      float ang = atan2f((float)dx, (float)(-dy));
      if (ang < 0.0f) ang += 2.0f * (float)M_PI;
      int sA = (int)(ang * (float)NA / (2.0f * (float)M_PI) + 0.5f);
      if (sA >= NA) sA -= NA;
      // Lit from straight above: the engraving turns, the highlight does not.
      // The level is ordered-dithered as it is quantised, so the bevel reads as
      // a smooth gradient instead of concentric steps - and since the dither is
      // baked in here it costs the render loop nothing.
      float dir = 0.72f + 0.28f * ((float)(-dy) / (fr > 1.0f ? fr : 1.0f));
      int v = (int)(prof[rI] * dir) + BAYER[((y & 3) << 2) | (x & 3)] / 2;
      int lit = v * NLIGHT / 256;
      if (lit < 0) lit = 0; else if (lit >= NLIGHT) lit = NLIGHT - 1;
      s_map[off++] = (uint32_t)sA | ((uint32_t)rI << 16) | ((uint32_t)lit << 24);
    }
  }

  // Per band, the rectangles we have to push. Where every row of the band has a
  // hole we can blit the two sides separately and leave LVGL's disc untouched;
  // where the hole opens or closes mid-band there is no common gap, so the band
  // goes out full-width and paints background over the hole's rim.
  for (int b = 0; b < NBAND; b++) {
    int y0 = b * BAND_H, y1 = y0 + BAND_H - 1;
    int bx0 = SCR_W, bx1 = -1, hl = -1, hr = SCR_W;
    bool any = false, hole = true;
    for (int y = y0; y <= y1; y++) {
      RowSpan *rs = &s_row[y];
      if (rs->a0 > rs->a1) continue;  // no ring at all on this row
      any = true;
      if (rs->a0 < bx0) bx0 = rs->a0;
      if (rs->b0 <= rs->b1) {
        if (rs->b1 > bx1) bx1 = rs->b1;
        if (rs->a1 + 1 > hl) hl = rs->a1 + 1;
        if (rs->b0 - 1 < hr) hr = rs->b0 - 1;
      } else {
        if (rs->a1 > bx1) bx1 = rs->a1;
        hole = false;
      }
    }
    BandChunks *bc = &s_bandc[b];
    bc->n = 0;
    if (!any) continue;
    if (hole && hl <= hr) {
      bc->x0[0] = (int16_t)bx0; bc->x1[0] = (int16_t)(hl - 1);
      bc->x0[1] = (int16_t)(hr + 1); bc->x1[1] = (int16_t)bx1;
      bc->n = 2;
    } else {
      bc->x0[0] = (int16_t)bx0; bc->x1[0] = (int16_t)bx1;
      bc->n = 1;
    }
  }
  return true;
}

static bool ringInit(void) {
  // The texture is read once per pixel with a scattered index, so it wants
  // internal RAM; PSRAM is the fallback rather than the first choice.
  s_pt = (uint8_t *)appDmaAlloc(NA * RBYTES);
  if (!s_pt) s_pt = (uint8_t *)appBulkAlloc(NA * RBYTES);
  if (!s_pt) return false;
  s_band = (uint16_t *)appDmaAlloc((uint32_t)SCR_W * BAND_H * sizeof(uint16_t));
  if (!s_band) return false;
  if (!buildMap()) return false;
  buildTexture();
  buildPalettes();
  // The index marker reaches down past the graduations and widens toward the
  // rim, so it reads as a pointer aimed at the number under it. Fixed shape, so
  // it is a table rather than something the frame recomputes.
  for (int r = 0; r < NR; r++)
    s_markHW[r] = (r >= Z_MARK_R0)
                      ? (uint8_t)(2 + (r - Z_MARK_R0) * 3 / (NR - 1 - Z_MARK_R0))
                      : 0;
  return true;
}

// ============================ Ring render ============================
static inline void renderSpan(int x0, int x1, uint32_t off, int cx0, int cx1,
                              uint16_t *dst, int y) {
  if (x0 > x1) return;
  int s = x0 > cx0 ? x0 : cx0;
  int e = x1 < cx1 ? x1 : cx1;
  if (s > e) return;
  const uint32_t *m = s_map + off + (s - x0);
  const uint8_t *bay = &BAYER[(y & 3) << 2];
  for (int x = s; x <= e; x++) {
    uint32_t p = *m++;
    int sA = (int)(p & 0xFFFF);
    int rI = (int)((p >> 16) & 0xFF);
    int lit = (int)(p >> 24);

    int tA = sA + s_rotOff;
    if (tA >= NA) tA -= NA;
    uint8_t pk = s_pt[ptOff(tA) + (rI >> 1)];
    uint8_t mat = (rI & 1) ? (uint8_t)(pk >> 4) : (uint8_t)(pk & 0x0F);

    uint8_t g = s_haloT[rI];
    uint16_t col = (g && bay[x & 3] < g) ? s_palG[lit][mat] : s_pal[lit][mat];

    // Screen-fixed overlays. sA is the angle on the panel, not on the dial, so
    // these stay put while the engraving turns underneath them.
    uint8_t hw = s_markHW[rI];
    bool onMark = false;
    if (hw) {
      int d = sA < NA / 2 ? sA : sA - NA;
      if (d < 0) d = -d;
      onMark = d <= hw;
    }
    if (onMark) {
      col = s_markCol;
    } else if (rI >= Z_DIR_R0 && rI <= Z_DIR_R1 && s_dirFrom != s_dirTo) {
      bool in = (s_dirFrom < s_dirTo) ? (sA >= s_dirFrom && sA <= s_dirTo)
                                      : (sA >= s_dirFrom || sA <= s_dirTo);
      if (in) col = s_dirCol;
    } else if (rI <= Z_ARC_R1 && sA < s_arcTo) {
      col = s_arcCol;
    }
    dst[x - cx0] = col;
  }
}

static void ringRender(void) {
  if (!s_ringOK) return;
  uint16_t bg = s_pal[0][M_BG];
  for (int b = 0; b < NBAND; b++) {
    BandChunks *bc = &s_bandc[b];
    int y0 = b * BAND_H;
    for (int c = 0; c < bc->n; c++) {
      int cx0 = bc->x0[c], cx1 = bc->x1[c];
      int cw = cx1 - cx0 + 1;
      if (cw <= 0) continue;
      int npx = cw * BAND_H;
      for (int i = 0; i < npx; i++) s_band[i] = bg;
      for (int y = y0; y < y0 + BAND_H; y++) {
        RowSpan *rs = &s_row[y];
        uint16_t *dst = s_band + (y - y0) * cw;
        renderSpan(rs->a0, rs->a1, rs->off, cx0, cx1, dst, y);
        renderSpan(rs->b0, rs->b1, rs->off + (uint32_t)(rs->a1 - rs->a0 + 1),
                   cx0, cx1, dst, y);
      }
      appBlit((int16_t)cx0, (int16_t)y0, (int16_t)cw, BAND_H, s_band);
    }
  }
}

// ============================ Helpers ============================
static bool lockoutActive(void) { return s_lockoutUntil != 0; }

static int circDist(int a, int b) {
  int d = a - b;
  if (d < 0) d = -d;
  return d < DIAL_N - d ? d : DIAL_N - d;
}
static bool requiredCW(void) { return (s_tumbler % 2) == 0; }
static bool dirCorrect(int mv) { return mv != 0 && ((mv > 0) == requiredCW()); }

// Distance to the tumbler -> halo colour and strength.
static void signalFor(int dist, uint32_t *col, int *amt) {
  if (dist == 0)       { *col = COL_GREEN;  *amt = 16; }
  else if (dist <= 2)  { *col = COL_ACCENT; *amt = 13; }
  else if (dist <= 5)  { *col = COL_ACCENT; *amt = 9; }
  else if (dist <= 9)  { *col = COL_ACCENT; *amt = 5; }
  else if (dist <= 14) { *col = COL_FAINT;  *amt = 3; }
  else                 { *col = COL_FAINT;  *amt = 1; }
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

static void redraw(void) {
  bool alarm = lockoutActive();

  char b[4];
  snprintf(b, sizeof(b), "%d", s_pos);
  lv_label_set_text(s_num, b);
  uint32_t numCol = alarm ? COL_RED : (s_flashUntil ? COL_GREEN : COL_TX);
  lv_obj_set_style_text_color(s_num, lv_color_hex(numCol), 0);

  updateDots();

  int dist = s_open ? 99 : circDist(s_pos, s_secret[s_tumbler]);

  const char *st;
  uint32_t sc;
  if (alarm) {
    st = "LOCKOUT"; sc = COL_RED;
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

static void cancelHold(void) { s_holdActive = false; }
static void startHold(void) { s_holdActive = true; s_holdStartMs = millis(); }

static void openSafe(void) {
  s_open = true;
  cancelHold();
  appHapticAlarm();
  s_flashUntil = 0;
  s_freeSpin = true;
  s_rotVel = 26.0f;   // half a turn a second, coasting down
  lv_obj_clear_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_openHit);
  redraw();
}

static void capture(void) {
  s_tumbler++;
  cancelHold();
  appHapticPress();
  s_rotVel += 5.0f;               // the tumbler drops, and the dial feels it
  s_flashUntil = millis() + FLASH_MS;
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
  s_freeSpin = false;
  cancelHold();
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
}

// Turning the wrong way trips the lock: alarm buzz, the dial judders red, and
// the game resets to the first tumbler. The combination is unchanged.
static void wrongDirReset(void) {
  appHapticAlarm();
  resetProgress();
  s_lockoutUntil = millis() + LOCKOUT_MS;
  redraw();
}

static void newGame(void) {
  for (int i = 0; i < N_TUMBLERS; i++) s_secret[i] = appRandom() % DIAL_N;
  resetProgress();
  s_lockoutUntil = 0;
  s_rotCur = 0.0f;
  s_rotVel = 0.0f;
#if GAME_DEBUG
  Serial.printf("Safe combo: %d %d %d %d %d\n", s_secret[0], s_secret[1],
                s_secret[2], s_secret[3], s_secret[4]);
#endif
  redraw();
}

// ---- touch callbacks ----
static void new_click_cb(lv_event_t *e) { (void)e; appHapticPress(); newGame(); }
static void open_click_cb(lv_event_t *e) { (void)e; appHapticPress(); newGame(); }

// ============================ LVGL chrome ============================
// Everything here has to fit inside RING_GUARD - see the note at the top of the
// file. The widest thing is the status line, and at its offset it clears it.
static void build(void) {
  s_scr = make_screen_base();
  // Without the ring there is no rendered index at 12 o'clock, so fall back to
  // the shared marker triangle rather than leaving the dial unlabelled.
  if (!s_ringOK) make_marker(s_scr);

  lv_obj_t *nb = make_label(s_scr, "NEW", &font_jbm_10, COL_ACCENT);
  lv_obj_set_style_text_letter_space(nb, 2, 0);
  lv_obj_align(nb, LV_ALIGN_CENTER, 0, -88);
  lv_obj_add_flag(nb, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(nb, 24);
  lv_obj_add_event_cb(nb, new_click_cb, LV_EVENT_CLICKED, NULL);

  s_num = lv_label_create(s_scr);
  lv_obj_set_style_text_font(s_num, &font_jbm_52, 0);
  lv_obj_set_style_text_color(s_num, lv_color_hex(COL_TX), 0);
  lv_label_set_text(s_num, "0");
  lv_obj_align(s_num, LV_ALIGN_CENTER, 0, -28);

  for (int i = 0; i < N_TUMBLERS; i++) {
    lv_obj_t *d = lv_obj_create(s_scr);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 11, 11);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(d, LV_ALIGN_CENTER, (i - 2) * 22, 18);
    s_dots[i] = d;
  }

  s_status = make_label(s_scr, "TURN CLOCKWISE", &font_jbm_10, COL_DIM);
  lv_obj_set_style_text_letter_space(s_status, 2, 0);
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 38);

  lv_obj_t *back = make_back_chevron(s_scr);
  lv_obj_align(back, LV_ALIGN_CENTER, 0, 70);  // inside the dial, not under it

  // "Tap to play again" target, only active on SAFE OPEN. A centre circle (not
  // full-screen) so the MENU chevron and NEW label stay tappable - and small
  // enough that invalidating it never repaints over the ring.
  s_openHit = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_openHit);
  lv_obj_set_size(s_openHit, 200, 200);
  lv_obj_center(s_openHit);
  lv_obj_set_style_radius(s_openHit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_openHit, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(s_openHit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_openHit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_openHit, open_click_cb, LV_EVENT_CLICKED, NULL);
}

// ============================ Per-frame ring state ============================
static void updateRingState(uint32_t now) {
  // Tint: the whole ring goes red under lockout, green once the safe is open,
  // and takes a short gold pulse each time a tumbler drops.
  uint32_t tint = 0;
  int amt = 0;
  if (lockoutActive()) {
    int left = (int)(s_lockoutUntil - now);
    if (left < 0) left = 0;
    tint = COL_RED;
    amt = 70 + left * 100 / LOCKOUT_MS;
  } else if (s_open) {
    tint = COL_GREEN;
    amt = 95;
  } else if (s_flashUntil) {
    int left = (int)(s_flashUntil - now);
    if (left < 0) left = 0;
    tint = COL_ACCENT;
    amt = left * 120 / FLASH_MS;
  }

  uint32_t halo = COL_FAINT;
  int hamt = 0;
  if (!s_open && !lockoutActive()) signalFor(circDist(s_pos, s_secret[s_tumbler]), &halo, &hamt);
  else if (s_open) { halo = COL_GREEN; hamt = 16; }
  else { halo = COL_RED; hamt = 16; }

  // Quantise before comparing: the tint decays continuously, and rebuilding the
  // palette on every single step of that would be pure churn.
  amt = (amt / 8) * 8;
  uint32_t sig = (tint & 0xffffffu) | ((uint32_t)amt << 24);
  sig ^= (halo << 3) + (uint32_t)hamt;
  if (sig != s_palTint) {
    s_palTint = sig;
    s_tintRGB = tint; s_tintAmt = amt;
    s_haloRGB = halo;
    buildPalettes();
    static const uint8_t PROF[Z_HALO_R1 + 1] = { 16, 15, 13, 10, 7, 4, 2 };
    memset(s_haloT, 0, sizeof(s_haloT));
    for (int r = 0; r <= Z_HALO_R1; r++) s_haloT[r] = (uint8_t)(PROF[r] * hamt / 16);
  }

  // Hold-to-capture sweeps the inner lip clockwise from the index.
  s_arcTo = 0;
  if (s_holdActive) {
    uint32_t el = now - s_holdStartMs;
    if (el > HOLD_MS) el = HOLD_MS;
    s_arcTo = (int)(el * NA / HOLD_MS);
  }

  // "Turn this way": a short arc leaving the index in the required direction.
  if (s_open || lockoutActive()) { s_dirFrom = s_dirTo = 0; }
  else if (requiredCW()) { s_dirFrom = 8; s_dirTo = 98; }
  else { s_dirFrom = NA - 98; s_dirTo = NA - 8; }

  // Dial rotation. A spring while you are playing, a free coast once it is open.
  float dt = (float)(now - s_lastTickMs) * 0.001f;
  s_lastTickMs = now;
  if (dt <= 0.0f) dt = 0.001f;
  if (dt > 0.03f) dt = 0.03f;  // a stall must not blow up the spring

  if (s_freeSpin) {
    s_rotVel -= s_rotVel * 1.1f * dt;
    s_rotCur += s_rotVel * dt;
    if (fabsf(s_rotVel) < 0.4f) { s_freeSpin = false; s_rotVel = 0.0f; }
  } else {
    float d = (float)s_pos - s_rotCur;
    while (d > DIAL_N * 0.5f) d -= DIAL_N;
    while (d < -DIAL_N * 0.5f) d += DIAL_N;
    s_rotVel += (d * SPRING - s_rotVel * DAMP) * dt;
    s_rotCur += s_rotVel * dt;
  }
  while (s_rotCur < 0.0f) s_rotCur += DIAL_N;
  while (s_rotCur >= DIAL_N) s_rotCur -= DIAL_N;

  // Lockout judder: the face fights you rather than the screen shaking.
  float eff = s_rotCur;
  if (lockoutActive()) {
    float k = (float)(s_lockoutUntil - now) / (float)LOCKOUT_MS;
    if (k < 0.0f) k = 0.0f;
    eff += sinf((float)now * 0.06f) * 1.1f * k * k;
  }

  int off = (int)lroundf(-eff * (float)ASTEP) % NA;
  if (off < 0) off += NA;
  s_rotOff = off;
}

// ============================ Entry points ============================
void game_show(void) {
  if (!s_built) {
    s_ringOK = ringInit();
    build();
    s_lastTickMs = millis();
    newGame();
    s_built = true;
  }
  lv_scr_load(s_scr);
  // lv_scr_load only queues the repaint; LVGL paints the new screen (background
  // and all) on the next lv_timer_handler, which would wipe a ring drawn now.
  // Redraw for a few frames so ours is the one left standing.
  s_forceFrames = 4;
  s_lastTickMs = millis();
}

void game_encoder(int rawDir) {
  if (!s_built || s_open || lockoutActive()) return;
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

  // Lockout expiry -> back to normal colours
  if (lockoutActive() && now >= s_lockoutUntil) {
    s_lockoutUntil = 0;
    redraw();
  }

  if (s_flashUntil && now >= s_flashUntil) {
    s_flashUntil = 0;
    if (!s_open && !lockoutActive())
      lv_obj_set_style_text_color(s_num, lv_color_hex(COL_TX), 0);
  }

  if (s_holdActive) {
    uint32_t el = now - s_holdStartMs;
    if (el >= HOLD_MS) { capture(); return; }
  }

  // The readout coasts with the face while the safe swings open.
  if (s_freeSpin) {
    char b[4];
    int n = ((int)lroundf(s_rotCur) % DIAL_N + DIAL_N) % DIAL_N;
    snprintf(b, sizeof(b), "%d", n);
    lv_label_set_text(s_num, b);
  }

  if (!s_ringOK) return;
  updateRingState(now);

  // Only push the ring when something about it actually moved.
  bool dirty = s_forceFrames > 0 || s_rotOff != s_lastRot || s_arcTo != s_lastArc ||
               s_dirFrom != s_lastDirFrom || s_palGen != s_lastPalGen;
  if (!dirty) return;
  if (s_forceFrames > 0) s_forceFrames--;
  s_lastRot = s_rotOff;
  s_lastArc = s_arcTo;
  s_lastDirFrom = s_dirFrom;
  s_lastPalGen = s_palGen;
  ringRender();
}
