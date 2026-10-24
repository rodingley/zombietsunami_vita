/* asset_cache.c -- see asset_cache.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "asset_cache.h"
#include "asset_io.h"
#include "config.h"
#include "so_platform.h"
#include "zlib/zlib.h"

#define CACHE_BUDGET (24 * 1024 * 1024)

struct cache_entry {
  char name[96];
  unsigned char *data;
  int len;
  uint32_t raw_len; /* stored gzip: unzipped size */
  int refs;
  uint64_t last_use;
  struct cache_entry *next;
};

static cache_entry *entries;
static long cache_bytes;
static SceKernelLwMutexWork lock;
static int ready;

void acache_init(void) {
  if (sceKernelCreateLwMutex(&lock, "acache", 0, 0, NULL) >= 0)
    ready = 1;
}

cache_entry *acache_get(const char *name, const unsigned char **data, int *len) {
  if (!ready)
    return NULL;
  sceKernelLockLwMutex(&lock, 1, NULL);
  cache_entry *e = entries;
  while (e && strcmp(e->name, name) != 0)
    e = e->next;
  if (e) {
    e->refs++;
    e->last_use = sceKernelGetProcessTimeWide();
    *data = e->data;
    *len = e->len;
  }
  sceKernelUnlockLwMutex(&lock, 1);
  return e;
}

uint32_t acache_raw_len(const cache_entry *e) { return e ? e->raw_len : 0; }

void acache_release(cache_entry *e) {
  if (!e)
    return;
  sceKernelLockLwMutex(&lock, 1, NULL);
  e->refs--;
  sceKernelUnlockLwMutex(&lock, 1);
}

/* drop the least recently used entries nobody is reading until `need` more bytes fit */
static void make_room(long need) {
  while (cache_bytes + need > CACHE_BUDGET) {
    cache_entry **victim = NULL;
    for (cache_entry **pp = &entries; *pp; pp = &(*pp)->next)
      if ((*pp)->refs == 0 && (!victim || (*pp)->last_use < (*victim)->last_use))
        victim = pp;
    if (!victim)
      return;
    cache_entry *e = *victim;
    *victim = e->next;
    cache_bytes -= e->len;
    free(e->data);
    free(e);
  }
}

void acache_put_stored(const char *name, unsigned char *data, int len, uint32_t raw_len) {
  if (!ready || len <= 0 || len > CACHE_BUDGET / 2) {
    free(data);
    return;
  }
  cache_entry *n = calloc(1, sizeof(cache_entry));
  if (!n) {
    free(data);
    return;
  }
  snprintf(n->name, sizeof(n->name), "%s", name);
  n->data = data;
  n->len = len;
  n->raw_len = raw_len;
  n->last_use = sceKernelGetProcessTimeWide();
  sceKernelLockLwMutex(&lock, 1, NULL);
  for (cache_entry *e = entries; e; e = e->next) {
    if (strcmp(e->name, name) == 0) { /* already there (another thread was quicker) */
      sceKernelUnlockLwMutex(&lock, 1);
      free(data);
      free(n);
      return;
    }
  }
  make_room(len);
  if (cache_bytes + len <= CACHE_BUDGET) {
    n->next = entries;
    entries = n;
    cache_bytes += len;
    n = NULL;
  }
  sceKernelUnlockLwMutex(&lock, 1);
  if (n) {
    free(data);
    free(n);
  }
}

void acache_put(const char *name, unsigned char *data, int len) { acache_put_stored(name, data, len, 0); }

/* ---- stored gzip ------------------------------------------------------------ */

#define SGZ_BLOCK 65535u
#define SGZ_HDR 10u

static uint32_t sgz_blocks(uint32_t raw_len) { return raw_len ? (raw_len + SGZ_BLOCK - 1) / SGZ_BLOCK : 1; }
static size_t sgz_pos(uint32_t raw_off) { return SGZ_HDR + (size_t)(raw_off / SGZ_BLOCK) * (SGZ_BLOCK + 5) + 5 + raw_off % SGZ_BLOCK; }

unsigned char *sgz_alloc(uint32_t raw_len, int *out_len) {
  uint32_t nb = sgz_blocks(raw_len);
  size_t total = SGZ_HDR + 5 * (size_t)nb + raw_len + 8;
  unsigned char *g = malloc(total);
  if (!g)
    return NULL;
  static const unsigned char hdr[SGZ_HDR] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
  memcpy(g, hdr, SGZ_HDR);
  for (uint32_t k = 0; k < nb; k++) {
    uint32_t len = raw_len - k * SGZ_BLOCK;
    if (len > SGZ_BLOCK)
      len = SGZ_BLOCK;
    unsigned char *p = g + SGZ_HDR + (size_t)k * (SGZ_BLOCK + 5);
    p[0] = (k == nb - 1);
    p[1] = len & 0xff;
    p[2] = len >> 8;
    p[3] = ~len & 0xff;
    p[4] = (~len >> 8) & 0xff;
  }
  *out_len = (int)total;
  return g;
}

void sgz_write(unsigned char *gz, uint32_t raw_off, const unsigned char *src, uint32_t n) {
  while (n) {
    uint32_t room = SGZ_BLOCK - raw_off % SGZ_BLOCK, c = n < room ? n : room;
    memcpy(gz + sgz_pos(raw_off), src, c);
    raw_off += c;
    src += c;
    n -= c;
  }
}

void sgz_read(const unsigned char *gz, uint32_t raw_off, unsigned char *dst, uint32_t n) {
  while (n) {
    uint32_t room = SGZ_BLOCK - raw_off % SGZ_BLOCK, c = n < room ? n : room;
    memcpy(dst, gz + sgz_pos(raw_off), c);
    raw_off += c;
    dst += c;
    n -= c;
  }
}

void sgz_finish_crc(unsigned char *gz, uint32_t raw_len, uint32_t crc) {
  unsigned char *p = gz + SGZ_HDR + 5 * (size_t)sgz_blocks(raw_len) + raw_len;
  for (int i = 0; i < 4; i++)
    p[i] = (unsigned char)(crc >> (8 * i));
  for (int i = 0; i < 4; i++)
    p[4 + i] = (unsigned char)(raw_len >> (8 * i));
}

void sgz_finish(unsigned char *gz, uint32_t raw_len) {
  uLong crc = crc32(0L, Z_NULL, 0);
  for (uint32_t off = 0; off < raw_len; off += SGZ_BLOCK) {
    uint32_t c = raw_len - off < SGZ_BLOCK ? raw_len - off : SGZ_BLOCK;
    crc = crc32(crc, gz + sgz_pos(off), c);
  }
  sgz_finish_crc(gz, raw_len, (uint32_t)crc);
}

unsigned char *acache_make_stored_gzip(const unsigned char *raw, uint32_t raw_len, int *out_len) {
  unsigned char *g = sgz_alloc(raw_len, out_len);
  if (!g)
    return NULL;
  sgz_write(g, 0, raw, raw_len);
  sgz_finish(g, raw_len);
  return g;
}

/* ---- prewarm: files the menus need soon after the loading screen ------------ */

static const struct {
  const char *name;
  int unzip; /* a zipped sheet: keep it unzipped */
} prewarm_files[] = {
  /* in the order the game asks for them as the main menu first appears */
  { "res/com/gfx/font/BerlinSansFBDemi-Bold.ttf", 0 }, /* menu text, read every time the main menu shows */
  { "res/com/gfx/font/MicrosoftYaHei.ttf", 0 },        /* 3.7 MB: took ~0.4 s right as the menu appeared */
  { "res/zombies/com/gfx/sprites/hall.spr", 0 },       /* trophy menu */
  { "res/zombies/com/gfx/sprites/hall_16bpp_pvrgz.jet", 1 },
};

static char pending[128]; /* the file the background loader is reading now */
static volatile int pending_on;

void acache_wait_pending(const char *name) {
  for (int i = 0; i < 1500 && pending_on; i++) { /* 3 s at most */
    sceKernelLockLwMutex(&lock, 1, NULL);
    int same = pending_on && strcmp(pending, name) == 0;
    sceKernelUnlockLwMutex(&lock, 1);
    if (!same)
      return;
    sceKernelDelayThread(2000);
  }
}

static void set_pending(const char *name) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  if (name)
    snprintf(pending, sizeof(pending), "%s", name);
  pending_on = name != NULL;
  sceKernelUnlockLwMutex(&lock, 1);
}

static unsigned char *read_whole(const char *path, int *len) {
  aio_t io;
  if (aio_open(&io, path) < 0)
    return NULL;
  unsigned char *d = malloc(io.len > 0 ? (size_t)io.len : 1);
  int n = d ? aio_read(&io, d, (int)io.len) : -1;
  aio_close(&io);
  if (n != (int)io.len) {
    free(d);
    return NULL;
  }
  *len = n;
  return d;
}

/* zipped file -> stored gzip, unzipped straight into place */
static unsigned char *unzip_to_stored(const unsigned char *gz, int gz_len, int *out_len, uint32_t *raw_len) {
  if (gz_len < 18 || gz[0] != 0x1f || gz[1] != 0x8b)
    return NULL;
  uint32_t isize = gz[gz_len - 4] | gz[gz_len - 3] << 8 | gz[gz_len - 2] << 16 | (uint32_t)gz[gz_len - 1] << 24;
  if (isize == 0 || isize > 16u * 1024 * 1024)
    return NULL;
  unsigned char *g = sgz_alloc(isize, out_len);
  unsigned char *chunk = malloc(SGZ_BLOCK);
  int ok = 0;
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (g && chunk && inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK) {
    zs.next_in = (Bytef *)gz;
    zs.avail_in = (uInt)gz_len;
    uint32_t done = 0;
    int r = Z_OK;
    while (r == Z_OK) {
      zs.next_out = chunk;
      zs.avail_out = SGZ_BLOCK;
      r = inflate(&zs, Z_NO_FLUSH);
      uint32_t n = SGZ_BLOCK - zs.avail_out;
      if (done + n > isize) {
        r = Z_DATA_ERROR;
        break;
      }
      sgz_write(g, done, chunk, n);
      done += n;
    }
    ok = r == Z_STREAM_END && done == isize;
    inflateEnd(&zs);
  }
  free(chunk);
  if (!ok) {
    free(g);
    return NULL;
  }
  sgz_finish(g, isize);
  *raw_len = isize;
  return g;
}

static int prewarm_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int count = 0;
  for (unsigned i = 0; i < sizeof(prewarm_files) / sizeof(prewarm_files[0]); i++) {
    const char *name = prewarm_files[i].name;
    const unsigned char *d;
    int dl;
    cache_entry *e = acache_get(name, &d, &dl);
    if (e) { /* the game already loaded it */
      acache_release(e);
      continue;
    }
    set_pending(name);
    char path[512];
    snprintf(path, sizeof(path), ASSET_PATH "/%s", name);
    int len = 0;
    unsigned char *file = read_whole(path, &len);
    if (file && prewarm_files[i].unzip) {
      int glen = 0;
      uint32_t raw = 0;
      unsigned char *g = unzip_to_stored(file, len, &glen, &raw);
      free(file);
      if (g)
        acache_put_stored(name, g, glen, raw), count++;
    } else if (file) {
      acache_put(name, file, len);
      count++;
    }
    set_pending(NULL);
  }
  so_log("[load] ready in memory in the background: %d file(s) for the menus (%u ms)", count,
         (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
  return sceKernelExitDeleteThread(0);
}

static volatile int capture_on;
int acache_capture_on(void) { return capture_on; }
void acache_capture_start(void) { capture_on = 1; }

void acache_prewarm(void) {
  static int started;
  if (started || !ready)
    return;
  started = 1;
  /* a little below the game's priority: it uses the time the game leaves free */
  SceUID t = sceKernelCreateThread("prewarm", prewarm_thread, 0x10000100 + 20, 0x10000, 0, 0, NULL);
  if (t >= 0)
    sceKernelStartThread(t, 0, NULL);
}
