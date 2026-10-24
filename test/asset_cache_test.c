/* asset_cache_test.c -- host test for src/asset_cache.c: stored-gzip streams
 * decode back to the same bytes with any zlib, and the cache's budget/LRU.
 *   cc -DHOST_TEST -Isrc -Itest/host_include test/asset_cache_test.c src/asset_cache.c src/asset_io.c -lz */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "asset_cache.h"

/* stand-ins for the Vita calls asset_cache.c uses */
static unsigned long long fake_time;
unsigned long long sceKernelGetProcessTimeWide(void) { return fake_time += 1000; }
int sceKernelCreateLwMutex(void *w, const char *n, unsigned a, int c, const void *o) { (void)w; (void)n; (void)a; (void)c; (void)o; return 0; }
int sceKernelLockLwMutex(void *w, int c, unsigned *t) { (void)w; (void)c; (void)t; return 0; }
int sceKernelUnlockLwMutex(void *w, int c) { (void)w; (void)c; return 0; }
int sceKernelCreateThread(const char *n, int (*e)(unsigned, void *), int p, unsigned s, unsigned a, int m, const void *o) { (void)n; (void)e; (void)p; (void)s; (void)a; (void)m; (void)o; return -1; }
int sceKernelStartThread(int t, unsigned l, void *a) { (void)t; (void)l; (void)a; return -1; }
int sceKernelExitDeleteThread(int s) { (void)s; return 0; }
int sceKernelDelayThread(unsigned us) { (void)us; return 0; }
void so_log(const char *fmt, ...) { (void)fmt; }

int main(void) {
  int bad = 0;
  acache_init();
  /* 1) stored gzip of several sizes decodes back exactly (checks headers, blocks, CRC, length) */
  unsigned sizes[] = { 1, 100, 65535, 65536, 131070, 131071, 2097152 + 52, 4194356 };
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    unsigned n = sizes[i];
    unsigned char *raw = malloc(n), *back = malloc(n + 16);
    for (unsigned k = 0; k < n; k++) raw[k] = (unsigned char)(k * 2654435761u >> 13);
    int glen = 0;
    unsigned char *g = acache_make_stored_gzip(raw, n, &glen);
    z_stream zs; memset(&zs, 0, sizeof(zs));
    inflateInit2(&zs, 16 + MAX_WBITS);
    zs.next_in = g; zs.avail_in = glen; zs.next_out = back; zs.avail_out = n + 16;
    int r = inflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END || zs.total_out != n || memcmp(raw, back, n)) { printf("stored gzip %u FAILED (r=%d)\n", n, r); bad++; }
    /* also in small pieces, the way a game may feed it */
    inflateReset(&zs); memset(back, 0, n);
    zs.next_in = g; zs.avail_in = 0; zs.next_out = back; zs.avail_out = 0; r = Z_OK;
    unsigned in_left = glen;
    while (r == Z_OK || r == Z_BUF_ERROR) {
      if (!zs.avail_in && in_left) { unsigned c = in_left < 4000 ? in_left : 4000; zs.avail_in = c; in_left -= c; }
      if (!zs.avail_out) zs.avail_out = (back + n + 16) - zs.next_out < 7000 ? (back + n + 16) - zs.next_out : 7000;
      r = inflate(&zs, Z_NO_FLUSH);
      if (r == Z_BUF_ERROR && !in_left && !zs.avail_in) break;
    }
    if (r != Z_STREAM_END || zs.total_out != n || memcmp(raw, back, n)) { printf("stored gzip %u (pieces) FAILED (r=%d)\n", n, r); bad++; }
    inflateEnd(&zs);
    free(g); free(raw); free(back);
  }
  /* 2) cache: get/put, refs keep entries alive, LRU eviction past the 24 MB budget */
  const unsigned char *d; int len;
  if (acache_get("a", &d, &len)) { printf("empty get FAILED\n"); bad++; }
  for (int i = 0; i < 6; i++) { char nm[8]; snprintf(nm, 8, "f%d", i); acache_put(nm, calloc(1, 5 << 20), 5 << 20); }
  int present = 0;
  for (int i = 0; i < 6; i++) { char nm[8]; snprintf(nm, 8, "f%d", i); cache_entry *e = acache_get(nm, &d, &len); if (e) { present++; acache_release(e); } }
  if (present != 4) { printf("budget FAILED (%d of 6 x 5 MB kept, want 4)\n", present); bad++; }
  cache_entry *held = acache_get("f5", &d, &len); /* in use: must survive */
  for (int i = 6; i < 12; i++) { char nm[8]; snprintf(nm, 8, "f%d", i); acache_put(nm, calloc(1, 5 << 20), 5 << 20); }
  cache_entry *still = acache_get("f5", &d, &len);
  if (!held || !still || len != 5 << 20) { printf("in-use entry evicted FAILED\n"); bad++; }
  acache_release(still); acache_release(held);
  printf(bad ? "FAILED\n" : "ALL OK\n");
  return bad != 0;
}
