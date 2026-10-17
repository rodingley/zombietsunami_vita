#!/usr/bin/env python3
"""make_popup.py -- the words of the report-bugs pop-up (popup/popup_text.png).

usage: make_popup.py <BerlinSansFBDemi-Bold.ttf> <out.png>

Only our own text is in the picture.  The pop-up itself (the game's white
panel, its X button, the warning sign and the blue banner with its shine) is cut
out of the game's own sprite sheets on the Vita when the port starts
(src/game_art.c, src/popup.c), so the port ships none of the game's art.

The layout copies the game's own pop-ups (numbers from libcgame):
  * the pop-up's centre: (226, 155) points = (452, 263.5) on the 960x544 screen;
    1 point = 1.7 pixels (a 480x320 layout stretched to the screen)
  * title: the pop-up title box (-90,-122) 180x20 points, white with the game's
    black text outline (6 units at 640 px high), centred
  * between the title and the banner (where event pop-ups have their picture):
    who made the port and its version
  * the blue banner of the mission pop-ups (the game has it at (0,60) points;
    here a little higher, (0,45), so the contact list fits under it), 547x61 px;
    its text is plain white, like the game's banner text
  * other text: the game's pop-up text blue (54,138,194)
The picture covers the screen rectangle x 80..824, y 0..530 (BX0, BY0 below);
popup.c draws it there, keep the two in step.
"""
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

FONT, OUT = sys.argv[1], sys.argv[2]

BX0, BY0, BW, BH = 80, 0, 744, 530    # where the picture sits on the screen (popup.c)
CX, CY, PT = 452.0, 263.5, 1.7        # the pop-up's centre, pixels per point
BLUE = (54, 138, 194)                 # the game's pop-up text colour (libcgame 0x19f6f6)
# the game's outlined text (libcgame 0x28fb4e..0x28fbf8, FreeType stroker 0x290ce6):
# a black stroke of 2 x (screen height / 640) px round the letters, pushed out
# 2 x that scale to the right and 1 x down -- heavier on the right, the 3D look
OUTLINE = 2 * 544 / 640               # 1.7 px
DROP = (2 * 544 / 640, 544 / 640)     # 1.7 px right, 0.85 px down
SS = 3                                # drawn 3x bigger, then scaled down (smooth edges)

TITLE = "ZOMBIE TSUNAMI VITA"
SLOT = [("PS Vita port by rodi", 40), ("Version 1.0", 26)]
BANNER = "Found a bug? Report it here!"
LIST = [("Instagram:", "@rodingley"), ("Discord:", "rodingley")]   # one under the other
LAST = "Just send me a DM"
BANNER_PT = 45                        # banner centre, points below the pop-up's centre (popup.c)


def pt(x, y):
    return CX + x * PT, CY + y * PT


img = Image.new("RGBA", (BW * SS, BH * SS), (0, 0, 0, 0))


def font(size):
    return ImageFont.truetype(FONT, size * SS)


def fit(text, size, w, h):
    """the game shrinks text until it fits its box"""
    while size > 8:
        l, t, r, b = font(size).getbbox(text)
        if (r - l) / SS <= w and size * 1.15 <= h:
            return size
        size -= 1
    return size


def text(xy, s, size, fill, outline=0, anchor="mm", drop=None):
    """outline: black stroke that many pixels wide; drop (dx, dy): the outline is
    smeared that far right and down, so it is thicker there (the game's 3D edge)"""
    x, y = (xy[0] - BX0) * SS, (xy[1] - BY0) * SS
    f = font(size)
    if outline:
        om = Image.new("L", img.size, 0)
        d = ImageDraw.Draw(om)
        sw = max(1, round(outline * SS))
        n = max(1, round(max(drop) * SS)) if drop else 0
        for k in range(n + 1):
            dx, dy = (drop[0] * SS * k / n, drop[1] * SS * k / n) if n else (0, 0)
            d.text((x + dx, y + dy), s, font=f, fill=255, stroke_width=sw, stroke_fill=255, anchor=anchor)
        a = np.asarray(om)
        holes = ndimage.binary_fill_holes(a > 100) & (a <= 100)   # no holes inside the outline
        om = Image.fromarray(np.where(holes, 255, a).astype(np.uint8))
        img.paste((0, 0, 0, 255), (0, 0), om)
    m = Image.new("L", img.size, 0)
    ImageDraw.Draw(m).text((x, y), s, font=f, fill=255, anchor=anchor)
    img.paste(tuple(fill) + (255,), (0, 0), m)


# title: the game's title box is (-90,-122) 180x20 points; the game's pop-ups
# show their title lower than the middle of that box, so it sits a little below
# the box's bottom edge here
x0, y0 = pt(-90, -122)
w, h = 180 * PT, 20 * PT
tsize = fit(TITLE, 40, w, h)
title_y = y0 + h + 6
text((x0 + w / 2, title_y), TITLE, tsize, (255, 255, 255), OUTLINE, drop=DROP)
title_bottom = title_y + tsize * 0.6

# who made it: centred between the title and the banner (where event pop-ups
# have their picture)
bx, by = pt(0, BANNER_PT)
sx, sy = CX, (title_bottom + (by - 61 / 2)) / 2
sizes = [fit(s, z, 460, 60) for s, z in SLOT]
gap = 0.62
text((sx, sy - sizes[1] * gap), SLOT[0][0], sizes[0], BLUE)
text((sx, sy + sizes[0] * gap), SLOT[1][0], sizes[1], BLUE)

# on the blue banner (547 x 61 px, its slanted ends take ~30 px each side): plain
# white, the capitals centred on the bar (the bar itself sits 2.5 px above its
# sprite's centre in the middle of the screen)
bsize = fit(BANNER, 34, 470, 50)
cap = font(bsize).getbbox("H", anchor="ls")[1] / SS          # capital height (negative: up)
text((bx, by - 2.5 - cap / 2), BANNER, bsize, (255, 255, 255), anchor="ms")

# under the banner, down to the panel's red line (y 470 in the middle): the
# contacts as a list (labels lined up on their colons), then the last line
top, bottom = by + 61 / 2, 466
size = 23
pitch = size * 1.22
rows = len(LIST) + 1
mid = (top + bottom) / 2
ys = [mid + (i - (rows - 1) / 2) * pitch for i in range(rows)]
colon = CX - 8
for (label, value), y in zip(LIST, ys):
    text((colon, y), label, size, BLUE, anchor="rm")
    text((colon + 12, y), value, size, BLUE, anchor="lm")
text((CX, ys[-1]), LAST, size, BLUE)

img = img.resize((BW, BH), Image.LANCZOS)
img.save(OUT, optimize=True)
print("wrote", OUT, img.size)
