/* assets.h -- the game's data files (Android's AAsset API), and what we do
 * while the game unzips them: put our own pictures into sprite sheets
 * (art_patch.c), keep unzipped sheets for next time (asset_cache.c), and hand
 * kept sheets back without unzipping again. */
#ifndef ASSETS_H
#define ASSETS_H

#include <stddef.h>
#include <stdint.h>
#include "zlib/zlib.h"

void assets_init(void);

void *assets_open(const char *filename);
void assets_close(void *asset);
int assets_read(void *asset, void *buf, size_t n);
int assets_seek(void *asset, int off, int whence);
int assets_length(void *asset);

/* the game's zlib calls go through these */
int assets_inflate_init2(z_streamp strm, int window_bits, const char *version, int stream_size);
int assets_inflate(z_streamp strm, int flush);
int assets_inflate_end(z_streamp strm);

/* the files the game thread opened since the last call, for the slow-frame log
 * line: writes e.g. " -- read 2 file(s) in 380 ms (slowest MicrosoftYaHei.ttf
 * 371 ms), unzip 12 ms" into out ("" if nothing), and starts counting afresh */
void assets_frame_report(char *out, size_t n);
extern volatile int g_game_thread; /* set by the frame loop */

/* time spent loading, added up over all threads (shown in the [fps] lines) */
extern volatile uint64_t g_load_read_us, g_load_unzip_us;

#endif
