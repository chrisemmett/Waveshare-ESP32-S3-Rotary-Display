#!/usr/bin/env python3
"""Author + preview the running-man run cycle, then emit sprites.h.

Frames are hand-drawn as ASCII art (16x16). Legend:
  '.' transparent   'R' red (cap/shirt)   'S' skin   'B' brown (hair/overalls/shoes)
Run `python3 sprite_author.py preview`  -> writes preview.png (scaled, black bg)
Run `python3 sprite_author.py emit`     -> prints sprites.h to stdout
"""
import sys

# RGB (we convert to RGB565 in the header). Index order matters: 0 = transparent.
PALETTE = {
    '.': None,               # transparent
    'R': (208, 48, 32),      # red  - cap & shirt
    'S': (248, 184, 136),    # skin - face & hands
    'B': (128, 72, 16),      # brown- hair, overalls, shoes
}
IDX = {'.': 0, 'R': 1, 'S': 2, 'B': 3}

W, H = 16, 16

# Facing RIGHT. Three-frame run cycle: contact / passing / contact(opposite).
# Shared head+torso (rows 0-7); arms (rows 8-10) and legs (rows 11-15) vary.
FRAME_A = [  # front (right) arm forward-high, right leg forward + left leg trailing back
    "................",
    ".....RRRR.......",
    "....RRRRRRRR....",
    "...BBBSSSSSS....",
    "...BBSSBSSSSS...",
    "...BSSSBBSSS....",
    "....SSSSSSS.....",
    ".....SSSSS......",
    "....RRRRRRSS....",
    "...SRRRRRRRS....",
    "...SRRRRRR......",
    "....BBBBBB......",
    "....BB..BB......",
    "...BB....BB.....",
    "..BB.....BB.....",
    ".BBB.....BBB....",
]
FRAME_B = [  # arms tucked, legs passing under body (one knee up)
    "................",
    ".....RRRR.......",
    "....RRRRRRRR....",
    "...BBBSSSSSS....",
    "...BBSSBSSSSS...",
    "...BSSSBBSSS....",
    "....SSSSSSS.....",
    ".....SSSSS......",
    "....RRRRRRR.....",
    "...SRRRRRRRS....",
    "....RRRRRR......",
    "....BBBBBB......",
    "....BBBBB.......",
    ".....BBBBB......",
    ".....BB.BB......",
    "....BBB..BBB....",
]
FRAME_C = [  # front (right) arm back-low, left leg forward + right leg trailing back
    "................",
    ".....RRRR.......",
    "....RRRRRRRR....",
    "...BBBSSSSSS....",
    "...BBSSBSSSSS...",
    "...BSSSBBSSS....",
    "....SSSSSSS.....",
    ".....SSSSS......",
    "..SSRRRRRR......",
    "...SRRRRRRRS....",
    "......RRRRRS....",
    "....BBBBBB......",
    "....BB..BB......",
    ".....BB....BB...",
    ".....BB.....BB..",
    "....BBB....BBB..",
]

FRAMES = [FRAME_A, FRAME_B, FRAME_C]      # distinct bitmaps
SEQUENCE = [0, 1, 2, 1]                    # playback order: stride, pass, stride, pass


def check():
    for n, f in enumerate(FRAMES):
        assert len(f) == H, f"frame {n} has {len(f)} rows"
        for r, row in enumerate(f):
            assert len(row) == W, f"frame {n} row {r} has {len(row)} cols: {row!r}"
            for ch in row:
                assert ch in IDX, f"frame {n} row {r} bad char {ch!r}"


def preview(path, scale=14, bg=(0, 0, 0)):
    from PIL import Image
    gap = 8
    seq = [FRAMES[i] for i in SEQUENCE]
    img = Image.new("RGB", (W * scale * len(seq) + gap * (len(seq) + 1),
                            H * scale + 2 * gap), bg)
    px = img.load()
    for fi, f in enumerate(seq):
        ox = gap + fi * (W * scale + gap)
        oy = gap
        for r in range(H):
            for c in range(W):
                col = PALETTE[f[r][c]]
                if col is None:
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        px[ox + c * scale + dx, oy + r * scale + dy] = col
    img.save(path)
    print("wrote", path)


def rgb565(rgb):
    r, g, b = rgb
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def emit():
    lines = []
    lines.append("// AUTO-GENERATED run-cycle sprite (see tools/sprite_author.py).")
    lines.append("// Small cartoon runner, 16x16, indexed palette. Index 0 = transparent.")
    lines.append("#pragma once")
    lines.append("#include <Arduino_GFX_Library.h>")
    lines.append("")
    lines.append("extern Arduino_GFX *gfx;")
    lines.append("")
    lines.append("#define SPR_W 16")
    lines.append("#define SPR_H 16")
    lines.append("#define RUN_FRAME_COUNT %d" % len(SEQUENCE))
    lines.append("#define SPR_TRANSPARENT 0")
    lines.append("")
    order = ['.', 'R', 'S', 'B']
    pal = ", ".join("0x%04X" % (rgb565(PALETTE[k]) if PALETTE[k] else 0) for k in order)
    lines.append("// Palette (RGB565). [0] is the transparent slot (never drawn).")
    lines.append("static const uint16_t RUN_PAL[] = { %s };" % pal)
    lines.append("")
    for fi, f in enumerate(FRAMES):
        lines.append("static const uint8_t RUN_F%d[SPR_W * SPR_H] = {" % fi)
        for r in range(H):
            vals = ", ".join(str(IDX[ch]) for ch in f[r])
            lines.append("  %s," % vals)
        lines.append("};")
    lines.append("")
    arr = ", ".join("RUN_F%d" % i for i in SEQUENCE)
    lines.append("// Playback sequence (stride, pass, stride, pass) for a smooth loop.")
    lines.append("static const uint8_t* const RUN_FRAMES[RUN_FRAME_COUNT] = { %s };" % arr)
    lines.append("")
    lines.append("#ifndef RUN_SCALE")
    lines.append("#define RUN_SCALE 14   // 16*14 = 224px, fits the 360px round dial")
    lines.append("#endif")
    lines.append("")
    lines.append("// Paint one frame as scale x scale blocks, centered at (cx,cy).")
    lines.append("// Transparent pixels are painted as `bg` so the fixed bounding box is")
    lines.append("// fully repainted each call (self-clearing, minimal flicker). flipH")
    lines.append("// mirrors horizontally so the runner can face either direction.")
    lines.append("static void drawSprite(uint8_t frame, int scale, int cx, int cy,")
    lines.append("                       uint16_t bg, bool flipH) {")
    lines.append("  const uint8_t *f = RUN_FRAMES[frame];")
    lines.append("  int x0 = cx - (SPR_W * scale) / 2;")
    lines.append("  int y0 = cy - (SPR_H * scale) / 2;")
    lines.append("  for (int row = 0; row < SPR_H; row++) {")
    lines.append("    for (int col = 0; col < SPR_W; col++) {")
    lines.append("      int sc = flipH ? (SPR_W - 1 - col) : col;")
    lines.append("      uint8_t idx = f[row * SPR_W + sc];")
    lines.append("      uint16_t c = (idx == SPR_TRANSPARENT) ? bg : RUN_PAL[idx];")
    lines.append("      gfx->fillRect(x0 + col * scale, y0 + row * scale, scale, scale, c);")
    lines.append("    }")
    lines.append("  }")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    check()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "preview"
    if cmd == "preview":
        preview(sys.argv[2] if len(sys.argv) > 2 else "preview.png")
    elif cmd == "emit":
        print(emit())
