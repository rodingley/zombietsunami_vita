/* so_platform.h -- the few system calls the ELF loader needs.
 *
 * The loader logic in so_util.c is platform independent. On the Vita these
 * are implemented in platform_vita.c (kubridge); for testing on a normal
 * computer they are implemented in test/platform_host.c (mmap).
 */

#ifndef SO_PLATFORM_H
#define SO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Logging.  On the Vita lines are collected in memory and written to log.txt
 * by a background thread about twice a second (see platform_vita.c);
 * so_log_flush() writes whatever is waiting right now. */
void so_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void so_log_flush(void);
void so_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Read a whole file into a temporary buffer. Returns 0 or a negative error. */
int so_plat_read_file(const char *path, void **data, size_t *size, int *blockid);

/* Allocate a block at a fixed address. exec != 0 means executable memory.
 * Returns 0 or a negative error. */
int so_plat_alloc(const char *name, size_t size, uintptr_t addr, int exec,
                  int *blockid, uintptr_t *base);

void so_plat_free(int blockid);

/* Write into memory even if it is read-only/executable. */
void so_plat_write(void *dst, const void *src, size_t size);

/* Make freshly written code visible to the instruction cache. */
void so_plat_flush(uintptr_t base, size_t size);

#endif
