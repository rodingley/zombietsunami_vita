/* asset_io.c -- see asset_io.h */

#include <stdlib.h>
#include <string.h>

#include "asset_io.h"

#ifdef HOST_TEST /* built on a computer for the unit test in test/asset_io_test.c */
#include <fcntl.h>
#include <unistd.h>
#define IO_OPEN(p) open((p), O_RDONLY)
#define IO_READ(fd, b, n) ((int)read((fd), (b), (n)))
#define IO_SEEK(fd, o, w) ((long)lseek((fd), (o), (w)))
#define IO_CLOSE(fd) close(fd)
#else
#include <psp2/io/fcntl.h>
#define IO_OPEN(p) sceIoOpen((p), SCE_O_RDONLY, 0)
#define IO_READ(fd, b, n) sceIoRead((fd), (b), (n))
#define IO_SEEK(fd, o, w) ((long)sceIoLseek((fd), (o), (w)))
#define IO_CLOSE(fd) sceIoClose(fd)
#endif

#define A_SEEK_SET 0
#define A_SEEK_CUR 1
#define A_SEEK_END 2

int aio_open(aio_t *a, const char *path) {
  memset(a, 0, sizeof(*a));
  a->fd = IO_OPEN(path);
  if (a->fd < 0)
    return -1;
  a->len = IO_SEEK(a->fd, 0, A_SEEK_END);
  if (a->len < 0 || IO_SEEK(a->fd, 0, A_SEEK_SET) != 0) {
    IO_CLOSE(a->fd);
    a->fd = -1;
    return -1;
  }
  a->buf = malloc(AIO_BUF_SIZE);
  return 0;
}

int aio_read(aio_t *a, void *dst, int n) {
  unsigned char *d = dst;
  int total = 0;
  while (n > 0 && a->pos < a->len) {
    /* served from the block we already have */
    if (a->buf_len > 0 && a->pos >= a->buf_off && a->pos < a->buf_off + a->buf_len) {
      long k = a->buf_off + a->buf_len - a->pos;
      if (k > n)
        k = n;
      memcpy(d, a->buf + (a->pos - a->buf_off), (size_t)k);
      d += k;
      n -= (int)k;
      total += (int)k;
      a->pos += k;
      continue;
    }
    if (a->fd_pos != a->pos) {
      if (IO_SEEK(a->fd, a->pos, A_SEEK_SET) != a->pos)
        break;
      a->fd_pos = a->pos;
    }
    if (n >= AIO_BUF_SIZE || !a->buf) {
      /* big read: straight into the caller's memory */
      long want = a->len - a->pos;
      if (want > n)
        want = n;
      int r = IO_READ(a->fd, d, (int)want);
      if (r <= 0)
        break;
      a->fd_pos += r;
      d += r;
      n -= r;
      total += r;
      a->pos += r;
    } else {
      /* small read: fetch the next block */
      int r = IO_READ(a->fd, a->buf, AIO_BUF_SIZE);
      if (r <= 0)
        break;
      a->fd_pos += r;
      a->buf_off = a->pos;
      a->buf_len = r;
    }
  }
  return total;
}

long aio_seek(aio_t *a, long off, int whence) {
  long p = whence == A_SEEK_SET ? off : whence == A_SEEK_CUR ? a->pos + off : whence == A_SEEK_END ? a->len + off : -1;
  if (p < 0 || p > a->len)
    return -1;
  a->pos = p;
  return p;
}

void aio_close(aio_t *a) {
  if (a->fd >= 0)
    IO_CLOSE(a->fd);
  a->fd = -1;
  free(a->buf);
  a->buf = NULL;
}
