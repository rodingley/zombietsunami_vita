/* art_patch.c -- pictures drawn over a few of the game's sprites.
 *
 * Used for the main menu's "report bugs" button: the game's own warning sign
 * (cut out of the game's hud sheet by game_art.c) in place of the Bluesky icon.
 * The game unzips its sprite sheets (.jet = gzipped PVR texture) with zlib;
 * imports.c hands us every piece that comes out of the unzip for the sheets we
 * patch, in order, straight after zlib wrote it and before the game uses it.
 * We overwrite the bytes of the old picture's rectangle with ours.  Nothing on
 * the memory card changes, nothing extra is unzipped, and it does not matter
 * how the game later sends the sheet to the GPU.
 * (zlib keeps its own copy of what it already produced, so changing the
 * output afterwards cannot disturb the rest of the unzip.)
 *
 * The new pixels are RGBA4444 premultiplied, like the sheet. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_patch.h"
#include "game_art.h"
#include "so_platform.h"

typedef struct {
  unsigned x, y, w, h;
  const uint16_t *px;
} art_rect;

typedef struct {
  const char *asset;
  int small; /* the small (320) sheet */
  unsigned sheet_w, sheet_h, count;
  art_rect rect;
  uint16_t *px;
  int tried, done;
} art_sheet;

static art_sheet sheets[] = {
  { "res/zombies/com/gfx/sprites/menu_16bpp_pvrgz.jet", 0, 0, 0, 0, { 0, 0, 0, 0, NULL }, NULL, 0, 0 },
  { "res/zombies/com/gfx/sprites/menu_320_16bpp_pvrgz.jet", 1, 0, 0, 0, { 0, 0, 0, 0, NULL }, NULL, 0, 0 },
};
#define NUM_SHEETS (sizeof(sheets) / sizeof(sheets[0]))

struct art_stream {
  art_sheet *s;
  uint32_t off;            /* bytes of unzipped data seen so far */
  unsigned char hdr[52];   /* the PVR header, collected as it goes past */
  int ok;                  /* header checked and matching: patching is on */
  int bad;                 /* header did not match: leave this sheet alone */
  unsigned written;        /* pixels rows written */
};

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the warning sign's pixels for this sheet, made once (game_art.c reads the
 * pieces from the game's own hud sheet; nothing is shipped) */
static void load_sheet(art_sheet *s) {
  s->tried = 1;
  unsigned x, y, w, h;
  if (ga_menu_icon(s->small, &s->sheet_w, &s->sheet_h, &x, &y, &w, &h, &s->px) < 0) {
    so_log("[art] menu button: the game's warning sign could not be read, the button keeps its old picture");
    return;
  }
  s->rect.x = x;
  s->rect.y = y;
  s->rect.w = w;
  s->rect.h = h;
  s->rect.px = s->px;
  s->count = 1;
  so_log("[art] menu button: warning sign ready for %s (%ux%u at %u,%u)", s->small ? "the small sheet" : "the big sheet",
         w, h, x, y);
}

/* made on the main thread at start-up, before the game can open the sheet */
void art_patch_prepare(void) {
  for (unsigned i = 0; i < NUM_SHEETS; i++)
    if (!sheets[i].tried)
      load_sheet(&sheets[i]);
}

art_stream *art_stream_begin(const char *asset) {
  for (unsigned i = 0; i < NUM_SHEETS; i++) {
    art_sheet *s = &sheets[i];
    if (strcmp(asset, s->asset) != 0)
      continue;
    if (!s->tried)
      load_sheet(s);
    if (!s->count)
      return NULL;
    art_stream *st = calloc(1, sizeof(art_stream));
    if (st)
      st->s = s;
    return st;
  }
  return NULL;
}

/* `out` holds unzipped bytes [st->off, st->off + n) of the sheet file */
void art_stream_output(art_stream *st, unsigned char *out, uint32_t n) {
  if (!st || st->bad || !n)
    return;
  uint32_t start = st->off, end = st->off + n;
  st->off = end;
  /* collect and check the PVR header: v3, RGBA4444, the size the patch was made for, no metadata */
  if (!st->ok) {
    for (uint32_t p = start; p < end && p < 52; p++)
      st->hdr[p] = out[p - start];
    if (end < 52)
      return;
    const unsigned char *h = st->hdr;
    if (memcmp(h, "PVR\3", 4) || rd32(h + 8) != 0x61626772u || rd32(h + 12) != 0x04040404u ||
        rd32(h + 24) != st->s->sheet_h || rd32(h + 28) != st->s->sheet_w || rd32(h + 48) != 0) {
      st->bad = 1;
      so_log("[art] %s: not the expected picture sheet, left alone", st->s->asset);
      return;
    }
    st->ok = 1;
  }
  /* write every row of every rectangle that overlaps this piece */
  const uint32_t data_off = 52, w2 = st->s->sheet_w * 2;
  for (unsigned r = 0; r < st->s->count; r++) {
    const art_rect *a = &st->s->rect;
    for (unsigned row = 0; row < a->h; row++) {
      uint32_t rs = data_off + (a->y + row) * w2 + a->x * 2, re = rs + a->w * 2;
      if (re <= start || rs >= end)
        continue;
      uint32_t from = rs > start ? rs : start, to = re < end ? re : end;
      memcpy(out + (from - start), (const unsigned char *)(a->px + row * a->w) + (from - rs), to - from);
      if (to == re)
        st->written++;
    }
  }
}

void art_stream_end(art_stream *st) {
  if (!st)
    return;
  unsigned rows = 0;
  for (unsigned r = 0; r < st->s->count; r++)
    rows += st->s->rect.h;
  if (st->ok && st->written >= rows && !st->s->done) {
    st->s->done = 1;
    so_log("[art] %s: picture replaced", st->s->asset);
  } else if (st->ok && st->written < rows) {
    so_log("[art] %s: picture only partly put in (%u of %u rows)", st->s->asset, st->written, rows);
  }
  free(st);
}
