// LVGL font declarations. The font data lives in fonts/*.c, generated from
// Space Grotesk / JetBrains Mono by tools/gen_lvgl_fonts.sh (see README).
#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(font_sg_23);   // Space Grotesk 600 23px - app name
LV_FONT_DECLARE(font_sg_14);   // Space Grotesk 600 14px - settings labels
LV_FONT_DECLARE(font_sg_12);   // Space Grotesk 400 12px - description / body
LV_FONT_DECLARE(font_jbm_10);  // JetBrains Mono 400 10px - titles / meta / status
LV_FONT_DECLARE(font_jbm_11);  // JetBrains Mono 500 11px - mode pill
LV_FONT_DECLARE(font_jbm_12);  // JetBrains Mono 400 12px - settings values
LV_FONT_DECLARE(font_jbm_52);  // JetBrains Mono 700 52px - timer MM:SS (digits+colon)
LV_FONT_DECLARE(font_sg_46);   // Space Grotesk 600 46px - timer finale ("TIME'S UP" glyphs only)
