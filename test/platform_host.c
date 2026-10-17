/* platform_host.c -- so_platform.h on a normal Linux computer (for testing
 * the loader logic against the real .so files; nothing is executed). */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../src/so_platform.h"

#define MAX_BLOCKS 64
static struct { void *base; size_t size; int used; } blocks[MAX_BLOCKS];

static int new_block(void *base, size_t size) {
  for (int i = 0; i < MAX_BLOCKS; i++) {
    if (!blocks[i].used) {
      blocks[i].used = 1;
      blocks[i].base = base;
      blocks[i].size = size;
      return i;
    }
  }
  return -1;
}

void so_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

void so_log_flush(void) {}

void so_fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("FATAL: ");
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  exit(2);
}

int so_plat_read_file(const char *path, void **data, size_t *size, int *blockid) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  void *buf = malloc(len);
  if (!buf || fread(buf, 1, len, f) != (size_t)len) {
    fclose(f);
    free(buf);
    return -1;
  }
  fclose(f);
  *data = buf;
  *size = len;
  *blockid = new_block(buf, 0); /* size 0 => malloc'd */
  return 0;
}

int so_plat_alloc(const char *name, size_t size, uintptr_t addr, int exec,
                  int *blockid, uintptr_t *base) {
  (void)name;
  (void)exec;
  void *p = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED)
    return -1;
  *base = (uintptr_t)p;
  *blockid = new_block(p, size);
  return 0;
}

void so_plat_free(int blockid) {
  if (blockid < 0 || blockid >= MAX_BLOCKS || !blocks[blockid].used)
    return;
  if (blocks[blockid].size == 0)
    free(blocks[blockid].base);
  else
    munmap(blocks[blockid].base, blocks[blockid].size);
  blocks[blockid].used = 0;
}

void so_plat_write(void *dst, const void *src, size_t size) {
  memcpy(dst, src, size);
}

void so_plat_flush(uintptr_t base, size_t size) {
  (void)base;
  (void)size;
}
