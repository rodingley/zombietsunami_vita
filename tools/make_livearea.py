#!/usr/bin/env python3
"""make_livearea.py -- the LiveArea picture and its start button.

The big zombie at the front of the banner holds the game's pink button.  So that
nothing has to line up exactly on the Vita, the button plate (with its soft
shadow) is painted INTO the banner picture (bg.png), under the zombie's hands,
with his hands drawn back on top -- one picture, no seams.  The tappable start
item (startgame.png) is just the play arrow, sitting in the middle of the plate,
plus an invisible (almost fully see-through) area the size of the plate so the
whole button can be tapped.

Inputs (all full quality):
  * the banner picture, 1024x500 (bg.png is its middle 840 columns),
  * the game's own menu sprites from the 2048 px menu sheet: the pink plate
    (menu module 151) and the black play arrow (module 65).

The LiveArea centres startgame.png horizontally; the script prints the
template.xml y="..." for it.  Measured from a photo of the real LiveArea: image
centre row on the banner = 254 - y.

usage: make_livearea.py <banner 1024x500> <plate sprite> <arrow sprite> <out bg.png> <out startgame.png> [preview.png]
"""
import sys

import cv2
import numpy as np
from PIL import Image, ImageFilter

BANNER, PLATE, ARROW, OUT_BG, OUT_BTN = sys.argv[1:6]
PREVIEW = sys.argv[6] if len(sys.argv) > 6 else None

# ---- the banner (bg.png = its middle 840 columns)
src = Image.open(BANNER).convert('RGB')
OX = (src.width - 840) // 2
ban = np.asarray(src)[:, OX:OX + 840].copy()

# ---- the front zombie's hands
hsv = cv2.cvtColor(ban[:, :, ::-1].copy(), cv2.COLOR_BGR2HSV).astype(int)
h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
green = (h >= 33) & (h <= 90) & (s >= 60) & (v >= 30)
near = cv2.dilate(green.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
outline = (v < 100) & (h >= 25) & (h <= 95) & near   # their dark green outline (not the navy jeans)
cand = green | outline
HANDS = ((394, 440, 310, 364), (394, 440, 450, 508))  # y0, y1, x0, x1: fingers and wrists, not the body
hand = np.zeros_like(cand)
for y0, y1, x0, x1 in HANDS:
    sub = np.zeros_like(cand)
    sub[y0:y1, x0:x1] = cand[y0:y1, x0:x1]
    n, lab, st, _ = cv2.connectedComponentsWithStats(sub.astype(np.uint8), connectivity=8)
    if n > 1:
        hand |= lab == 1 + np.argmax(st[1:, cv2.CC_STAT_AREA])
# fill pinholes inside the hands (not the gaps between fingers, which open outwards)
inv = (~hand).astype(np.uint8)
cv2.floodFill(inv, np.zeros((hand.shape[0] + 2, hand.shape[1] + 2), np.uint8), (0, 0), 2)
hand = hand | (inv == 1)
# soft 1px edge, coloured like the hand next to it (never the road behind it)
hm = hand.astype(np.float32)
soft = np.clip(cv2.GaussianBlur(hm, (0, 0), 0.5) * 1.6 - 0.3, 0, 1)
nsum = cv2.boxFilter(hm, -1, (3, 3), normalize=False)
hand_a = np.where(hand, 1.0, np.where(nsum > 0, soft, 0.0))
hand_rgb = ban.astype(np.float32).copy()
for ch in range(3):
    csum = cv2.boxFilter(ban[..., ch].astype(np.float32) * hm, -1, (3, 3), normalize=False)
    hand_rgb[..., ch] = np.where(hand, ban[..., ch], csum / np.maximum(nsum, 1))

# ---- the pink plate, scaled once (premultiplied), centred under the two hands
plate = Image.open(PLATE).convert('RGBA')
PW = 158                                   # plate width on the banner (black edge included)
PH = round(plate.height * PW / plate.width)
PCX, PY0 = 408, 413                        # centre column between the hands; top just under the knuckles
PX0 = PCX - PW // 2
pl = plate.convert('RGBa').resize((PW, PH), Image.LANCZOS).convert('RGBA')

pic = Image.fromarray(ban).convert('RGBA')
shadow = Image.new('L', pic.size, 0)
shadow.paste(pl.getchannel('A'), (PX0 + 2, PY0 + 3))
shadow = shadow.filter(ImageFilter.GaussianBlur(2.5)).point(lambda a: int(a * 0.45))
pic.alpha_composite(Image.merge('RGBA', (Image.new('L', pic.size, 0),) * 3 + (shadow,)))
pic.alpha_composite(pl, (PX0, PY0))
# the hands back on top (only where the plate or its shadow changed the picture)
changed = (np.asarray(shadow) > 0)
changed[PY0:PY0 + PH, PX0:PX0 + PW] |= np.asarray(pl)[..., 3] > 0
ha = (hand_a * changed)[..., None]
p = np.asarray(pic).astype(np.float32)
p[..., :3] = p[..., :3] * (1 - ha) + hand_rgb * ha
pic = Image.fromarray(p.round().clip(0, 255).astype(np.uint8)).convert('RGB')

# ---- bg.png: 8-bit palette (LiveArea pictures must be).  The button's pink gets
# NBTN palette colours of its own (so it is smooth, not speckled); the other
# NREST are chosen for the rest of the picture (median cut, then refined).
NBTN, NREST = 32, 224
def refine(x, c, iters):
    for _ in range(iters):
        lab = np.concatenate([((x[i:i + 4096, None, :] - c[None]) ** 2).sum(-1).argmin(1)
                              for i in range(0, len(x), 4096)])
        for j in range(len(c)):
            m = lab == j
            if m.any():
                c[j] = x[m].mean(0)
    return c
pa = np.asarray(pic).astype(np.float32)
btn_area = np.zeros(pa.shape[:2], bool)
btn_area[PY0:PY0 + PH, PX0:PX0 + PW] = np.asarray(pl)[..., 3] > 128
btn_area &= ~hand
btn_area &= pa.max(axis=2) > 90          # the pink only; the black edge uses the main colours
rng = np.random.default_rng(1)
xr = pa[~btn_area]
start = Image.fromarray(xr.astype(np.uint8)[None]).quantize(colors=NREST, method=Image.Quantize.MEDIANCUT)
c_rest = np.array(start.getpalette()[:NREST * 3], np.float32).reshape(-1, 3)
c_rest = refine(xr[rng.choice(len(xr), 60000, replace=False)], c_rest, 8)
xb = pa[btn_area]
c_btn = refine(xb, xb[rng.choice(len(xb), NBTN, replace=False)].copy(), 30)
palimg = Image.new('P', (1, 1))
palimg.putpalette([int(round(v)) for v in np.concatenate([c_rest, c_btn]).flatten()])
qbg = np.asarray(pic.quantize(palette=palimg, dither=Image.Dither.FLOYDSTEINBERG)).copy()
# the button itself: dithered only among its own colours, so no stray dots
# from the picture around it creep into the pink
bpal = Image.new('P', (1, 1))
bpal.putpalette(([int(round(v)) for v in c_btn.flatten()] * 256)[:768])
sub = btn_area[PY0:PY0 + PH, PX0:PX0 + PW]
crop = pa[PY0:PY0 + PH, PX0:PX0 + PW].copy()
fill = cv2.inpaint(crop.round().astype(np.uint8), (~sub).astype(np.uint8), 3, cv2.INPAINT_TELEA)
crop[~sub] = fill[~sub]                     # non-pink pixels must not push errors into the pink
qb = np.asarray(Image.fromarray(crop.round().clip(0, 255).astype(np.uint8)).quantize(palette=bpal, dither=Image.Dither.FLOYDSTEINBERG))
region = qbg[PY0:PY0 + PH, PX0:PX0 + PW]
region[sub] = NREST + (qb[sub] % NBTN)
outbg = Image.fromarray(qbg, 'P')
outbg.putpalette(palimg.getpalette())
outbg.save(OUT_BG, optimize=True)

# ---- startgame.png: the play arrow in the middle of the plate + an invisible tap area
arrow = Image.open(ARROW).convert('RGBA')
ah = round(PH * 0.46)
aw = round(arrow.width * ah / arrow.height)
arr = arrow.convert('RGBa').resize((aw, ah), Image.LANCZOS).convert('RGBA')
ax0 = PX0 + (PW - aw) // 2 + 2
ay0 = PY0 + (PH - ah) // 2 - 1
LEFT = 420 - max(420 - PX0, PX0 + PW - 420)
W = 2 * (420 - LEFT)
TOP, H = PY0, PH
btn = Image.new('RGBA', (W, H), (0, 0, 0, 0))
tap = Image.new('RGBA', (PW, PH), (0, 0, 0, 2))            # 1% black: invisible, but still "there"
btn.paste(tap, (PX0 - LEFT, 0), pl.getchannel('A').point(lambda a: 255 if a > 0 else 0))
btn.alpha_composite(arr, (ax0 - LEFT, ay0 - TOP))
# palette: 254 colours for the arrow, one for the invisible tap area, one fully transparent
ca = np.asarray(btn)
qa = btn.quantize(colors=254, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
idx = np.asarray(qa).copy()
rgba = qa.getpalette('RGBA')[:254 * 4]
rgba += [0] * (254 * 4 - len(rgba))
idx[(ca[..., 3] > 0) & (ca[..., 3] <= 2)] = 254
idx[ca[..., 3] == 0] = 255
palette = [c for i in range(254) for c in rgba[i * 4:i * 4 + 3]] + [0, 0, 0, 0, 0, 0]
trns = [rgba[i * 4 + 3] for i in range(254)] + [2, 0]
out = Image.fromarray(idx, 'P')
out.putpalette(palette)
out.save(OUT_BTN, transparency=bytes(trns))

yattr = round(254 - (TOP + H / 2))
print(f'{OUT_BG}: plate {PW}x{PH} at ({PX0},{PY0}); {OUT_BTN}: {W}x{H}; template.xml: <image y="{yattr}">')
if PREVIEW:
    prev = Image.open(OUT_BG).convert('RGBA')
    prev.alpha_composite(Image.open(OUT_BTN).convert('RGBA'), (LEFT, TOP))
    prev.save(PREVIEW)
