/* game_art.c -- pieces of the game's own sprite sheets, read from the memory card.
 *
 * The port ships none of the game's pictures.  The report-bugs pop-up's white
 * panel and X button, and the warning sign on it and on the main menu's button,
 * are cut out of the game's own sheets when they are needed:
 * assets/res/zombies/com/gfx/sprites/NAME_pvrgz.jet (a gzipped PVR v3 texture,
 * RGBA4444, premultiplied) and NAME.spr (where each sprite, "module", sits).
 * A sheet is unzipped piece by piece and only the rows we need are kept, so
 * this needs very little memory. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_art.h"
#include "zlib/zlib.h"

#ifdef GA_HOST_TEST /* built on a PC to check the result (tools/) */
#define ga_log(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#ifndef GA_ROOT
#define GA_ROOT "assets"
#endif
#define lock_hud()
#define unlock_hud()
#else
#include <psp2/kernel/threadmgr.h>
#include "config.h"
#include "so_platform.h"
#define ga_log so_log
#define GA_ROOT ASSET_PATH
static SceKernelLwMutexWork hud_lock;
static int hud_lock_made;
static void lock_hud(void) {
  /* the first call is made on the main thread at start-up (popup_preload),
   * before any other thread can get here */
  if (!hud_lock_made)
    hud_lock_made = sceKernelCreateLwMutex(&hud_lock, "game_art", 0, 0, NULL) >= 0 ? 1 : -1;
  if (hud_lock_made > 0)
    sceKernelLockLwMutex(&hud_lock, 1, NULL);
}
static void unlock_hud(void) {
  if (hud_lock_made > 0)
    sceKernelUnlockLwMutex(&hud_lock, 1);
}
#endif

#define SPRITES GA_ROOT "/res/zombies/com/gfx/sprites/"

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* where modules sit in the sheet: the .spr's "MODU" block, 20 bytes per module,
 * starting with x, y, w, h */
static int read_modules(const char *spr, const int *mods, int n, int (*rect)[4]) {
  char path[256];
  snprintf(path, sizeof(path), SPRITES "%s.spr", spr);
  FILE *f = fopen(path, "rb");
  if (!f) {
    ga_log("[art] %s.spr not found", spr);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *d = len > 32 ? malloc((size_t)len) : NULL;
  int ok = d && fread(d, 1, (size_t)len, f) == (size_t)len;
  fclose(f);
  long j = -1;
  for (long i = 0; ok && i + 14 <= len && i < 4096; i++)
    if (!memcmp(d + i, "MODU", 4)) {
      j = i;
      break;
    }
  if (j < 0) {
    ga_log("[art] %s.spr: no module list", spr);
    free(d);
    return -1;
  }
  unsigned count = rd16(d + j + 12);
  for (int k = 0; k < n; k++) {
    long q = j + 14 + 20L * mods[k];
    if (mods[k] < 0 || (unsigned)mods[k] >= count || q + 8 > len) {
      ga_log("[art] %s.spr: no module %d", spr, mods[k]);
      free(d);
      return -1;
    }
    for (int c = 0; c < 4; c++)
      rect[k][c] = (int)rd16(d + q + 2 * c);
  }
  free(d);
  return 0;
}

int ga_cut(const char *sheet, const char *spr, const int *mods, int n, int pad, ga_img *out) {
  int rect[16][4];
  memset(out, 0, sizeof(ga_img) * (size_t)n);
  if (n > 16 || read_modules(spr, mods, n, rect) < 0)
    return -1;
  char path[256];
  snprintf(path, sizeof(path), SPRITES "%s_pvrgz.jet", sheet);
  FILE *f = fopen(path, "rb");
  if (!f) {
    ga_log("[art] %s_pvrgz.jet not found", sheet);
    return -1;
  }
  int last_row = 0;
  for (int k = 0; k < n; k++) {
    out[k].w = rect[k][2];
    out[k].h = rect[k][3];
    out[k].pad = pad;
    out[k].px = calloc((size_t)(rect[k][2] + 2 * pad) * (rect[k][3] + 2 * pad), 4);
    if (!out[k].px)
      goto fail;
    if (rect[k][1] + rect[k][3] + pad > last_row)
      last_row = rect[k][1] + rect[k][3] + pad;
  }

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
    goto fail;
  enum { IN = 65536 };
  unsigned char *in = malloc(IN), hdr[52];
  unsigned char *row = NULL;
  int W = 0, H = 0, y = 0, got = 0, stage = 0, ret = Z_OK; /* stage 0: header, 1: rows */
  uint32_t row_bytes = 52;
  zs.next_out = hdr;
  zs.avail_out = 52;
  while (in && ret != Z_STREAM_END && y < last_row) {
    if (zs.avail_in == 0) {
      size_t r = fread(in, 1, IN, f);
      if (!r)
        break;
      zs.next_in = in;
      zs.avail_in = (uInt)r;
    }
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
      break;
    if (zs.avail_out)
      continue;
    if (stage == 0) {
      /* PVR v3, RGBA 4444, no extra data */
      if (memcmp(hdr, "PVR\3", 4) || rd32(hdr + 8) != 0x61626772u || rd32(hdr + 12) != 0x04040404u ||
          rd32(hdr + 48) != 0) {
        ga_log("[art] %s: not the expected kind of picture", sheet);
        break;
      }
      H = (int)rd32(hdr + 24);
      W = (int)rd32(hdr + 28);
      row_bytes = (uint32_t)W * 2;
      row = W > 0 && W <= 8192 ? malloc(row_bytes) : NULL;
      if (!row)
        break;
      for (int k = 0; k < n; k++)
        if (rect[k][0] + rect[k][2] > W || rect[k][1] + rect[k][3] > H) {
          ga_log("[art] %s: sprite outside the sheet", sheet);
          H = 0;
        }
      if (!H)
        break;
      stage = 1;
    } else {
      /* one sheet row: copy it into every piece that wants it (edges repeat
       * past the sheet's own edge) */
      for (int k = 0; k < n; k++) {
        int iw = rect[k][2] + 2 * pad, ih = rect[k][3] + 2 * pad;
        for (int oy = 0; oy < ih; oy++) {
          if (clampi(rect[k][1] - pad + oy, 0, H - 1) != y)
            continue;
          uint8_t *dst = out[k].px + (size_t)oy * iw * 4;
          for (int ox = 0; ox < iw; ox++) {
            int sx = clampi(rect[k][0] - pad + ox, 0, W - 1);
            unsigned v = row[2 * sx] | row[2 * sx + 1] << 8;
            dst[4 * ox + 0] = (uint8_t)((v >> 12) * 17);
            dst[4 * ox + 1] = (uint8_t)(((v >> 8) & 15) * 17);
            dst[4 * ox + 2] = (uint8_t)(((v >> 4) & 15) * 17);
            dst[4 * ox + 3] = (uint8_t)((v & 15) * 17);
          }
          got++;
        }
      }
      y++;
    }
    zs.next_out = row;
    zs.avail_out = row_bytes;
  }
  inflateEnd(&zs);
  free(in);
  free(row);
  fclose(f);
  f = NULL;
  int want = 0;
  for (int k = 0; k < n; k++)
    want += rect[k][3] + 2 * pad;
  if (stage == 1 && got >= want)
    return 0;
  ga_log("[art] %s: could not read the sprites (%d of %d rows)", sheet, got, want);
fail:
  if (f)
    fclose(f);
  for (int k = 0; k < n; k++)
    ga_free(&out[k]);
  return -1;
}

void ga_free(ga_img *im) {
  free(im->px);
  im->px = NULL;
}

static void sample(const ga_img *im, float fx, float fy, float *o) {
  int iw = im->w + 2 * im->pad, ih = im->h + 2 * im->pad;
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  if (fx > iw - 1) fx = (float)(iw - 1);
  if (fy > ih - 1) fy = (float)(ih - 1);
  int x0 = (int)fx, y0 = (int)fy;
  int x1 = x0 + 1 < iw ? x0 + 1 : x0, y1 = y0 + 1 < ih ? y0 + 1 : y0;
  float ax = fx - x0, ay = fy - y0;
  const uint8_t *p00 = im->px + ((size_t)y0 * iw + x0) * 4, *p10 = im->px + ((size_t)y0 * iw + x1) * 4;
  const uint8_t *p01 = im->px + ((size_t)y1 * iw + x0) * 4, *p11 = im->px + ((size_t)y1 * iw + x1) * 4;
  for (int c = 0; c < 4; c++)
    o[c] = (p00[c] * (1 - ax) + p10[c] * ax) * (1 - ay) + (p01[c] * (1 - ax) + p11[c] * ax) * ay;
}

void ga_blit(ga_canvas *c, const ga_img *im, float x0, float y0, float x1, float y1, int flip_x, int flip_y,
             int bright) {
  if (!im || !im->px || x1 <= x0 || y1 <= y0)
    return;
  /* the pixels whose centres fall inside the rectangle (like the GPU) */
  int px0 = clampi((int)ceilf(x0 - 0.5f), 0, c->w), px1 = clampi((int)ceilf(x1 - 0.5f), 0, c->w);
  int py0 = clampi((int)ceilf(y0 - 0.5f), 0, c->h), py1 = clampi((int)ceilf(y1 - 0.5f), 0, c->h);
  float su = im->w / (x1 - x0), sv = im->h / (y1 - y0); /* sprite pixels per screen pixel */
  int nx = su > 1 ? (int)ceilf(su) : 1, ny = sv > 1 ? (int)ceilf(sv) : 1; /* shrinking: average a few */
  for (int py = py0; py < py1; py++) {
    uint8_t *d = c->px + ((size_t)py * c->w + px0) * 4;
    for (int px = px0; px < px1; px++, d += 4) {
      float acc[4] = { 0, 0, 0, 0 };
      for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
          float u = (px + (i + 0.5f) / nx - x0) * su, v = (py + (j + 0.5f) / ny - y0) * sv;
          if (flip_x) u = im->w - u;
          if (flip_y) v = im->h - v;
          float s[4];
          sample(im, im->pad + u - 0.5f, im->pad + v - 0.5f, s);
          for (int k = 0; k < 4; k++)
            acc[k] += s[k];
        }
      float inv = 1.0f / (nx * ny);
      for (int k = 0; k < 4; k++)
        acc[k] *= inv;
      if (bright)
        for (int k = 0; k < 3; k++) {
          acc[k] = acc[k] * 1.2f + 40.0f * acc[3] / 255.0f;
          if (acc[k] > acc[3]) acc[k] = acc[3];
        }
      float keep = 1.0f - acc[3] / 255.0f;
      for (int k = 0; k < 4; k++) {
        float v = acc[k] + d[k] * keep;
        d[k] = (uint8_t)(v > 255 ? 255 : v + 0.5f);
      }
    }
  }
}

/* ---- the warning sign (hud sheet) ----
 * The game's in-run warning: hud.spr module 8 (the triangle) at (-64,-56) and
 * module 44 (the "!") at (-23,-48) drawn at 0.87 of its size round its own
 * centre -- the last frame of the sign's animation.  Its baked-in shadow is
 * taken off and a black outline drawn round it instead, like the game's
 * pop-up badges. */
static ga_img hud[2];
static int hud_state; /* 0 not read yet, 1 ok, -1 could not */

/* the triangle comes with a soft black shadow baked in (pure black, fading out);
 * the game's pop-up badges have a black outline instead, so the shadow is taken off */
static void remove_shadow(ga_img *im) {
  int n = (im->w + 2 * im->pad) * (im->h + 2 * im->pad);
  for (int i = 0; i < n; i++) {
    uint8_t *p = im->px + 4 * i;
    int m = p[0] > p[1] ? p[0] : p[1];
    if (p[2] > m) m = p[2];
    if (p[3] && m * 255 < 40 * p[3]) /* (nearly) black: shadow */
      p[0] = p[1] = p[2] = p[3] = 0;
  }
}

static int hud_pieces(void) {
  lock_hud();
  if (!hud_state) {
    static const int mods[2] = { 8, 44 };
    hud_state = ga_cut("hud_16bpp", "hud", mods, 2, 2, hud) == 0 ? 1 : -1;
    if (hud_state > 0)
      remove_shadow(&hud[0]);
  }
  int ok = hud_state > 0;
  unlock_hud();
  return ok;
}

static void draw_sign(ga_canvas *c, float cx, float cy, float s) {
  const ga_img *tri = &hud[0], *ex = &hud[1];
  ga_blit(c, tri, cx - 64 * s, cy - 56 * s, cx + (-64 + tri->w) * s, cy + (-56 + tri->h) * s, 0, 0, 0);
  float ecx = -23 + ex->w / 2.0f, ecy = -48 + ex->h / 2.0f;
  float hw = ex->w * 0.87f / 2, hh = ex->h * 0.87f / 2;
  ga_blit(c, ex, cx + (ecx - hw) * s, cy + (ecy - hh) * s, cx + (ecx + hw) * s, cy + (ecy + hh) * s, 0, 0, 0);
}

int ga_draw_warning(ga_canvas *c, float cx, float cy, float s, float outline) {
  if (!hud_pieces())
    return 0;
  if (outline <= 0) {
    draw_sign(c, cx, cy, s);
    return 1;
  }
  /* drawn on its own first, then a black copy grown by `outline` pixels goes behind it */
  int r = (int)ceilf(outline);
  int w = (int)ceilf(124 * s) + 2 * r + 4, h = (int)ceilf(115 * s) + 2 * r + 4;
  float ox = cx - 64 * s - r - 2, oy = cy - 56 * s - r - 2; /* where the little canvas sits */
  int ix = (int)floorf(ox), iy = (int)floorf(oy);
  ga_canvas t = { w + 1, h + 1, calloc((size_t)(w + 1) * (h + 1), 4) };
  if (!t.px) {
    draw_sign(c, cx, cy, s);
    return 1;
  }
  draw_sign(&t, cx - ix, cy - iy, s);
  uint8_t *grown = calloc((size_t)t.w * t.h, 1);
  if (grown) {
    for (int y = 0; y < t.h; y++)
      for (int x = 0; x < t.w; x++) {
        int best = 0;
        for (int dy = -r; dy <= r && best < 255; dy++)
          for (int dx = -r; dx <= r; dx++) {
            int yy = y + dy, xx = x + dx;
            if (yy < 0 || yy >= t.h || xx < 0 || xx >= t.w) continue;
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > outline + 0.5f) continue;
            int a = t.px[((size_t)yy * t.w + xx) * 4 + 3];
            if (d > outline - 0.5f) a = (int)(a * (outline + 0.5f - d)); /* soft edge */
            if (a > best) best = a;
          }
        grown[(size_t)y * t.w + x] = (uint8_t)best;
      }
  }
  for (int y = 0; y < t.h; y++)
    for (int x = 0; x < t.w; x++) {
      int X = ix + x, Y = iy + y;
      if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
      uint8_t *d = c->px + ((size_t)Y * c->w + X) * 4;
      const uint8_t *sp = t.px + ((size_t)y * t.w + x) * 4;
      float ga = grown ? grown[(size_t)y * t.w + x] : 0; /* black outline layer */
      float keep = 1.0f - ga / 255.0f;
      for (int k = 0; k < 3; k++) d[k] = (uint8_t)(d[k] * keep);
      d[3] = (uint8_t)(ga + d[3] * keep);
      float ks = 1.0f - sp[3] / 255.0f; /* the sign on top */
      for (int k = 0; k < 4; k++) {
        float v = sp[k] + d[k] * ks;
        d[k] = (uint8_t)(v > 255 ? 255 : v + 0.5f);
      }
    }
  free(grown);
  free(t.px);
  return 1;
}

/* ---- the main menu's report-bugs button ----
 * It replaces the menu's Bluesky icon, menu.spr module 173 (70 x 70 in the big
 * sheet, half that in the small one).  The warning sign is fitted into it, and
 * the sheet's border round the sprite (`pad` pixels) repeats its edge pixels. */
int ga_menu_icon(int small, unsigned *sheet_w, unsigned *sheet_h, unsigned *x, unsigned *y, unsigned *w,
                 unsigned *h, uint16_t **px) {
  static const int mod = 173;
  int r[1][4];
  *px = NULL;
  if (read_modules("menu", &mod, 1, r) < 0)
    return -1;
  int size = small ? 35 : 70, pad = small ? 1 : 2;
  int mx = small ? r[0][0] / 2 : r[0][0], my = small ? r[0][1] / 2 : r[0][1];
  int mw = small ? r[0][2] / 2 : r[0][2], mh = small ? r[0][3] / 2 : r[0][3];
  if (mw != size || mh != size) {
    ga_log("[art] menu button sprite is %dx%d, not %dx%d: left alone", mw, mh, size, size);
    return -1;
  }
  int tw = size + 2 * pad;
  ga_canvas c = { tw, tw, calloc((size_t)tw * tw, 4) };
  if (!c.px)
    return -1;
  /* the outlined sign is made exactly as big as the Bluesky logo it replaces:
   * that logo fills the whole square, so the sign (outline included) spans it
   * edge to edge across its wider side, centred.  Its size and centre are
   * measured on a scratch canvas first. */
  float outline = small ? 2.0f : 4.0f;
  float s = (size - 2 * outline) / 124.0f, ox = 0, oy = 0;
  int sw = 3 * size;
  ga_canvas t = { sw, sw, malloc((size_t)sw * sw * 4) };
  if (!t.px) {
    free(c.px);
    return -1;
  }
  for (int pass = 0; pass < 3; pass++) {
    memset(t.px, 0, (size_t)sw * sw * 4);
    if (!ga_draw_warning(&t, sw / 2.0f, sw / 2.0f, s, outline)) {
      free(t.px);
      free(c.px);
      return -1;
    }
    int x0 = sw, y0 = sw, x1 = -1, y1 = -1;
    for (int yy = 0; yy < sw; yy++)
      for (int xx = 0; xx < sw; xx++)
        if (t.px[((size_t)yy * sw + xx) * 4 + 3] > 8) {
          if (xx < x0) x0 = xx;
          if (xx > x1) x1 = xx;
          if (yy < y0) y0 = yy;
          if (yy > y1) y1 = yy;
        }
    if (x1 < 0)
      break;
    float bw = (float)(x1 - x0 + 1), bh = (float)(y1 - y0 + 1), big = bw > bh ? bw : bh;
    ox = sw / 2.0f - (x0 + x1 + 1) / 2.0f; /* sign's centre point -> middle of its box */
    oy = sw / 2.0f - (y0 + y1 + 1) / 2.0f;
    if (pass < 2)
      s *= (size - 2 * outline) / (big - 2 * outline);
  }
  free(t.px);
  if (!ga_draw_warning(&c, pad + size / 2.0f + ox, pad + size / 2.0f + oy, s, outline)) {
    free(c.px);
    return -1;
  }
  /* the border repeats the edge pixels of the button itself */
  for (int yy = 0; yy < tw; yy++)
    for (int xx = 0; xx < tw; xx++) {
      int ix = clampi(xx, pad, pad + size - 1), iy = clampi(yy, pad, pad + size - 1);
      if (ix != xx || iy != yy)
        memcpy(c.px + ((size_t)yy * tw + xx) * 4, c.px + ((size_t)iy * tw + ix) * 4, 4);
    }
  uint16_t *out = malloc((size_t)tw * tw * 2);
  if (!out) {
    free(c.px);
    return -1;
  }
  /* to 4 bits per channel like the sheet, with a little ordered dither */
  static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
  for (int yy = 0; yy < tw; yy++)
    for (int xx = 0; xx < tw; xx++) {
      const uint8_t *p = c.px + ((size_t)yy * tw + xx) * 4;
      float dz = (bayer[yy & 3][xx & 3] / 16.0f - 0.5f) * 0.9f;
      int a = clampi((int)floorf(p[3] / 17.0f + 0.5f), 0, 15), q[3];
      for (int k = 0; k < 3; k++) {
        q[k] = clampi((int)floorf(p[k] / 17.0f + 0.5f + dz), 0, 15);
        if (q[k] > a) q[k] = a;
      }
      out[(size_t)yy * tw + xx] = (uint16_t)(q[0] << 12 | q[1] << 8 | q[2] << 4 | a);
    }
  free(c.px);
  *sheet_w = *sheet_h = small ? 1024 : 2048;
  *x = (unsigned)(mx - pad);
  *y = (unsigned)(my - pad);
  *w = *h = (unsigned)tw;
  *px = out;
  return 0;
}
