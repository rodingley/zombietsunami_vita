/* asset_io_test.c -- checks src/asset_io.c against plain pread() with random
 * reads and seeks.  Build and run on a computer:
 *   cc -O2 -DHOST_TEST -Isrc test/asset_io_test.c src/asset_io.c -o aio_test && ./aio_test FILE... */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "asset_io.h"

int main(int argc, char **argv) {
  int bad = 0;
  srand(1234);
  for (int fi = 1; fi < argc; fi++) {
    int ref = open(argv[fi], O_RDONLY);
    long len = lseek(ref, 0, SEEK_END);
    unsigned char *want = malloc(len + 1), *got = malloc(len + 300000);
    pread(ref, want, len, 0);
    aio_t a;
    if (aio_open(&a, argv[fi]) != 0 || a.len != len) { printf("open/len FAILED %s\n", argv[fi]); bad++; continue; }
    /* 1) whole file in one read, then 0 at the end */
    if (aio_read(&a, got, (int)len + 10) != len || memcmp(got, want, len) || aio_read(&a, got, 10) != 0) { printf("whole read FAILED\n"); bad++; }
    /* 2) random mix of seeks and reads of every size */
    long pos = 0;
    aio_seek(&a, 0, 0);
    for (int i = 0; i < 20000; i++) {
      int op = rand() % 4;
      if (op == 0) { /* seek */
        int wh = rand() % 3;
        long target = rand() % (len + 1);
        long off = wh == 0 ? target : wh == 1 ? target - pos : target - len;
        if (aio_seek(&a, off, wh) != target) { printf("seek FAILED\n"); bad++; break; }
        pos = target;
      } else {
        int sizes[] = { 1, 4, 16, 100, 4096, 65535, 65536, 65537, 200000 };
        int n = sizes[rand() % 9];
        int r = aio_read(&a, got, n);
        long expect = len - pos < n ? len - pos : n;
        if (r != expect || memcmp(got, want + pos, r)) { printf("read FAILED at %ld n %d (got %d, want %ld)\n", pos, n, r, expect); bad++; break; }
        pos += r;
      }
    }
    /* 3) bad seeks are refused and do not move */
    if (aio_seek(&a, -1, 0) != -1 || aio_seek(&a, len + 1, 0) != -1) { printf("bad seek accepted\n"); bad++; }
    aio_close(&a);
    printf("%s: %ld bytes ok\n", argv[fi], len);
    free(want);
    free(got);
    close(ref);
  }
  printf(bad ? "FAILED\n" : "ALL OK\n");
  return bad != 0;
}
