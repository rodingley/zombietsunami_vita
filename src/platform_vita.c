/* platform_vita.c -- so_platform.h on the PS Vita (needs kubridge.skprx) */

#include <vitasdk.h>
#include <kubridge.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "so_platform.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

/* ---- log.txt ------------------------------------------------------------------
 * Writing to the memory card takes a few milliseconds, which is a dropped frame
 * when the game's own thread has to wait for it.  So lines are collected in
 * memory and a low-priority thread on another CPU core writes them out about
 * twice a second (sooner when a lot is waiting).  Every line also goes to the
 * debug console (sceClibPrintf), and that is what a crash dump (.psp2dmp)
 * keeps, so the last lines before a crash are never lost. */
#define LOG_BUF_SIZE (128 * 1024)
/* two buffers: the game adds lines to one while the other is being written out,
 * so a line never has to wait for the memory card (the lock is only ever held
 * for a short copy or for swapping the two) */
static char log_bufs[2][LOG_BUF_SIZE];
static int log_cur;
static unsigned log_used, log_dropped;
static SceKernelLwMutexWork log_lock, log_io_lock;
static SceUID log_sema = -1;
static volatile int log_async; /* 0 until the writer thread runs: then lines are written straight away */

static void log_write_now(const char *buf, int n) {
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
  }
}

void so_log_flush(void) {
  if (!log_async)
    return;
  sceKernelLockLwMutex(&log_io_lock, 1, NULL);
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  int full = log_cur;
  unsigned n = log_used, dropped = log_dropped;
  log_cur ^= 1;
  log_used = log_dropped = 0;
  sceKernelUnlockLwMutex(&log_lock, 1);
  if (dropped) {
    char m[80];
    int k = snprintf(m, sizeof(m), "[log] %u bytes of log lines were dropped (too many at once)\n", dropped);
    log_write_now(m, k);
  }
  if (n)
    log_write_now(log_bufs[full], (int)n);
  sceKernelUnlockLwMutex(&log_io_lock, 1);
}

static int log_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    SceUInt to = 500 * 1000;
    sceKernelWaitSema(log_sema, 1, &to);
    so_log_flush();
  }
  return 0;
}

void so_log_reset(void) {
  sceIoMkdir(DATA_PATH, 0777);
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) {
    static const char hdr[] = "--- log start ---\n";
    sceIoWrite(fd, hdr, sizeof(hdr) - 1);
    sceIoClose(fd);
  }
  if (sceKernelCreateLwMutex(&log_lock, "log", 0, 0, NULL) < 0 || sceKernelCreateLwMutex(&log_io_lock, "logio", 0, 0, NULL) < 0)
    return;
  log_sema = sceKernelCreateSema("log", 0, 0, 1, NULL);
  SceUID t = sceKernelCreateThread("log writer", log_thread, 0x10000100, 0x4000, 0, 0, NULL);
  if (log_sema >= 0 && t >= 0 && sceKernelStartThread(t, 0, NULL) >= 0)
    log_async = 1;
}

volatile int so_log_quiet; /* once set, only a few important lines are written */

void so_log(const char *fmt, ...) {
  if (so_log_quiet && strncmp(fmt, "[fps]", 5) != 0 && strncmp(fmt, "FATAL", 5) != 0 && strncmp(fmt, "[gl]", 4) != 0 &&
      strncmp(fmt, "[popup]", 7) != 0 && strncmp(fmt, "[art]", 5) != 0 && strncmp(fmt, "===", 3) != 0 &&
      strncmp(fmt, "[input]", 7) != 0 && strncmp(fmt, "[load]", 6) != 0 && strncmp(fmt, "[shop]", 6) != 0 &&
      strncmp(fmt, "[daily]", 7) != 0 && strncmp(fmt, "[state]", 7) != 0 &&
      strncmp(fmt, "[promo]", 7) != 0)
    return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n > (int)sizeof(buf) - 2)
    n = sizeof(buf) - 2;
  buf[n++] = '\n';
  buf[n] = '\0';
  sceClibPrintf("%s", buf);

  if (!log_async) {
    log_write_now(buf, n);
    return;
  }
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  if (log_used + (unsigned)n <= LOG_BUF_SIZE) {
    memcpy(log_bufs[log_cur] + log_used, buf, n);
    log_used += n;
  } else {
    log_dropped += n;
  }
  int busy = log_used > LOG_BUF_SIZE / 2;
  sceKernelUnlockLwMutex(&log_lock, 1);
  if (busy)
    sceKernelSignalSema(log_sema, 1);
}

void so_fatal(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  so_log("FATAL: %s", buf);
  so_log_flush();
  sceKernelExitProcess(0);
  for (;;)
    ;
}

int so_plat_read_file(const char *path, void **data, size_t *size, int *blockid) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return fd;

  size_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);

  SceUID id = sceKernelAllocMemBlock("so_file", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                     (len + 0xfff) & ~0xfff, NULL);
  if (id < 0) {
    sceIoClose(fd);
    return id;
  }

  void *buf;
  sceKernelGetMemBlockBase(id, &buf);
  int rd = sceIoRead(fd, buf, len);
  sceIoClose(fd);
  if (rd != (int)len) {
    sceKernelFreeMemBlock(id);
    return -1;
  }

  *data = buf;
  *size = len;
  *blockid = id;
  return 0;
}

int so_plat_alloc(const char *name, size_t size, uintptr_t addr, int exec,
                  int *blockid, uintptr_t *base) {
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  opt.attr = 0x1;
  opt.field_C = (SceUInt32)addr;

  SceUID id = kuKernelAllocMemBlock(name,
                                    exec ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
                                         : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                    size, &opt);
  if (id < 0)
    return id;

  void *b;
  sceKernelGetMemBlockBase(id, &b);
  *blockid = id;
  *base = (uintptr_t)b;
  return 0;
}

void so_plat_free(int blockid) {
  if (blockid >= 0)
    sceKernelFreeMemBlock(blockid);
}

void so_plat_write(void *dst, const void *src, size_t size) {
  kuKernelCpuUnrestrictedMemcpy(dst, src, size);
}

void so_plat_flush(uintptr_t base, size_t size) {
  kuKernelFlushCaches((void *)base, size);
}
