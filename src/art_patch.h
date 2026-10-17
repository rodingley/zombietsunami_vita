/* art_patch.h -- see art_patch.c */
#ifndef ART_PATCH_H
#define ART_PATCH_H

#include <stdint.h>

typedef struct art_stream art_stream;

/* the game opens this asset: a handle if we patch it, else NULL */
art_stream *art_stream_begin(const char *asset);
/* every piece of unzipped data, in order, right after zlib produced it */
void art_stream_output(art_stream *st, unsigned char *out, uint32_t n);
void art_stream_end(art_stream *st);
/* get the new pictures ready (main thread, at start-up, before the game loads) */
void art_patch_prepare(void);

#endif
