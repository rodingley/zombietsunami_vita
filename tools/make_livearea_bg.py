#!/usr/bin/env python3
"""make_livearea_bg.py -- the LiveArea background (bg.png) from the original
banner picture (1024x500).

The Vita draws its own three buttons (search, update, PS Plus) over the top
centre of the LiveArea, where the banner's "Zombie Tsunami" logo is.  So the
banner is first made 15 % taller at the top, at the banner's own resolution
(1024x500 -> 1024x575).  The new strip is painted as the scene carrying on
upwards, not stretched, and lined up with the picture first: every edge that
crosses the top edge carries straight on along the line it follows there
(measured).  The sky continues, the clouds carry on and close into rounded tops,
the spire and the antenna end in a point, the buildings on the left and right
carry on (the red building's roof edge and grey trim continue straight on at
the angle measured from the picture, with one more bracket where the
perspective puts it),
and only then a light vertical motion blur, growing with height, blends it
into the real picture so the join cannot be seen.  That full-resolution picture is
saved as it is.

The LiveArea background itself is a fixed 840x500 pixels, drawn 1:1 on the
screen, so the 575-row picture has to go into 500 rows one of two ways:
  fit   (default)  shown at 87 % -- the whole picture, nothing cut off
  full             shown at 100 % -- the bottom 75 rows fall off the screen

LiveArea pictures must be 256-colour (palette) PNGs, so the result is reduced
to the 256 colours that match it best, judged the way the eye sees colour
(needs numpy, opencv, pillow, scikit-image, scikit-learn; takes ~10 s).
A full-colour copy of the 840x500 picture is written too (the start button is
made from it).

usage: make_livearea_bg.py <banner 1024x500> <out tall 1024x575> <out bg.png>
                           <out full-colour bg.png> [fit|full]
"""
import sys

import cv2
import numpy as np
from PIL import Image
from skimage import color
from sklearn.cluster import KMeans

SRC, OUT_TALL, OUT, OUT_RGB = sys.argv[1:5]
MODE = sys.argv[5] if len(sys.argv) > 5 else 'fit'

art = np.asarray(Image.open(SRC).convert('RGB')).astype(np.float32)
H, W = art.shape[:2]                              # 500, 1024
TOP = round(H * 0.15)                             # 75 new rows at the top


def extend_top(art, TOP=75, blur=True):
    H, W = art.shape[:2]
    art = art.astype(np.float32)
    ext = np.zeros((TOP, W, 3), np.float32)          # ext row r  <->  banner row r - TOP (negative)
    d = (TOP - np.arange(TOP))[:, None]               # rows above the banner's top edge: TOP .. 1
    X = np.arange(W)[None, :].astype(np.float32)

    def put(alpha, colour):                           # paint colour (array or per-pixel) with coverage alpha
        a = np.clip(alpha, 0, 1)[..., None]
        ext[:] = ext * (1 - a) + colour * a

    # -- the sky: the banner's sky colour, very slowly deepening upwards
    sky0 = np.array([11, 145, 235], np.float32)
    deep = np.array([-4, -10, -5], np.float32)
    ext[:] = sky0 + deep * (d / TOP)[..., None]

    # -- the grey building at the far left: its edge leans left going up (0.5 px a row),
    #    so it carries on for a few rows, copied along that edge
    for r in range(TOP):
        dd = TOP - r
        edge = 8.5 - 0.5 * dd
        if edge <= 0:
            continue
        for x in range(int(np.ceil(edge)) + 1):
            cover = np.clip(edge - x, 0, 1)
            sx, sy = min(W - 1, int(round(x + 0.5 * 20))), 20 - dd
            if sy < 0:
                continue
            ext[r, x] = ext[r, x] * (1 - cover) + art[sy, sx] * cover

    # -- the clouds cut by the top edge carry on upwards: their left and right
    #    edges continue exactly along the lines they follow just below the edge
    #    (measured), then close into a rounded top.  Across the cloud the colours
    #    are those of its top rows, at the same place across it (so its shaded
    #    underside stays on the same side), smoothed further up.
    def cloud_continue(L0, sL, R0, sR, K, p):
        rows_src = art[0:3]
        soft_src = cv2.GaussianBlur(rows_src, (0, 0), sigmaX=2.5, sigmaY=0.1)
        Ls = np.array([L0 + sL * r for r in range(3)]); Rs = np.array([R0 + sR * r for r in range(3)])
        for r in range(TOP):
            k = TOP - r                                   # rows above the edge
            if k > K:
                continue
            L, R = L0 - sL * k, R0 - sR * k               # the edges carried straight on
            e = 1 - np.sqrt(max(0.0, 1 - (k / K) ** 2))   # 0 at the edge, 1 at the top
            Wd = R - L
            L2, R2 = L + e * p * Wd, R - e * (1 - p) * Wd
            if R2 - L2 < 0.3:
                continue
            xs = np.arange(int(np.floor(L2)) - 1, int(np.ceil(R2)) + 2)
            cover = np.clip(np.minimum(xs + 1, R2) - np.maximum(xs, L2), 0, 1)
            if k == K or (k + 1) <= K:
                # soft top: fade the last rows a little
                cover *= np.clip((K - k + 1) / 2.0, 0, 1)
            rel = np.clip((xs + 0.5 - L2) / (R2 - L2), 0, 1)
            col = 0
            t_soft = min(1.0, k / 10.0)                   # exact colours at the edge, smoothed higher up
            for rs in range(3):
                xsrc = Ls[rs] + rel * (Rs[rs] - Ls[rs])
                x0i = np.clip(np.floor(xsrc).astype(int), 0, W - 2); f = (xsrc - x0i)[:, None]
                c_ex = rows_src[rs, x0i] * (1 - f) + rows_src[rs, x0i + 1] * f
                c_sm = soft_src[rs, x0i] * (1 - f) + soft_src[rs, x0i + 1] * f
                col = col + (c_ex * (1 - t_soft) + c_sm * t_soft) / 3
            col = col * (1 - 0.05 * e) + 255 * 0.05 * e    # a touch lighter towards the top
            ok = (xs >= 0) & (xs < W)
            xs, cover, col = xs[ok], cover[ok], col[ok]
            ext[r, xs] = ext[r, xs] * (1 - cover[:, None]) + col * cover[:, None]

    cloud_continue(51.7, 1.6, 146.5, 1.1, 24, 0.42)   # the long cloud top left (edges lean up-left)
    cloud_continue(603.0, -0.3, 636.0, 0.5, 16, 0.5)  # the puff behind the logo (narrows upwards)

    # -- the spire on the white building: tapers to a needle
    prof = art[0:2, 230:246].mean(0)                  # colours across it at the top edge
    for r in range(TOP):
        dd = TOP - r
        w = 7.0 * max(0.0, 1 - dd / 26.0)             # width 7 px at the edge, gone 26 rows up
        if w <= 0.05:
            continue
        cx = 237.5 - 0.3 * dd
        for x in range(int(cx - w), int(cx + w) + 2):
            cover = np.clip(min(x + 1, cx + w / 2) - max(x, cx - w / 2), 0, 1)
            if cover <= 0:
                continue
            t = (x + 0.5 - (cx - w / 2)) / max(w, 1e-3)       # 0..1 across the spire
            c = prof[int(np.clip(4 + t * 7, 0, 15))]
            ext[r, x] = ext[r, x] * (1 - cover) + c * cover

    # -- the thin antenna right of the logo: a few more rows, fading out
    ac = art[0:2, 823].mean(0)
    for r in range(TOP):
        dd = TOP - r
        if dd > 14:
            continue
        x = 823 - 0.17 * dd
        xi = int(np.floor(x)); f = x - xi
        a = 0.9 * (1 - dd / 15)
        ext[r, xi] = ext[r, xi] * (1 - a * (1 - f)) + ac * a * (1 - f)
        ext[r, xi + 1] = ext[r, xi + 1] * (1 - a * f) + ac * a * f

    # -- the red brick building: measured from the picture, its roof edge (the
    #    top of the grey trim) runs at x = 896.2 - 0.964*row and the dark line under
    #    the trim at x = 908.4 - 0.995*row (the trim gets a little thicker towards
    #    the top, in perspective).  Both lines carry straight on up; across them the
    #    building is rebuilt from its own top rows: the trim from rows 0-2, the
    #    shaded brick under it from rows 33-45 (between two brackets), and one more
    #    bracket where the perspective puts it.
    def x_top(row): return 896.23 - 0.964 * row
    def x_dark(row): return 908.40 - 0.995 * row
    yb = -d.astype(np.float32)                         # banner row of each ext row (negative)
    xt, xd = x_top(yb), x_dark(yb)
    v = (X - xt) / (xd - xt)                            # 0 at the roof edge, 1 at the dark line

    def sample(row, xf):                                # banner row, fractional x (per pixel)
        x0 = np.clip(np.floor(xf).astype(int), 0, W - 2); f = np.clip(xf - x0, 0, 1)[..., None]
        return art[row, x0] * (1 - f) + art[row, x0 + 1] * f

    # the trim: same place across it as in rows 0-2 (where the sky is behind it)
    trim = np.zeros_like(ext)
    for r0 in (0, 1, 2):
        trim += sample(r0, x_top(r0) + v * (x_dark(r0) - x_top(r0))) / 3
    # the brick under the trim: same distance below the dark line as in rows 33-45
    w_below = X - xd                                    # px below (right of) the dark line
    brick = np.zeros_like(ext)
    for r0 in (0, 1, 2):
        brick += sample(r0, x_dark(r0) + w_below) / 3
    # far right (the pale corner pillar of the building): straight up from rows 0-2
    corner = art[0:3].mean(0)[None].repeat(TOP, 0)
    bld = np.where((v < 1.15)[..., None], trim, brick)
    t_c = (np.clip((X - 980) / 6, 0, 1) * np.clip((w_below - 4) / 4, 0, 1))[..., None]
    bld = bld * (1 - t_c) + corner * t_c
    a_bld = np.clip((X - xt) + 0.5, 0, 1)               # anti-aliased roof edge against the sky
    put(a_bld, bld)

    # one more bracket under the trim: the brackets are 30 and 37 rows apart going
    # up (perspective), so the next one is about 45 rows above the top one, 1.2x larger
    bx0, bx1, by0, by1 = 892, 921, 1, 33               # box around the top bracket (centre ~ (899, 13))
    box = art[by0:by1, bx0:bx1]
    yy, xx = np.mgrid[by0:by1, bx0:bx1]
    beyond = xx - x_dark(yy)                            # under the dark line, not the trim itself
    r_, g_, b_ = box[..., 0], box[..., 1], box[..., 2]
    is_brick = (r_ > 150) & (g_ < 125) & (r_ - b_ > 90)
    m = ((beyond > 1.0) & ~is_brick).astype(np.float32)
    m = cv2.GaussianBlur(m, (0, 0), 0.5)
    S, DY = 1.2, -45.0
    DX = -0.995 * DY                                    # along the dark line
    ax, ay = 904.0, 3.0                                 # where the bracket hangs from the dark line
    # place: banner (x, y) of the new bracket pixel = attach' + S * ((x0, y0) - attach)
    for r in range(TOP):
        y = r - TOP
        sy = ay + (y - (ay + DY)) / S                   # source row in the box
        if sy < by0 or sy > by1 - 2:
            continue
        for x in range(int(ax + DX - 12), int(ax + DX + 30)):
            sx = ax + (x - (ax + DX)) / S
            if sx < bx0 or sx > bx1 - 2:
                continue
            iy, ix = int(sy) - by0, int(sx) - bx0; fy, fx = sy - int(sy), sx - int(sx)
            def bil(a):
                return (a[iy, ix] * (1 - fx) * (1 - fy) + a[iy, ix + 1] * fx * (1 - fy)
                        + a[iy + 1, ix] * (1 - fx) * fy + a[iy + 1, ix + 1] * fx * fy)
            a = bil(m)
            if a > 0.01 and x < W:
                ext[r, x] = ext[r, x] * (1 - a) + bil(box) * a

    tall = np.concatenate([ext, art], 0)
    if not blur:
        return tall

    # -- the soft blur: a light vertical motion blur that grows with height,
    #    nothing at all a few rows into the banner
    levels = [(0.0, 0.0), (0.6, 0.3), (1.2, 0.5), (2.0, 0.7), (3.0, 0.9), (4.0, 1.1), (5.0, 1.3)]
    top_part = tall[:TOP + 30]
    blurred = [top_part] + [cv2.GaussianBlur(top_part, (0, 0), sigmaX=sx, sigmaY=sy, borderType=cv2.BORDER_REFLECT)
                            for sy, sx in levels[1:]]
    out = tall.copy()
    for r in range(TOP + 30):
        dd = TOP - r                                  # >0 above the edge, <=0 inside the banner
        if dd > 0:
            lv = 1 + (len(levels) - 2) * (dd / TOP) ** 0.8
        else:
            lv = max(0.0, 1 + dd / 8.0)               # fades to sharp 8 rows into the banner
        i = int(np.floor(lv)); f = lv - i
        j = min(i + 1, len(levels) - 1)
        out[r] = blurred[i][r] * (1 - f) + blurred[j][r] * f
    return out


tall = extend_top(art, TOP)
tall_img = Image.fromarray(tall.round().clip(0, 255).astype(np.uint8))
tall_img.save(OUT_TALL)

# ---- into the LiveArea's 840x500
if MODE == 'full':
    pic_img = tall_img                            # 100 %: keep the top 500 rows
    note = f'shown at 100 %, bottom {tall_img.height - 500} rows cut off'
else:
    s = 500 / tall_img.height                     # 87 %: the whole picture
    pic_img = tall_img.resize((round(W * s), 500), Image.LANCZOS)
    note = f'shown at {s * 100:.0f} %, nothing cut off'
x0 = (pic_img.width - 840) // 2
rgb = pic_img.crop((x0, 0, x0 + 840, 500))
rgb.save(OUT_RGB)

# ---- 256 colours, chosen and placed the way the eye judges colour:
#  * the 256 colours: median cut, then refined to the picture (k-means) in CIE
#    Lab with lightness counted double, giving smooth areas (where a wrong
#    colour shows) more say than busy texture (which hides it);
#  * each pixel takes the nearest of them (again lightness first) and passes
#    85 % of what it got wrong on to its neighbours (serpentine Floyd-Steinberg),
#    so gradients stay smooth without visible grain.
LW = np.array([2.0, 1.0, 1.0], np.float32)       # lightness weight
lab = (color.rgb2lab(np.asarray(rgb)) * LW).astype(np.float32)
gray = cv2.cvtColor(np.asarray(rgb), cv2.COLOR_RGB2GRAY).astype(np.float32)
busy = cv2.GaussianBlur((gray - cv2.GaussianBlur(gray, (0, 0), 1.5)) ** 2, (0, 0), 2)
weight = (0.3 + 1 / (1 + busy / 60)).reshape(-1)
start = rgb.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
c0 = np.array(start.getpalette()[:256 * 3], np.uint8).reshape(1, -1, 3)
c0 = color.rgb2lab(c0)[0] * LW
km = KMeans(256, init=c0, n_init=1, max_iter=40, random_state=0)
km.fit(lab.reshape(-1, 3), sample_weight=weight)
pal_rgb = (color.lab2rgb((km.cluster_centers_ / LW)[None].astype(np.float64))[0] * 255).round().clip(0, 255).astype(np.uint8)
pal_lab = (color.rgb2lab(pal_rgb[None])[0] * LW).astype(np.float32)


def dither(img, pal, amount, clampv):
    h, w, _ = img.shape
    out = np.zeros((h, w), np.uint8)
    cur = np.zeros((w + 2, 3), np.float32)
    nxt = np.zeros((w + 2, 3), np.float32)
    for y in range(h):
        d = -1 if y & 1 else 1
        nxt[:] = 0
        row = img[y]
        for x in (range(w) if d > 0 else range(w - 1, -1, -1)):
            v = row[x] + np.clip(cur[x + 1] * amount, -clampv, clampv)
            diff = pal - v
            k = int(np.einsum('ij,ij->i', diff, diff).argmin())
            out[y, x] = k
            q = v - pal[k]
            cur[x + 1 + d] += q * (7 / 16)
            nxt[x + 1 - d] += q * (3 / 16)
            nxt[x + 1] += q * (5 / 16)
            nxt[x + 1 + d] += q * (1 / 16)
        cur, nxt = nxt, cur
    return out


idx = dither(lab, pal_lab, 0.85, 24.0)
q = Image.fromarray(idx, 'P')
q.putpalette(pal_rgb.flatten().tolist())
q.save(OUT, optimize=True)
qrgb = pal_rgb[idx]
b = lambda a: cv2.GaussianBlur(a.astype(np.float32), (0, 0), 1.0)
ref = color.rgb2lab(np.asarray(rgb))
seen = np.sqrt(((b(color.rgb2lab(qrgb)) - b(ref)) ** 2).sum(-1)).mean()
print(f'{OUT_TALL}: {W}x{H + TOP}; {OUT}: {note}, columns {x0}..{x0 + 840}, '
      f'colour difference as seen {seen:.2f} (lower is better)')
