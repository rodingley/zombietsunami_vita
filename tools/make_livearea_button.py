#!/usr/bin/env python3
"""make_livearea_button.py -- the LiveArea start button (startgame.png), a
separate picture drawn over the background (bg.png, made by
make_livearea_bg.py).

The big zombie at the front of the banner holds the game's pink button:
  * the pink plate (the game's menu sprite 151) with a soft shadow under it,
  * the shaded play arrow (the game's HUD sprite hud_2 023) in the middle,
  * the zombie's hands drawn back on top, so he holds the plate.
Only the part of the hands over the plate or its shadow is copied (plus a
2-pixel margin), from bg.png pixel for pixel with exactly the same colours
(startgame.png's palette holds those very colours), so where the copy ends
there is nothing to see.
Only where a hand's edge lies over the pink plate is the edge softened.

Where the LiveArea draws it: centred horizontally, and a picture that would
reach below the LiveArea's bottom edge is drawn sitting on that edge (seen in
two photos of the real LiveArea: with y="-190" and with y="-176" the picture's
bottom was on the background's last row both times).  So startgame.png is
made to reach exactly down to the bottom (row 500, the rows under the button
see-through) and template.xml uses y="-190", which puts it there: its place no
longer depends on a measurement.

usage: make_livearea_button.py <bg.png> <full-colour bg.png> <plate sprite>
                               <arrow sprite> <out startgame.png> [preview.png]
"""
import sys

import cv2
import numpy as np
from PIL import Image, ImageFilter
from skimage import color
from sklearn.cluster import KMeans

BG, BG_RGB, PLATE, ARROW, OUT = sys.argv[1:6]
PREVIEW = sys.argv[6] if len(sys.argv) > 6 else None

bgp = Image.open(BG)
assert bgp.mode == 'P' and bgp.size == (840, 500)
bg_idx = np.asarray(bgp)
bg_pal = np.array(bgp.getpalette()[:768], np.uint8).reshape(-1, 3)
bg = bg_pal[bg_idx]                                  # what the Vita shows
full = np.asarray(Image.open(BG_RGB).convert('RGB'))  # the same picture in full colour

# ---- the front zombie's hands and wrists
hsv = cv2.cvtColor(full[:, :, ::-1].copy(), cv2.COLOR_BGR2HSV).astype(int)
h, s, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
green = (h >= 33) & (h <= 90) & (s >= 60) & (v >= 30)
near = cv2.dilate(green.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
outline = (v < 100) & (h >= 25) & (h <= 95) & near   # their dark green outline (not the navy jeans)
cand = green | outline
HANDS = ((396, 446, 318, 380), (396, 446, 438, 500))  # y0, y1, x0, x1 around each hand and wrist
hand = np.zeros_like(cand)
for y0, y1, x0, x1 in HANDS:
    sub = np.zeros_like(cand)
    sub[y0:y1, x0:x1] = cand[y0:y1, x0:x1]
    n, lab, st, _ = cv2.connectedComponentsWithStats(sub.astype(np.uint8), connectivity=8)
    if n > 1:
        hand |= lab == 1 + np.argmax(st[1:, cv2.CC_STAT_AREA])
inv = (~hand).astype(np.uint8)                       # fill pinholes, not the gaps between fingers
cv2.floodFill(inv, np.zeros((502, 842), np.uint8), (0, 0), 2)
hand |= inv == 1

# ---- the plate, scaled once (premultiplied), held by its top corners
plate = Image.open(PLATE).convert('RGBA')
PW = 126
PH = round(plate.height * PW / plate.width)          # 57
PX0, PY0 = 344, 421                                  # corners under the hands, top edge behind the fingers
pl = plate.convert('RGBa').resize((PW, PH), Image.LANCZOS).convert('RGBA')

canvas = Image.new('RGBA', (840, 500), (0, 0, 0, 0))
sh = Image.new('L', (840, 500), 0)
sh.paste(pl.getchannel('A'), (PX0 + 2, PY0 + 3))
sh = sh.filter(ImageFilter.GaussianBlur(2.5)).point(lambda a: int(a * 0.45))
canvas.putalpha(sh)                                   # soft black shadow
# copy only the part of the hands that is over the plate or its shadow (plus a
# 2 px margin): everywhere else the background already shows those very pixels
need = np.asarray(sh) > 0
need[PY0:PY0 + PH, PX0:PX0 + PW] |= np.asarray(pl)[..., 3] > 0
need = cv2.dilate(need.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
copy = hand & need
canvas.alpha_composite(pl, (PX0, PY0))

# ---- the shaded play arrow, in the middle of the plate
arrow = Image.open(ARROW).convert('RGBA')
AH = 30
AW = round(arrow.width * AH / arrow.height)
ar = arrow.convert('RGBa').resize((AW, AH), Image.LANCZOS).convert('RGBA')
pa = np.asarray(pl)[..., 3] > 128
pcx = PX0 + np.nonzero(pa)[1].mean()                  # middle of the (slanted) plate
pcy = PY0 + np.nonzero(pa)[0].mean()
AX0 = int(round(pcx - AW / 2 + 1))                    # a touch right: a triangle looks left-heavy
AY0 = int(round(pcy - AH / 2))
canvas.alpha_composite(ar, (AX0, AY0))

# ---- the hands back on top: exact copies of bg.png's pixels
c = np.asarray(canvas).astype(np.float32)
c[copy, :3] = bg[copy]
c[copy, 3] = 255
# where a hand's edge lies over the plate, soften it with the hand's own colour
hm = hand.astype(np.float32)
soft = np.clip(cv2.GaussianBlur(hm, (0, 0), 0.5) * 1.6 - 0.3, 0, 1)
nsum = cv2.boxFilter(hm, -1, (3, 3), normalize=False)
on_plate = np.zeros((500, 840), bool)
on_plate[PY0:PY0 + PH, PX0:PX0 + PW] = np.asarray(pl)[..., 3] == 255
rim = (~hand) & (nsum > 0) & on_plate
for ch in range(3):
    csum = cv2.boxFilter(full[..., ch].astype(np.float32) * hm, -1, (3, 3), normalize=False)
    hc = csum / np.maximum(nsum, 1)
    c[..., ch] = np.where(rim, hc * soft + c[..., ch] * (1 - soft), c[..., ch])

# ---- the picture's extent: centred on the LiveArea's middle column (420), even size
ys, xs = np.nonzero(c[..., 3] > 0)
half = max(420 - xs.min(), xs.max() + 1 - 420)
half += half & 1
LEFT, W = 420 - half, 2 * half
TOP, BOT = ys.min(), 500                             # down to the LiveArea's bottom edge
if (BOT - TOP) & 1:
    TOP -= 1
H = BOT - TOP
c = c[TOP:BOT, LEFT:LEFT + W]
copy_c = copy[TOP:BOT, LEFT:LEFT + W]
alpha = c[..., 3].round().astype(int)

# ---- palette (256 entries): 1 fully see-through, NS shadow shades, the exact
# hand colours from bg.png, and the rest for the plate and arrow
out = np.zeros((H, W), np.uint8)
pal = []                                              # (r, g, b, a)
pal.append((0, 0, 0, 0))
semi = (alpha > 0) & (alpha < 255) & ~copy_c
NS = 32
sv = np.concatenate([c[semi][:, :3] * c[semi][:, 3:] / 255, c[semi][:, 3:]], 1)  # premultiplied
ks = KMeans(min(NS, len(np.unique(sv.round(), axis=0))), n_init=1, random_state=0).fit(sv)
for cc in ks.cluster_centers_:
    a = max(1, min(254, round(cc[3])))
    pal.append(tuple(int(round(min(255, x * 255 / a))) for x in cc[:3]) + (a,))
out[semi] = 1 + ks.predict(sv)

hand_ids = np.unique(bg_idx[TOP:BOT, LEFT:LEFT + W][copy_c])
base = len(pal)
remap = np.zeros(256, int)
for i, k in enumerate(hand_ids):
    pal.append(tuple(int(x) for x in bg_pal[k]) + (255,))
    remap[k] = base + i
out[copy_c] = remap[bg_idx[TOP:BOT, LEFT:LEFT + W][copy_c]]

solid = (alpha == 255) & ~copy_c                       # plate, arrow, softened hand edges
NK = 256 - len(pal)
LW = np.array([2.0, 1.0, 1.0], np.float32)
lab = (color.rgb2lab(c[..., :3].round().clip(0, 255).astype(np.uint8)) * LW).astype(np.float32)
kk = KMeans(NK, n_init=1, random_state=0).fit(lab[solid])
new_rgb = (color.lab2rgb((kk.cluster_centers_ / LW)[None].astype(np.float64))[0] * 255).round().clip(0, 255).astype(np.uint8)
choice_rgb = np.concatenate([new_rgb, bg_pal[hand_ids]])  # the hand colours may be used too
choice_lab = (color.rgb2lab(choice_rgb[None])[0] * LW).astype(np.float32)
for rgb_ in new_rgb:
    pal.append(tuple(int(x) for x in rgb_) + (255,))
choice_idx = np.concatenate([np.arange(len(pal) - NK, len(pal)), base + np.arange(len(hand_ids))])

# dither the solid part among those colours only (the rest is filled in with
# nearby solid colours first so it pushes no error into the plate)
fill = cv2.inpaint(c[..., :3].round().clip(0, 255).astype(np.uint8), (~solid).astype(np.uint8), 3, cv2.INPAINT_TELEA)
src = np.where(solid[..., None], c[..., :3], fill).round().clip(0, 255).astype(np.uint8)
src_lab = (color.rgb2lab(src) * LW).astype(np.float32)


def dither(img, palette, amount, clampv):
    hh, ww, _ = img.shape
    res = np.zeros((hh, ww), int)
    cur = np.zeros((ww + 2, 3), np.float32)
    nxt = np.zeros((ww + 2, 3), np.float32)
    for y in range(hh):
        d = -1 if y & 1 else 1
        nxt[:] = 0
        for x in (range(ww) if d > 0 else range(ww - 1, -1, -1)):
            vv = img[y, x] + np.clip(cur[x + 1] * amount, -clampv, clampv)
            diff = palette - vv
            k = int(np.einsum('ij,ij->i', diff, diff).argmin())
            res[y, x] = k
            q = vv - palette[k]
            cur[x + 1 + d] += q * (7 / 16)
            nxt[x + 1 - d] += q * (3 / 16)
            nxt[x + 1] += q * (5 / 16)
            nxt[x + 1 + d] += q * (1 / 16)
        cur, nxt = nxt, cur
    return res


dq = dither(src_lab, choice_lab, 0.85, 24.0)
out[solid] = choice_idx[dq[solid]]
assert len(pal) == 256

img = Image.fromarray(out, 'P')
img.putpalette([x for p in pal for x in p[:3]])
img.save(OUT, transparency=bytes(p[3] for p in pal), optimize=False)

yattr = -190                                         # sits on the bottom edge (see above)
print(f'{OUT}: {W}x{H} at ({LEFT},{TOP}); plate {PW}x{PH} at ({PX0},{PY0}); arrow {AW}x{AH} at ({AX0},{AY0}); '
      f'{len(hand_ids)} hand colours copied exactly; template.xml: <image y="{yattr}">')

# ---- check: drawn over bg.png, every copied hand pixel is exactly bg.png's
chk = Image.open(OUT).convert('RGBA')
comp = Image.fromarray(bg).convert('RGBA')
comp.alpha_composite(chk, (LEFT, TOP))
comp = np.asarray(comp)[..., :3]
copy_full = np.zeros((500, 840), bool)
copy_full[TOP:BOT, LEFT:LEFT + W] = copy_c
assert (comp[copy_full] == bg[copy_full]).all(), 'copied hand pixels differ from bg.png'
touched = np.zeros((500, 840), bool)
touched[TOP:BOT, LEFT:LEFT + W] = np.asarray(chk)[..., 3] > 0
same = (comp == bg).all(-1)
print(f'check: {copy_full.sum()} hand pixels identical to bg.png; '
      f'{(~same & ~touched).sum()} pixels changed outside the button picture')
if PREVIEW:
    Image.fromarray(comp).save(PREVIEW)
