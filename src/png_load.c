/* png_load.c -- a small PNG reader (8-bit, not interlaced) on top of the bundled zlib */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "popup.h"
#include "zlib/zlib.h"

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  if (pa <= pb && pa <= pc)
    return a;
  return pb <= pc ? b : c;
}

unsigned char *png_load_rgba(const char *path, int *w_out, int *h_out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *file = malloc(len > 0 ? (size_t)len : 1);
  if (!file || len < 8 || fread(file, 1, (size_t)len, f) != (size_t)len) {
    fclose(f);
    free(file);
    return NULL;
  }
  fclose(f);

  static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
  unsigned char *idat = NULL, *raw = NULL, *out = NULL;
  size_t idat_len = 0;
  uint32_t w = 0, h = 0;
  int depth = 0, ctype = -1, interlace = 0;
  unsigned char pal[256 * 4];
  memset(pal, 255, sizeof(pal));

  if (memcmp(file, sig, 8) != 0)
    goto fail;
  idat = malloc((size_t)len);
  if (!idat)
    goto fail;
  for (long p = 8; p + 12 <= len;) {
    uint32_t n = be32(file + p);
    const unsigned char *type = file + p + 4, *data = file + p + 8;
    if ((long)n > len - p - 12)
      goto fail;
    if (!memcmp(type, "IHDR", 4) && n >= 13) {
      w = be32(data);
      h = be32(data + 4);
      depth = data[8];
      ctype = data[9];
      interlace = data[12];
    } else if (!memcmp(type, "PLTE", 4)) {
      for (uint32_t i = 0; i < n / 3 && i < 256; i++)
        memcpy(pal + i * 4, data + i * 3, 3);
    } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
      for (uint32_t i = 0; i < n && i < 256; i++)
        pal[i * 4 + 3] = data[i];
    } else if (!memcmp(type, "IDAT", 4)) {
      memcpy(idat + idat_len, data, n);
      idat_len += n;
    } else if (!memcmp(type, "IEND", 4)) {
      break;
    }
    p += 12 + n;
  }

  int chans;
  switch (ctype) {
  case 0: chans = 1; break; /* grey */
  case 2: chans = 3; break; /* RGB */
  case 3: chans = 1; break; /* palette */
  case 4: chans = 2; break; /* grey + alpha */
  case 6: chans = 4; break; /* RGBA */
  default: goto fail;
  }
  if (depth != 8 || interlace != 0 || w == 0 || h == 0 || w > 4096 || h > 4096)
    goto fail;

  size_t stride = (size_t)w * chans;
  size_t raw_len = (stride + 1) * h;
  raw = malloc(raw_len);
  out = malloc((size_t)w * h * 4);
  if (!raw || !out)
    goto fail;

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit(&zs) != Z_OK)
    goto fail;
  zs.next_in = idat;
  zs.avail_in = (uInt)idat_len;
  zs.next_out = raw;
  zs.avail_out = (uInt)raw_len;
  int zr = inflate(&zs, Z_FINISH);
  inflateEnd(&zs);
  if (zr != Z_STREAM_END || zs.avail_out != 0)
    goto fail;

  /* undo the per-row filters in place */
  for (uint32_t y = 0; y < h; y++) {
    unsigned char *row = raw + y * (stride + 1);
    int ft = row[0];
    unsigned char *cur = row + 1, *prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
    for (size_t i = 0; i < stride; i++) {
      int a = i >= (size_t)chans ? cur[i - chans] : 0;
      int b = prev ? prev[i] : 0;
      int c = (prev && i >= (size_t)chans) ? prev[i - chans] : 0;
      int v = cur[i];
      switch (ft) {
      case 1: v += a; break;
      case 2: v += b; break;
      case 3: v += (a + b) / 2; break;
      case 4: v += paeth(a, b, c); break;
      default: break;
      }
      cur[i] = (unsigned char)v;
    }
  }

  for (uint32_t y = 0; y < h; y++) {
    const unsigned char *s = raw + y * (stride + 1) + 1;
    unsigned char *d = out + (size_t)y * w * 4;
    for (uint32_t x = 0; x < w; x++, d += 4) {
      switch (ctype) {
      case 0: d[0] = d[1] = d[2] = s[x]; d[3] = 255; break;
      case 2: d[0] = s[x * 3]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3 + 2]; d[3] = 255; break;
      case 3: memcpy(d, pal + s[x] * 4, 4); break;
      case 4: d[0] = d[1] = d[2] = s[x * 2]; d[3] = s[x * 2 + 1]; break;
      case 6: memcpy(d, s + x * 4, 4); break;
      }
    }
  }
  free(file);
  free(idat);
  free(raw);
  *w_out = (int)w;
  *h_out = (int)h;
  return out;

fail:
  free(file);
  free(idat);
  free(raw);
  free(out);
  return NULL;
}
