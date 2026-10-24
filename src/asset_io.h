/* asset_io.h -- reading the game's asset files from the memory card.
 *
 * Our own small buffered reader on top of the system's file calls (no C
 * library stdio): reads of a few bytes are served from a 64 KB block that is
 * read in one go; big reads go straight from the card into the game's memory.
 * Seeking only moves a counter, nothing is read until it is needed. */
#ifndef ASSET_IO_H
#define ASSET_IO_H

#define AIO_BUF_SIZE (64 * 1024)

typedef struct {
  int fd;
  long len;            /* file size */
  long pos;            /* where the game is reading */
  long fd_pos;         /* where the system's file offset is */
  unsigned char *buf;  /* AIO_BUF_SIZE bytes (may be NULL if out of memory: then no buffering) */
  long buf_off;        /* file offset of buf[0] */
  int buf_len;         /* valid bytes in buf */
} aio_t;

int aio_open(aio_t *a, const char *path); /* 0 = ok, -1 = could not open */
int aio_read(aio_t *a, void *dst, int n);  /* bytes read (0 at the end of the file) */
long aio_seek(aio_t *a, long off, int whence); /* new position, or -1 */
void aio_close(aio_t *a);

#endif
