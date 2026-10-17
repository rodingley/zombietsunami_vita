/* asset_cache.h -- keeps recently used game files in memory, ready to use.
 *
 * Some menus (the trophy menu above all) load their sprite sheet again every
 * time they are opened: read from the memory card, unzip, upload.  The cache
 * keeps such files in memory after the first time, and for zipped sprite
 * sheets (.jet) it keeps them already unzipped (handed back to the game as a
 * gzip stream that is not compressed, which assets.c then copies straight to
 * the game without running the unzip at all).  So opening the menu again
 * costs almost nothing. */
#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include <stdint.h>

typedef struct cache_entry cache_entry;

void acache_init(void);
/* a cached copy of this asset, or NULL; release it with acache_release */
cache_entry *acache_get(const char *name, const unsigned char **data, int *len);
/* for an entry that is a stored gzip made here (see below): the unzipped size; else 0 */
uint32_t acache_raw_len(const cache_entry *e);
void acache_release(cache_entry *e);
/* hand a buffer (malloc'd) to the cache; it frees it when it is not needed */
void acache_put(const char *name, unsigned char *data, int len);
/* the same for a stored gzip stream (sgz_* below) of raw_len unzipped bytes */
void acache_put_stored(const char *name, unsigned char *gz, int len, uint32_t raw_len);

/* "stored gzip": a gzip stream whose data is not compressed (blocks of up to
 * 65535 bytes with a 5-byte header each).  Any unzip passes it straight
 * through, and we can copy any part of the data out of it directly. */
unsigned char *sgz_alloc(uint32_t raw_len, int *out_len); /* headers written; fill with sgz_write */
void sgz_write(unsigned char *gz, uint32_t raw_off, const unsigned char *src, uint32_t n);
void sgz_read(const unsigned char *gz, uint32_t raw_off, unsigned char *dst, uint32_t n);
void sgz_finish(unsigned char *gz, uint32_t raw_len); /* CRC and size at the end */
void sgz_finish_crc(unsigned char *gz, uint32_t raw_len, uint32_t crc); /* the same, CRC already known */
/* raw unzipped data -> a stored gzip stream, malloc'd */
unsigned char *acache_make_stored_gzip(const unsigned char *raw, uint32_t raw_len, int *out_len);

/* called when the game's loading is done: loads the files the menus need right
 * away (fonts, trophy menu) into the cache in the background */
void acache_prewarm(void);
/* from now on sheets the game unzips are kept (during the loading screen they
 * are not: those load only once) */
void acache_capture_start(void);
int acache_capture_on(void);
/* if the background loader is reading this file right now, wait until it is
 * in memory (quicker than reading it a second time at the same moment) */
void acache_wait_pending(const char *name);

#endif
