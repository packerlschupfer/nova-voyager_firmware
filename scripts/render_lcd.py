#!/usr/bin/env python3
"""
Render synthetic ST7920 16x4 character LCD images for documentation.

Uses an 8x16 bitmap font (FONT_8X16, MIT licensed — see font8x16.py)
matching the ST7920's native HCGROM layout: each character glyph is
8 columns x 16 rows of physical dots. The visible space between
adjacent characters on a real module comes from the dot grid showing
through the empty padding columns of each cell, not from any extra
software-added gap.

Usage:
    python3 scripts/render_lcd.py            # all screens
    python3 scripts/render_lcd.py idle       # one screen by name

Output: docs/screenshots/lcd_*.png
"""

import sys
from pathlib import Path
from PIL import Image, ImageDraw

from font8x16 import FONT_8X16

# Layout: 16 chars x 4 rows of 8x16 cells = 128 x 64 dot panel
COLS, ROWS = 16, 4
CELL_W, CELL_H = 8, 16

SCALE = 8                       # Output pixel scale per logical dot
DOT_SIZE = SCALE - 1            # Lit-dot square size — leaves 1px gap to neighbours
                                # (88% fill, 12% gap — tight like the real ST7920)
PAD = 4                         # Inside-LCD padding (unscaled dots)
BEZEL = 14                      # Bezel around LCD (output pixels)

# Colors sampled from real LCD photos (yellow-green backlight)
BG       = (210, 230,  55)
DOT_OFF  = (180, 200,  45)
DOT_ON   = (  0,   0,   0)      # Pure black for max contrast
BEZEL_C  = ( 38,  38,  38)


def glyph(ch):
    """Return the 16-row x 8-col bitmap for a character."""
    code = ord(ch) if isinstance(ch, str) else ch
    if 0 <= code < len(FONT_8X16):
        return FONT_8X16[code]
    return FONT_8X16[ord("?")]


def _draw_dot(draw, cx_px, cy_px, color):
    """Draw a single dot anchored at the top-left of its cell, leaving 1px gap
    on the right/bottom (which becomes the gap to the next dot)."""
    draw.rectangle(
        [cx_px, cy_px, cx_px + DOT_SIZE - 1, cy_px + DOT_SIZE - 1],
        fill=color,
    )


def render_lcd(lines, out_path):
    assert len(lines) <= ROWS, f"too many lines: {len(lines)}"
    lines = [l[:COLS].ljust(COLS) for l in lines] + [" " * COLS] * (ROWS - len(lines))

    panel_cols = COLS * CELL_W   # 128
    panel_rows = ROWS * CELL_H   # 64

    pad_px  = PAD * SCALE
    lcd_w   = panel_cols * SCALE + 2 * pad_px
    lcd_h   = panel_rows * SCALE + 2 * pad_px

    img  = Image.new("RGB", (lcd_w, lcd_h), BG)
    draw = ImageDraw.Draw(img)

    # 1) Faint inactive-dot grid covering the entire 128x64 panel area.
    for py in range(panel_rows):
        for px_x in range(panel_cols):
            _draw_dot(draw, pad_px + px_x * SCALE, pad_px + py * SCALE, DOT_OFF)

    # 2) Lit glyph dots from the 8x16 bitmap.
    for r, line in enumerate(lines):
        for c, ch in enumerate(line):
            rows = glyph(ch)
            for gy in range(CELL_H):
                row_byte = rows[gy]
                if row_byte == 0:
                    continue
                for gx in range(CELL_W):
                    if row_byte & (0x80 >> gx):
                        px_col = c * CELL_W + gx
                        px_row = r * CELL_H + gy
                        _draw_dot(draw,
                                  pad_px + px_col * SCALE,
                                  pad_px + px_row * SCALE,
                                  DOT_ON)

    # Bezel
    final = Image.new("RGB", (img.width + 2 * BEZEL, img.height + 2 * BEZEL), BEZEL_C)
    final.paste(img, (BEZEL, BEZEL))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    final.save(out_path, optimize=True)
    print(f"wrote {out_path} ({final.width}x{final.height})")


# ---------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------

OUT_DIR = Path(__file__).resolve().parent.parent / "docs" / "screenshots"

SCREENS = {
    "idle": [
        "   0        1500",
        "0% IDL --- ---",
        "T:      D:   0.0",
        "SPD TAP DEP MOD",
    ],
    "drilling": [
        "1487        1500",
        "32% RUN ---- FWD",
        "T:      D:  12.4",
        "SPD TAP DEP MOD",
    ],
    "tapping": [
        " 580         600",
        "47% TAP:DIQ FWD",
        "T:25.0  D:  18.7",
        "SPD TAP DEP MOD",
    ],
    "menu": [
        "MENU > Tapping",
        ">[*]Depth   25.0",
        " [*]LdInc   80%",
        " [ ]LdSlp",
    ],
    "estop": [
        "!! E-STOP !!",
        "Release to clear",
        "",
        "",
    ],
}


if __name__ == "__main__":
    selected = sys.argv[1:] or list(SCREENS.keys())
    for name in selected:
        if name not in SCREENS:
            print(f"unknown screen: {name}", file=sys.stderr)
            sys.exit(1)
        render_lcd(SCREENS[name], OUT_DIR / f"lcd_{name}.png")
