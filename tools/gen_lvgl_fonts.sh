#!/usr/bin/env bash
# Regenerate the LVGL C fonts in ScrollKnob/fonts/ from Space Grotesk and
# JetBrains Mono. Requires Node.js. Fetches the TTF/WOFF from the @fontsource
# npm packages (no Google Fonts download needed) and runs lv_font_conv.
#
# Usage:  tools/gen_lvgl_fonts.sh
#
# The generated symbols (font_sg_23, font_jbm_52, ...) are declared in
# ScrollKnob/fonts_knob.h. If you change sizes/glyph ranges here, update that
# header and the widgets that reference them.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="$ROOT/ScrollKnob/fonts"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
npm init -y >/dev/null 2>&1
echo "Installing fonts + lv_font_conv..."
npm install @fontsource/space-grotesk @fontsource/jetbrains-mono lv_font_conv >/dev/null 2>&1

SG="node_modules/@fontsource/space-grotesk/files"
JBM="node_modules/@fontsource/jetbrains-mono/files"
BIN="node_modules/.bin/lv_font_conv"
ASCII="0x20-0x7E,0xB7"   # printable ASCII + middot (·)

gen() { # font size range outname
  "$BIN" --font "$1" --size "$2" --bpp 4 --format lvgl --range "$3" \
         --no-compress --force-fast-kern-format -o "$OUT/$4.c"
  echo "  wrote $4.c"
}

mkdir -p "$OUT"
echo "Generating LVGL fonts into $OUT ..."
gen "$SG/space-grotesk-latin-600-normal.woff"   23 "$ASCII"     font_sg_23
gen "$SG/space-grotesk-latin-600-normal.woff"   14 "$ASCII"     font_sg_14
gen "$SG/space-grotesk-latin-400-normal.woff"   12 "$ASCII"     font_sg_12
gen "$JBM/jetbrains-mono-latin-400-normal.woff" 10 "$ASCII"     font_jbm_10
gen "$JBM/jetbrains-mono-latin-500-normal.woff" 11 "$ASCII"     font_jbm_11
gen "$JBM/jetbrains-mono-latin-400-normal.woff" 12 "$ASCII"     font_jbm_12
gen "$JBM/jetbrains-mono-latin-700-normal.woff" 52 "0x30-0x3A"  font_jbm_52
echo "Done."
