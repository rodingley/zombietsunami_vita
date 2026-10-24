/* assets.c -- see assets.h
 *
 * How the game loads a sprite sheet (.jet = gzipped PVR texture): it opens the
 * file, reads the unzipped size from its last 4 bytes, then reads 16 KB pieces
 * into a buffer and lets zlib unzip each into a 16 KB buffer on its stack,
 * which it copies on into the texture's memory.  We see every one of those
 * calls, and work out which open file an unzip belongs to from the buffer it
 * reads from (the game's last read went there), or else from the file's first
 * bytes.  Nothing here depends on which thread does what. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "art_patch.h"
#include "asset_cache.h"
#include "asset_io.h"
#include "assets.h"
#include "config.h"
#include "so_platform.h"

typedef struct {
  aio_t io;           /* the file on the memory card (asset_io.c), unless ... */
  unsigned char *mem; /* ... it is served from memory: */
  cache_entry *ce;    /*   a copy kept in the cache (asset_cache.c), or */
  int cache_on_close; /*   a small file read in one go (mem is ours; handed to the cache at close) */
  long pos;
  int len;
  /* the game's unzip of this file */
  int tracked;            /* in the list the unzip calls look at */
  z_streamp strm;         /* the game's unzip, once we know which one it is */
  int strm_done;          /* that unzip is over */
  const void *last_buf;   /* where the game's last read of this file put the data */
  unsigned char head[32]; /* the file's first bytes */
  int head_len;
  art_stream *art;        /* our pictures go into this sheet as it is unzipped */
  int cap_on, cap_failed, cap_done, cap_crc_ok;
  uint32_t cap_isize, cap_len, cap_crc;
  unsigned char *cap_gz;  /* what comes out of the unzip, kept as a stored gzip */
  int cap_gz_len;
  int fast;               /* kept unzipped in the cache: copied straight out, no unzip */
  uint32_t fast_len, fast_pos;
  /* timing, to find what makes menus / runs stutter (the [load] log lines) */
  uint64_t t_open, read_us, unzip_at_open, upload_at_open;
  char name[48];
  char key[128];
} asset_t;

volatile uint64_t g_load_read_us, g_load_unzip_us;
extern volatile uint64_t g_load_upload_us; /* gl_real.c */
volatile int g_game_thread;
int assets_fast_streams; /* unzips skipped so far (for the host test) */

#define MAX_TRACKED 16
#define CAPTURE_MAX_SIZE (4 * 1024 * 1024 + 1024) /* sheets up to 1024x2048 (bigger ones load once only) */
#define SMALL_FILE_MAX (1024 * 1024)
static asset_t *tracked[MAX_TRACKED];
static volatile int tracked_count;
static SceKernelLwMutexWork track_lock;
static int file_log_count;

/* what the game thread opened during the current frame */
static uint64_t fr_files_us, fr_unzip_us, fr_slow_us;
static int fr_files;
static char fr_slow[48];

void assets_init(void) {
  sceKernelCreateLwMutex(&track_lock, "assets", 0, 0, NULL);
  acache_init();
}

static int ends_with(const char *s, const char *tail) {
  size_t a = strlen(s), b = strlen(tail);
  return a >= b && strcmp(s + a - b, tail) == 0;
}

static void track(asset_t *a, int on) {
  sceKernelLockLwMutex(&track_lock, 1, NULL);
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (on && !tracked[i]) {
      tracked[i] = a;
      tracked_count++;
      a->tracked = 1;
      break;
    }
    if (!on && tracked[i] == a) {
      tracked[i] = NULL;
      tracked_count--;
      a->tracked = 0;
      break;
    }
  }
  sceKernelUnlockLwMutex(&track_lock, 1);
}

void *assets_open(const char *filename) {
  char path[512];
  snprintf(path, sizeof(path), ASSET_PATH "/%s", filename);
  asset_t *a = calloc(1, sizeof(asset_t));
  if (!a)
    return NULL;
  a->io.fd = -1;
  a->t_open = sceKernelGetProcessTimeWide();
  a->unzip_at_open = g_load_unzip_us;
  a->upload_at_open = g_load_upload_us;
  const char *base = strrchr(filename, '/');
  snprintf(a->name, sizeof(a->name), "%s", base ? base + 1 : filename);
  snprintf(a->key, sizeof(a->key), "%s", filename);

  /* 1) kept in memory from an earlier time (or being loaded by the background loader right now) */
  acache_wait_pending(filename);
  const unsigned char *cd;
  int clen;
  if ((a->ce = acache_get(filename, &cd, &clen))) {
    a->mem = (unsigned char *)cd;
    a->len = clen;
    uint32_t raw = acache_raw_len(a->ce);
    if (raw && ends_with(filename, ".jet")) { /* kept unzipped: the unzip can be skipped */
      a->fast = 1;
      a->fast_len = raw;
      track(a, 1);
    }
    return a;
  }
  if (aio_open(&a->io, path) < 0) {
    so_log("asset not found: %s", filename);
    free(a);
    return NULL;
  }
  a->len = (int)a->io.len;
  if (file_log_count++ < 800)
    so_log("[file] asset \"%s\" (%d bytes)", filename, a->len);

  /* 2) small files the game reads again and again (sprite lists, fonts, and
   * text.bin, all the game's text, which the screens after a run re-read up to
   * ~50 times in a row): one read, kept */
  if ((ends_with(filename, ".spr") || ends_with(filename, ".ttf") || ends_with(filename, ".bin")) && a->len > 0 &&
      a->len <= SMALL_FILE_MAX) {
    unsigned char *m = malloc((size_t)a->len);
    uint64_t t = sceKernelGetProcessTimeWide();
    int got = m ? aio_read(&a->io, m, a->len) : -1;
    t = sceKernelGetProcessTimeWide() - t;
    a->read_us += t;
    g_load_read_us += t;
    if (got == a->len) {
      aio_close(&a->io);
      a->mem = m;
      a->cache_on_close = 1;
      return a;
    }
    free(m);
    aio_seek(&a->io, 0, SEEK_SET);
  }

  /* 3) zipped sprite sheets: our pictures go in while it is unzipped, and
   * (after the loading screen) what comes out is kept for next time */
  a->art = art_stream_begin(filename);
  a->cap_on = ends_with(filename, "_pvrgz.jet") && acache_capture_on();
  if (a->art || a->cap_on)
    track(a, 1);
  return a;
}

/* a read of n bytes at file offset pos went to buf */
static void note_read(asset_t *a, long pos, const unsigned char *buf, int n) {
  a->last_buf = buf;
  if (pos == 0 && !a->head_len && !a->fast) {
    a->head_len = n < (int)sizeof(a->head) ? n : (int)sizeof(a->head);
    memcpy(a->head, buf, (size_t)a->head_len);
  }
  /* the game reads the unzipped size (the last 4 bytes) before it unzips */
  if (a->cap_on && !a->cap_isize && pos <= a->len - 4 && pos + n >= a->len) {
    const unsigned char *t = buf + (a->len - 4 - pos);
    a->cap_isize = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
    if (a->cap_isize == 0 || a->cap_isize > CAPTURE_MAX_SIZE)
      a->cap_failed = 1;
  }
}

int assets_read(void *asset, void *buf, size_t n) {
  asset_t *a = (asset_t *)asset;
  if (a->mem) {
    long pos = a->pos, left = a->len - a->pos;
    if ((long)n > left)
      n = left > 0 ? (size_t)left : 0;
    memcpy(buf, a->mem + a->pos, n);
    a->pos += (long)n;
    if (a->tracked && n)
      note_read(a, pos, buf, (int)n);
    return (int)n;
  }
  long pos = a->io.pos;
  uint64_t t = sceKernelGetProcessTimeWide();
  int r = aio_read(&a->io, buf, (int)n);
  t = sceKernelGetProcessTimeWide() - t;
  a->read_us += t;
  g_load_read_us += t;
  if (a->tracked && r > 0)
    note_read(a, pos, buf, r);
  return r;
}

int assets_seek(void *asset, int off, int whence) {
  asset_t *a = (asset_t *)asset;
  if (a->mem) {
    long p = whence == SEEK_SET ? off : whence == SEEK_CUR ? a->pos + off : whence == SEEK_END ? a->len + off : -1;
    if (p < 0 || p > a->len)
      return -1;
    a->pos = p;
    return (int)p;
  }
  return (int)aio_seek(&a->io, off, whence);
}

int assets_length(void *asset) { return ((asset_t *)asset)->len; }

void assets_close(void *asset) {
  asset_t *a = (asset_t *)asset;
  if (!a)
    return;
  uint64_t total = sceKernelGetProcessTimeWide() - a->t_open;
  if (total >= 30000) /* only the slow ones */
    so_log("[load] %s (%d KB): %u ms open, of which reading %u ms; unzip %u ms, to GPU %u ms meanwhile%s", a->name,
           a->len / 1024, (unsigned)(total / 1000), (unsigned)(a->read_us / 1000),
           (unsigned)((g_load_unzip_us - a->unzip_at_open) / 1000),
           (unsigned)((g_load_upload_us - a->upload_at_open) / 1000), a->ce ? " (from memory)" : "");
  if (g_game_thread && sceKernelGetThreadId() == g_game_thread) {
    fr_files++;
    fr_files_us += total;
    if (total > fr_slow_us) {
      fr_slow_us = total;
      snprintf(fr_slow, sizeof(fr_slow), "%s", a->name);
    }
  }
  if (a->tracked)
    track(a, 0);
  if (a->art && a->head_len && !a->strm && !a->strm_done)
    so_log("[art] %s was read, but its unzip was not recognised: picture not put in", a->name);
  art_stream_end(a->art);
  if (a->cap_gz) {
    if (a->cap_done && !a->cap_failed && a->cap_len == a->cap_isize) {
      if (a->cap_crc_ok)
        sgz_finish_crc(a->cap_gz, a->cap_isize, a->cap_crc);
      else
        sgz_finish(a->cap_gz, a->cap_isize);
      acache_put_stored(a->key, a->cap_gz, a->cap_gz_len, a->cap_isize);
    } else {
      free(a->cap_gz);
    }
  }
  if (a->ce)
    acache_release(a->ce);
  else if (a->mem && a->cache_on_close)
    acache_put(a->key, a->mem, a->len);
  else if (a->mem)
    free(a->mem);
  else
    aio_close(&a->io);
  free(a);
}

/* ---- the game's unzip ------------------------------------------------------- */

/* forget any file this unzip was linked to (it is starting afresh, or ending) */
static void drop_stream(z_streamp strm) {
  if (!tracked_count)
    return;
  sceKernelLockLwMutex(&track_lock, 1, NULL);
  for (int i = 0; i < MAX_TRACKED; i++)
    if (tracked[i] && tracked[i]->strm == strm) {
      tracked[i]->strm = NULL;
      tracked[i]->strm_done = 1;
    }
  sceKernelUnlockLwMutex(&track_lock, 1);
}

/* which open file does this unzip read? */
static asset_t *find_stream(z_streamp strm) {
  asset_t *a = NULL, *by_head = NULL;
  sceKernelLockLwMutex(&track_lock, 1, NULL);
  for (int i = 0; i < MAX_TRACKED && !a; i++)
    if (tracked[i] && tracked[i]->strm == strm)
      a = tracked[i];
  if (!a && strm->total_in == 0 && strm->total_out == 0 && strm->next_in && strm->avail_in) {
    /* a new unzip: the game points it at the buffer its last read of the file filled */
    for (int i = 0; i < MAX_TRACKED; i++) {
      asset_t *c = tracked[i];
      if (!c || c->strm || c->strm_done)
        continue;
      if (c->last_buf == strm->next_in) {
        if (!a || c->t_open > a->t_open)
          a = c;
      } else if (c->head_len >= 16 && strm->avail_in >= 16) {
        unsigned k = strm->avail_in < (unsigned)c->head_len ? strm->avail_in : (unsigned)c->head_len;
        if (memcmp(c->head, strm->next_in, k) == 0 && (!by_head || c->t_open > by_head->t_open))
          by_head = c;
      }
    }
    if (!a)
      a = by_head;
    if (a)
      a->strm = strm;
  }
  sceKernelUnlockLwMutex(&track_lock, 1);
  return a;
}

/* a sheet kept unzipped: give the game the next part of it as if zlib had
 * unzipped it (the game only looks at what came out and at the result code) */
static int fast_inflate(asset_t *a, z_streamp strm) {
  uint32_t left = a->fast_len - a->fast_pos, n = left < strm->avail_out ? left : strm->avail_out;
  if (n) {
    sgz_read(a->mem, a->fast_pos, strm->next_out, n);
    a->fast_pos += n;
    strm->next_out += n;
    strm->avail_out -= n;
    strm->total_out += n;
  }
  if (a->fast_pos < a->fast_len)
    return n ? Z_OK : Z_BUF_ERROR;
  /* everything is out: all the input counts as used */
  strm->total_in += strm->avail_in;
  strm->next_in += strm->avail_in;
  strm->avail_in = 0;
  return Z_STREAM_END;
}

static void got_output(asset_t *a, z_streamp strm, unsigned char *out, uint32_t n, int r) {
  if (a->art)
    art_stream_output(a->art, out, n);
  if (a->cap_on && !a->cap_failed && n) {
    if (!a->cap_isize)
      a->cap_failed = 1; /* the size was not read first: cannot keep it */
    else if (!a->cap_gz && !(a->cap_gz = sgz_alloc(a->cap_isize, &a->cap_gz_len)))
      a->cap_failed = 1;
    else if (a->cap_len + n > a->cap_isize)
      a->cap_failed = 1;
    else {
      sgz_write(a->cap_gz, a->cap_len, out, n);
      a->cap_len += n;
    }
  }
  if (r == Z_STREAM_END) {
    a->cap_done = 1;
    /* zlib checked the data against its CRC; that CRC fits the copy unless we drew into it */
    a->cap_crc = (uint32_t)strm->adler;
    a->cap_crc_ok = !a->art;
  }
}

int assets_inflate(z_streamp strm, int flush) {
  uint64_t t = sceKernelGetProcessTimeWide();
  asset_t *a = tracked_count ? find_stream(strm) : NULL;
  int r;
  if (a && a->fast) {
    if (a->fast_pos == 0)
      assets_fast_streams++;
    r = fast_inflate(a, strm);
  } else {
    Bytef *before = strm->next_out;
    r = inflate(strm, flush);
    if (a)
      got_output(a, strm, before, (uint32_t)(strm->next_out - before), r);
  }
  uint64_t dt = sceKernelGetProcessTimeWide() - t;
  g_load_unzip_us += dt;
  if (g_game_thread && sceKernelGetThreadId() == g_game_thread)
    fr_unzip_us += dt;
  return r;
}

int assets_inflate_init2(z_streamp strm, int window_bits, const char *version, int stream_size) {
  drop_stream(strm);
  return inflateInit2_(strm, window_bits, version, stream_size);
}

int assets_inflate_end(z_streamp strm) {
  drop_stream(strm);
  return inflateEnd(strm);
}

void assets_frame_report(char *out, size_t n) {
  if (out && n) {
    out[0] = '\0';
    size_t used = 0;
    if (fr_files)
      used += (size_t)snprintf(out, n, " -- it read %d file(s) in %u ms (slowest %s, %u ms)", fr_files,
                               (unsigned)(fr_files_us / 1000), fr_slow, (unsigned)(fr_slow_us / 1000));
    if (fr_unzip_us >= 1000 && used < n)
      snprintf(out + used, n - used, "%s unzip %u ms", fr_files ? "," : " --", (unsigned)(fr_unzip_us / 1000));
  }
  fr_files = 0;
  fr_files_us = fr_unzip_us = fr_slow_us = 0;
  fr_slow[0] = '\0';
}
