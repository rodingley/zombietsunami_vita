/* imports.c -- Android (bionic) functions, implemented on top of the Vita.
 *
 * The game's libraries were built for Android. Many C library functions look
 * the same but are NOT binary compatible with the Vita's newlib:
 *   - pthread types are 4 bytes on Android, bigger/different here
 *   - time_t is 32-bit on Android, 64-bit here (so struct timespec/tm differ)
 *   - struct stat / dirent / FILE layouts differ, open() flag values differ
 * Everything like that is translated here. Functions that really are
 * identical are listed in the generated direct_imports.c instead.
 *
 * Entries for these shims are in shim_dynlib[] at the bottom of the file.
 */

#include <vitasdk.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "config.h"
#include "so_platform.h"
#include "so_util.h"
#include "stage1.h"
#include "zlib/zlib.h"
#include "assets.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */

/* "libcgame.so+0x1234" for an address inside one of the game libraries */
static const char *where(const void *addr, char *buf, size_t n) {
  uintptr_t a = (uintptr_t)addr;
  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    if (a >= game_mods[i].text_base && a < game_mods[i].text_base + game_mods[i].text_size) {
      snprintf(buf, n, "%s+0x%x", game_mods[i].name, (unsigned)(a - game_mods[i].text_base));
      return buf;
    }
  }
  snprintf(buf, n, "%p", addr);
  return buf;
}

/* Best-effort "who is waiting": scans the stack above this function for
 * values that look like return addresses into the game's code. */
static void log_wait_trace(const char *what, int waited) {
  if (waited != 10)
    return;
  uintptr_t marker;
  uintptr_t *sp = &marker;
  char line[400];
  int n = snprintf(line, sizeof(line), "[wait-trace] thread %d %s %ds:", (int)sceKernelGetThreadId(), what, waited);
  int found = 0;
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof(ti));
  ti.size = sizeof(ti);
  if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0 || !ti.stack || !ti.stackSize) {
    so_log("%s (no stack info)", line);
    return;
  }
  uintptr_t top = (uintptr_t)ti.stack + (uintptr_t)ti.stackSize;
  int maxw = (int)((top - (uintptr_t)sp) / sizeof(uintptr_t));
  if ((uintptr_t)sp < (uintptr_t)ti.stack || (uintptr_t)sp >= top)
    maxw = 0;
  if (maxw > 600)
    maxw = 600;
  for (int i = 1; i < maxw && found < 12 && n < (int)sizeof(line) - 40; i++) {
    uintptr_t v = sp[i];
    if (!(v & 1))
      continue;
    for (int m = 0; m < NUM_GAME_LIBS; m++) {
      uintptr_t a = v & ~(uintptr_t)1;
      if (a >= game_mods[m].text_base && a < game_mods[m].text_base + game_mods[m].text_size) {
        const char *nm = game_mods[m].name;
        n += snprintf(line + n, sizeof(line) - n, " %c%c+0x%x", nm[3], nm[4], (unsigned)(a - game_mods[m].text_base));
        found++;
        break;
      }
    }
  }
  so_log("%s", line);
}

#define WAIT_TIMEOUT_ERR ((int)0x80028005)

/* Many of the game's helper threads simply sleep until there is work (that is
 * normal), so a long wait is only worth a log line now and then. */
static int wait_worth_logging(int waited) { return waited == 10 || waited % 120 == 0; }
#define WAIT_SLICE_US 2000000u

static int file_log_count;
#define FILE_LOG(...) do { if (file_log_count++ < 800) so_log(__VA_ARGS__); } while (0)

static int ret0(void) { return 0; }
static int ret_m1(void) { return -1; }
static void *ret_null(void) { return NULL; }
int gl_stub(void) { return 0; } /* OpenGL calls are stubbed in stage 2a */

#define BIONIC_EAGAIN 11
#define BIONIC_EBUSY 16
#define BIONIC_ENOMEM 12
#define BIONIC_ETIMEDOUT 110

/* ------------------------------------------------------------------ */
/* stdio: Android's stdout/stderr are &__sF[1] / &__sF[2]                */
/* ------------------------------------------------------------------ */

#define BIONIC_FILE_SIZE 84
static uint8_t sF_buf[3 * BIONIC_FILE_SIZE] __attribute__((aligned(8)));
static void *stderr_ptr = &sF_buf[2 * BIONIC_FILE_SIZE];
static uint32_t stack_chk_guard = 0x42424242;

static int is_std(const void *f) {
  const uint8_t *p = (const uint8_t *)f;
  return p >= sF_buf && p < sF_buf + sizeof(sF_buf);
}

static char std_line[400];
static int std_line_len;

static void std_putc(int c) {
  if (c == '\n' || std_line_len >= (int)sizeof(std_line) - 1) {
    std_line[std_line_len] = '\0';
    so_log("[stdio] %s", std_line);
    std_line_len = 0;
    if (c == '\n')
      return;
  }
  std_line[std_line_len++] = (char)c;
}

static int vfprintf_shim(void *f, const char *fmt, va_list ap) {
  if (is_std(f)) {
    char buf[400];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    for (int i = 0; i < n && i < (int)sizeof(buf) - 1; i++)
      std_putc(buf[i]);
    return n;
  }
  return vfprintf((FILE *)f, fmt, ap);
}

static int fprintf_shim(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf_shim(f, fmt, ap);
  va_end(ap);
  return r;
}

static size_t fwrite_shim(const void *p, size_t size, size_t n, void *f) {
  if (is_std(f)) {
    const char *c = (const char *)p;
    for (size_t i = 0; i < size * n; i++)
      std_putc(c[i]);
    return n;
  }
  return fwrite(p, size, n, (FILE *)f);
}

static int fputc_shim(int c, void *f) {
  if (is_std(f)) {
    std_putc(c);
    return c;
  }
  return fputc(c, (FILE *)f);
}

static int fflush_shim(void *f) {
  if (f == NULL || is_std(f))
    return 0;
  return fflush((FILE *)f);
}

static int fputwc_shim(wchar_t c, void *f) {
  (void)f;
  return (int)c;
}

static int fileno_shim(void *f) {
  if (is_std(f))
    return (int)(((uint8_t *)f - sF_buf) / BIONIC_FILE_SIZE);
  return fileno((FILE *)f);
}

static int isatty_shim(int fd) {
  (void)fd;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Android logging and fatal errors                                     */
/* ------------------------------------------------------------------ */

static int android_log_print(int prio, const char *tag, const char *fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  so_log("[android %d] %s: %s", prio, tag ? tag : "?", buf);
  return 0;
}

static int android_log_write(int prio, const char *tag, const char *text) {
  so_log("[android %d] %s: %s", prio, tag ? tag : "?", text ? text : "");
  return 0;
}

static void abort_shim(void) {
  so_fatal("abort() called by the game");
}

static void exit_shim(int code) {
  so_log("exit(%d) called by the game", code);
  exit(code);
}

static void assert2_shim(const char *file, int line, const char *func, const char *expr) {
  so_fatal("assert failed: %s:%d %s: %s", file ? file : "?", line, func ? func : "?",
           expr ? expr : "?");
}

static void stack_chk_fail_shim(void) {
  so_fatal("stack smashing detected (__stack_chk_fail)");
}

/* ------------------------------------------------------------------ */
/* Fortified (_chk) functions and ARM EABI memory helpers               */
/* ------------------------------------------------------------------ */

static void *memcpy_chk_shim(void *d, const void *s, size_t n, size_t dl) {
  (void)dl;
  return memcpy(d, s, n);
}

static void *memset_chk_shim(void *d, int c, size_t n, size_t dl) {
  (void)dl;
  return memset(d, c, n);
}

static ssize_t read_chk_shim(int fd, void *buf, size_t n, size_t bl) {
  (void)bl;
  return read(fd, buf, n);
}

static size_t strlen_chk_shim(const char *s, size_t sl) {
  (void)sl;
  return strlen(s);
}

static char *strcpy_chk_shim(char *d, const char *s, size_t dl) {
  (void)dl;
  return strcpy(d, s);
}

static char *strcat_chk_shim(char *d, const char *s, size_t dl) {
  (void)dl;
  return strcat(d, s);
}

static char *strncpy_chk2_shim(char *d, const char *s, size_t n, size_t dl, size_t sl) {
  (void)dl;
  (void)sl;
  return strncpy(d, s, n);
}

static int vsnprintf_chk_shim(char *d, size_t max, int flags, size_t dl, const char *fmt,
                              va_list ap) {
  (void)flags;
  (void)dl;
  return vsnprintf(d, max, fmt, ap);
}

static void fd_chk_shim(int fd, void *set, size_t size) {
  (void)fd;
  (void)set;
  (void)size;
}

/* ARM EABI: __aeabi_memset(dst, n, c) has n and c swapped vs memset */
static void *aeabi_memset(void *d, size_t n, int c) { return memset(d, c, n); }
static void *aeabi_memclr(void *d, size_t n) { return memset(d, 0, n); }
static void *aeabi_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *aeabi_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }

static int cxa_atexit_shim(void *fn, void *arg, void *dso) {
  (void)fn;
  (void)arg;
  (void)dso;
  return 0; /* we never run static destructors: the process just exits */
}

static void cxa_finalize_shim(void *dso) { (void)dso; }

static int register_atfork_shim(void *a, void *b, void *c, void *d) {
  (void)a; (void)b; (void)c; (void)d;
  return 0;
}

static uintptr_t dl_unwind_find_exidx_shim(uintptr_t pc, int *count) {
  return so_find_exidx(pc, count);
}

static int ctype_get_mb_cur_max_shim(void) { return 4; }

static int posix_memalign_shim(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return BIONIC_ENOMEM;
  *out = p;
  return 0;
}

/* XSI version: really copies the message */
static int strerror_r_xsi_shim(int err, char *buf, size_t n) {
  const char *s = strerror(err);
  if (n == 0)
    return 0;
  strncpy(buf, s ? s : "error", n - 1);
  buf[n - 1] = '\0';
  return 0;
}

static int *errno_shim(void) { return &errno; }

static int cxa_thread_atexit_shim(void *fn, void *arg, void *dso) {
  (void)fn; (void)arg; (void)dso;
  return 0;
}

/* printf / puts / putchar go to the log file instead of the Vita's stdout */
static int printf_shim(const char *fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (int i = 0; i < n && i < (int)sizeof(buf) - 1; i++)
    std_putc(buf[i]);
  return n;
}

static int puts_shim(const char *s) {
  while (*s)
    std_putc(*s++);
  std_putc('\n');
  return 1;
}

static int putchar_shim(int c) {
  std_putc(c);
  return c;
}

/* ------------------------------------------------------------------ */
/* dynamic loading: FMOD asks for audio back-ends this way               */
/* ------------------------------------------------------------------ */

extern void *opensl_dlopen(const char *name);
extern void *opensl_dlsym(void *handle, const char *name);

static void *dlopen_shim(const char *name, int flags) {
  (void)flags;
  void *h = opensl_dlopen(name);
  if (h)
    return h;
  so_log("dlopen(\"%s\") -> NULL", name ? name : "(null)");
  return NULL;
}

static void *dlsym_shim(void *handle, const char *name) {
  void *sym = opensl_dlsym(handle, name);
  if (sym)
    return sym;
  so_log("dlsym(%p, \"%s\") -> NULL", handle, name ? name : "(null)");
  return NULL;
}

static int dlclose_shim(void *handle) {
  (void)handle;
  return 0;
}

/* ------------------------------------------------------------------ */
/* system information                                                   */
/* ------------------------------------------------------------------ */

static long sysconf_shim(int name) {
  switch (name) {
    case 0x27: return 4096; /* _SC_PAGESIZE */
    case 0x60:              /* _SC_NPROCESSORS_CONF */
    case 0x61: return 4;    /* _SC_NPROCESSORS_ONLN */
    case 0x02: return 100;  /* _SC_CLK_TCK */
    default: return -1;
  }
}

static unsigned long getauxval_shim(unsigned long type) {
  if (type == 16) /* AT_HWCAP: VFP | NEON | VFPv3, as on a real Cortex-A9 */
    return (1u << 6) | (1u << 12) | (1u << 13);
  return 0;
}

static int gettid_shim(void) { return (int)sceKernelGetThreadId(); }

static long syscall_shim(long number, ...) {
  if (number == 224) /* __NR_gettid on ARM */
    return sceKernelGetThreadId();
  return -1;
}

static void *getenv_shim(const char *name) {
  (void)name;
  return NULL;
}

static char *realpath_shim(const char *path, char *out) {
  if (!out)
    out = malloc(1024);
  if (out) {
    strncpy(out, path, 1023);
    out[1023] = '\0';
  }
  return out;
}

static int sched_yield_shim(void) {
  sceKernelDelayThread(1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* time: Android's time_t, timespec and timeval are 32-bit              */
/* ------------------------------------------------------------------ */

typedef struct {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
} bionic_tm;

static bionic_tm tm_buf;

static bionic_tm *fill_tm(const struct tm *t) {
  if (!t)
    return NULL;
  memset(&tm_buf, 0, sizeof(tm_buf));
  tm_buf.tm_sec = t->tm_sec;
  tm_buf.tm_min = t->tm_min;
  tm_buf.tm_hour = t->tm_hour;
  tm_buf.tm_mday = t->tm_mday;
  tm_buf.tm_mon = t->tm_mon;
  tm_buf.tm_year = t->tm_year;
  tm_buf.tm_wday = t->tm_wday;
  tm_buf.tm_yday = t->tm_yday;
  tm_buf.tm_isdst = t->tm_isdst;
  tm_buf.tm_zone = "UTC";
  return &tm_buf;
}

static int32_t time_shim(int32_t *out) {
  int32_t v = (int32_t)time(NULL);
  if (out)
    *out = v;
  return v;
}

static bionic_tm *localtime_shim(const int32_t *t) {
  time_t tt = *t;
  return fill_tm(localtime(&tt));
}

static bionic_tm *gmtime_shim(const int32_t *t) {
  time_t tt = *t;
  return fill_tm(gmtime(&tt));
}

static char *ctime_shim(const int32_t *t) {
  time_t tt = *t;
  return ctime(&tt);
}

static int gettimeofday_shim(int32_t *tv, void *tz) {
  (void)tz;
  struct timeval t;
  gettimeofday(&t, NULL);
  tv[0] = (int32_t)t.tv_sec;
  tv[1] = (int32_t)t.tv_usec;
  return 0;
}

static int clock_gettime_shim(int clk, int32_t *ts) {
  uint64_t us;
  if (clk == 0 || clk == 5) { /* CLOCK_REALTIME(_COARSE) */
    struct timeval t;
    gettimeofday(&t, NULL);
    us = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_usec;
  } else { /* monotonic, boottime, cpu time, ... */
    us = sceKernelGetProcessTimeWide();
  }
  ts[0] = (int32_t)(us / 1000000u);
  ts[1] = (int32_t)((us % 1000000u) * 1000u);
  return 0;
}

static int nanosleep_shim(const int32_t *req, int32_t *rem) {
  uint64_t us = (uint64_t)(uint32_t)req[0] * 1000000u + (uint32_t)req[1] / 1000u;
  if (us > 0xffffffffu)
    us = 0xffffffffu;
  sceKernelDelayThread((SceUInt)us);
  if (rem) {
    rem[0] = 0;
    rem[1] = 0;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* files: stat, open, directories                                       */
/* ------------------------------------------------------------------ */

/* Android ARM32 struct stat (104 bytes): mode@16 nlink@20 size@48 (int64)
 * blksize@56 blocks@64 atime@72 mtime@80 ctime@88 */
static void stat_fill(void *buf, const struct stat *st) {
  uint8_t *b = (uint8_t *)buf;
  memset(b, 0, 104);
  *(uint32_t *)(b + 16) = (uint32_t)st->st_mode;
  *(uint32_t *)(b + 20) = 1;
  *(int64_t *)(b + 48) = (int64_t)st->st_size;
  *(uint32_t *)(b + 56) = 4096;
  *(int64_t *)(b + 64) = ((int64_t)st->st_size + 511) / 512;
  *(uint32_t *)(b + 72) = (uint32_t)st->st_atime;
  *(uint32_t *)(b + 80) = (uint32_t)st->st_mtime;
  *(uint32_t *)(b + 88) = (uint32_t)st->st_ctime;
}

static int stat_shim(const char *path, void *buf) {
  struct stat st;
  int r = stat(path, &st);
  FILE_LOG("[file] stat \"%s\" -> %d", path, r);
  if (r == 0)
    stat_fill(buf, &st);
  return r;
}

static int fstat_shim(int fd, void *buf) {
  struct stat st;
  int r = fstat(fd, &st);
  if (r == 0)
    stat_fill(buf, &st);
  return r;
}

static int open_flags(int f) {
  int r = f & 3;
  if (f & 0x40) r |= O_CREAT;
  if (f & 0x80) r |= O_EXCL;
  if (f & 0x200) r |= O_TRUNC;
  if (f & 0x400) r |= O_APPEND;
  return r;
}


/* Android-only system files that FMOD reads: give it small fake copies */
static const char *fake_sysfile(const char *path) {
  const char *content = NULL, *dst = NULL;
  if (!path)
    return path;
  if (!strcmp(path, "/proc/cpuinfo")) {
    dst = "ux0:data/zombietsunami/fake_cpuinfo.txt";
    content = "Processor\t: ARMv7 Processor rev 0 (v7l)\n"
              "processor\t: 0\n"
              "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpd32\n"
              "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc09\nCPU revision\t: 0\n\n"
              "Hardware\t: Vita\n";
  } else if (!strcmp(path, "/sys/devices/system/cpu/possible") || !strcmp(path, "/sys/devices/system/cpu/present")) {
    dst = "ux0:data/zombietsunami/fake_cpucount.txt";
    content = "0-3\n";
  } else {
    return path;
  }
  FILE *f = fopen(dst, "wb");
  if (f) {
    fputs(content, f);
    fclose(f);
    return dst;
  }
  return path;
}

static int open_shim(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & 0x40) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  path = fake_sysfile(path);
  int r = open(path, open_flags(flags), mode);
  FILE_LOG("[file] open \"%s\" 0x%x -> %d", path, flags, r);
  return r;
}

static int open2_shim(const char *path, int flags) {
  path = fake_sysfile(path);
  return open(path, open_flags(flags), 0666);
}

static void *fopen_shim(const char *path, const char *mode) {
  path = fake_sysfile(path);
  FILE *f = fopen(path, mode);
  if (f)
    FILE_LOG("[file] fopen \"%s\" %s -> ok", path, mode);
  else
    so_log("[file] fopen \"%s\" %s -> FAILED (errno %d)", path, mode, errno);
  return f;
}

static int access_shim(const char *path, int mode) {
  int r = access(path, mode);
  FILE_LOG("[file] access \"%s\" %d -> %d", path, mode, r);
  return r;
}

static int mkdir_shim(const char *path, int mode) {
  int r = mkdir(path, mode);
  FILE_LOG("[file] mkdir \"%s\" -> %d", path, r);
  return r;
}

static void *opendir_shim(const char *path) {
  void *d = opendir(path);
  FILE_LOG("[file] opendir \"%s\" -> %s", path, d ? "ok" : "FAILED");
  return d;
}

static int remove_shim(const char *path) {
  int r = remove(path);
  FILE_LOG("[file] remove \"%s\" -> %d", path, r);
  return r;
}

static int rename_shim(const char *a, const char *b) {
  int r = rename(a, b);
  FILE_LOG("[file] rename \"%s\" \"%s\" -> %d", a, b, r);
  return r;
}

/* memory: only logs failures */
static void *malloc_shim(size_t n) {
  void *p = malloc(n);
  if (!p)
    so_log("[mem] malloc(%u) FAILED", (unsigned)n);
  return p;
}
static void *calloc_shim(size_t a, size_t b) {
  void *p = calloc(a, b);
  if (!p)
    so_log("[mem] calloc(%u,%u) FAILED", (unsigned)a, (unsigned)b);
  return p;
}
static void *realloc_shim(void *q, size_t n) {
  void *p = realloc(q, n);
  if (!p && n)
    so_log("[mem] realloc(%u) FAILED", (unsigned)n);
  return p;
}

/* Android struct dirent: ino@0 off@8 reclen@16 type@18 name@19 */
static uint8_t dirent_buf[280];

static void *readdir_shim(void *dir) {
  struct dirent *e = readdir((DIR *)dir);
  if (!e)
    return NULL;
  memset(dirent_buf, 0, sizeof(dirent_buf));
  *(uint16_t *)(dirent_buf + 16) = sizeof(dirent_buf);
  dirent_buf[18] = 0; /* DT_UNKNOWN: callers stat the entry if they care */
  strncpy((char *)dirent_buf + 19, e->d_name, 255);
  return dirent_buf;
}

/* ------------------------------------------------------------------ */
/* assets: the game reads its data through Android's AAsset API (assets.c) */
/* ------------------------------------------------------------------ */

static void *AAssetManager_fromJava_shim(void *env, void *obj) {
  (void)env;
  (void)obj;
  return (void *)0x1;
}

static void *AAssetManager_open_shim(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;
  return assets_open(filename);
}

/* ------------------------------------------------------------------ */
/* threads: Android pthread types are 4 bytes; we build them on Vita     */
/* kernel objects and keep a pointer/handle in that 4-byte slot          */
/* ------------------------------------------------------------------ */

static SceKernelLwMutexWork lazy_guard;

void imports_init(void) {
  sceKernelCreateLwMutex(&lazy_guard, "lazy_guard", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
  assets_init();
}

/* values below this are static initializers (0, 0x4000, 0x8000), not pointers */
#define SLOT_MIN_PTR 0x10000u

static SceKernelLwMutexWork *mutex_get(uint32_t *slot) {
  if (*slot >= SLOT_MIN_PTR)
    return (SceKernelLwMutexWork *)(uintptr_t)*slot;
  sceKernelLockLwMutex(&lazy_guard, 1, NULL);
  if (*slot < SLOT_MIN_PTR) {
    SceKernelLwMutexWork *w = memalign(8, sizeof(SceKernelLwMutexWork));
    if (w && sceKernelCreateLwMutex(w, "mutex", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0) {
      free(w);
      w = NULL;
    }
    *slot = (uint32_t)(uintptr_t)w;
  }
  sceKernelUnlockLwMutex(&lazy_guard, 1);
  return (SceKernelLwMutexWork *)(uintptr_t)*slot;
}

static int mutex_init_shim(uint32_t *slot, const void *attr) {
  (void)attr;
  *slot = 0;
  return mutex_get(slot) ? 0 : BIONIC_ENOMEM;
}

static int mutex_destroy_shim(uint32_t *slot) {
  if (*slot >= SLOT_MIN_PTR) {
    SceKernelLwMutexWork *w = (SceKernelLwMutexWork *)(uintptr_t)*slot;
    sceKernelDeleteLwMutex(w);
    free(w);
  }
  *slot = 0;
  return 0;
}

static int mutex_lock_shim(uint32_t *slot) {
  SceKernelLwMutexWork *w = mutex_get(slot);
  if (!w)
    return BIONIC_ENOMEM;
  int waited = 0;
  for (;;) {
    SceUInt to = WAIT_SLICE_US;
    int r = sceKernelLockLwMutex(w, 1, &to);
    if (r >= 0)
      return 0;
    if (r != WAIT_TIMEOUT_ERR)
      return -1;
    waited += 2;
    char b[64];
    if (wait_worth_logging(waited))
      so_log("[blocked] mutex_lock for %ds, caller %s", waited, where(__builtin_return_address(0), b, sizeof(b)));
    log_wait_trace("mutex_lock", waited);
  }
}

static int mutex_unlock_shim(uint32_t *slot) {
  SceKernelLwMutexWork *w = mutex_get(slot);
  if (!w)
    return BIONIC_ENOMEM;
  return sceKernelUnlockLwMutex(w, 1) < 0 ? -1 : 0;
}

static int mutex_trylock_shim(uint32_t *slot) {
  SceKernelLwMutexWork *w = mutex_get(slot);
  if (!w)
    return BIONIC_ENOMEM;
  return sceKernelTryLockLwMutex(w, 1) < 0 ? BIONIC_EBUSY : 0;
}

static int mutexattr_init_shim(int *a) { *a = 0; return 0; }
static int mutexattr_settype_shim(int *a, int t) { *a = t; return 0; }
static int mutexattr_destroy_shim(int *a) { (void)a; return 0; }

/* condition variable = Vita semaphore + waiter count */
typedef struct {
  SceUID sema;
  volatile int waiters;
} cond_t;

static cond_t *cond_get(uint32_t *slot) {
  if (*slot >= SLOT_MIN_PTR)
    return (cond_t *)(uintptr_t)*slot;
  sceKernelLockLwMutex(&lazy_guard, 1, NULL);
  if (*slot < SLOT_MIN_PTR) {
    cond_t *c = malloc(sizeof(cond_t));
    if (c) {
      c->waiters = 0;
      c->sema = sceKernelCreateSema("cond", 0, 0, 0x7fffffff, NULL);
      if (c->sema < 0) {
        free(c);
        c = NULL;
      }
    }
    *slot = (uint32_t)(uintptr_t)c;
  }
  sceKernelUnlockLwMutex(&lazy_guard, 1);
  return (cond_t *)(uintptr_t)*slot;
}

static int cond_init_shim(uint32_t *slot, const void *attr) {
  (void)attr;
  *slot = 0;
  return cond_get(slot) ? 0 : BIONIC_ENOMEM;
}

static int cond_destroy_shim(uint32_t *slot) {
  if (*slot >= SLOT_MIN_PTR) {
    cond_t *c = (cond_t *)(uintptr_t)*slot;
    sceKernelDeleteSema(c->sema);
    free(c);
  }
  *slot = 0;
  return 0;
}

static int cond_wait_shim(uint32_t *cslot, uint32_t *mslot) {
  cond_t *c = cond_get(cslot);
  if (!c)
    return BIONIC_ENOMEM;
  __sync_fetch_and_add(&c->waiters, 1);
  mutex_unlock_shim(mslot);
  int waited = 0;
  for (;;) {
    SceUInt to = WAIT_SLICE_US;
    int r = sceKernelWaitSema(c->sema, 1, &to);
    if (r >= 0 || r != WAIT_TIMEOUT_ERR)
      break;
    waited += 2;
    char b[64];
    if (wait_worth_logging(waited))
      so_log("[blocked] cond_wait for %ds, caller %s", waited, where(__builtin_return_address(0), b, sizeof(b)));
    log_wait_trace("cond_wait", waited);
  }
  mutex_lock_shim(mslot);
  return 0;
}

static int cond_timedwait_shim(uint32_t *cslot, uint32_t *mslot, const int32_t *abstime) {
  cond_t *c = cond_get(cslot);
  if (!c)
    return BIONIC_ENOMEM;
  struct timeval now;
  gettimeofday(&now, NULL);
  int64_t target = (int64_t)abstime[0] * 1000000 + abstime[1] / 1000;
  int64_t cur = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
  int64_t diff = target - cur;
  if (diff < 1)
    diff = 1;
  if (diff > 0xffffffffll)
    diff = 0xffffffffll;

  __sync_fetch_and_add(&c->waiters, 1);
  mutex_unlock_shim(mslot);
  SceUInt timeout = (SceUInt)diff;
  int r = sceKernelWaitSema(c->sema, 1, &timeout);
  mutex_lock_shim(mslot);
  if (r < 0) {
    int w;
    do {
      w = c->waiters;
      if (w <= 0)
        break;
    } while (!__sync_bool_compare_and_swap(&c->waiters, w, w - 1));
    return BIONIC_ETIMEDOUT;
  }
  return 0;
}

static int cond_signal_shim(uint32_t *cslot) {
  cond_t *c = cond_get(cslot);
  if (!c)
    return BIONIC_ENOMEM;
  int w;
  do {
    w = c->waiters;
    if (w <= 0)
      return 0;
  } while (!__sync_bool_compare_and_swap(&c->waiters, w, w - 1));
  sceKernelSignalSema(c->sema, 1);
  return 0;
}

static int cond_broadcast_shim(uint32_t *cslot) {
  cond_t *c = cond_get(cslot);
  if (!c)
    return BIONIC_ENOMEM;
  int w = __sync_lock_test_and_set(&c->waiters, 0);
  if (w > 0)
    sceKernelSignalSema(c->sema, w);
  return 0;
}

/* semaphores */
static int sem_init_shim(int *uid, int pshared, unsigned value) {
  (void)pshared;
  *uid = sceKernelCreateSema("sem", 0, (int)value, 0x7fffffff, NULL);
  return *uid < 0 ? -1 : 0;
}
static int sem_destroy_shim(int *uid) { return sceKernelDeleteSema(*uid) < 0 ? -1 : 0; }
static int sem_post_shim(int *uid) { return sceKernelSignalSema(*uid, 1) < 0 ? -1 : 0; }
static int sem_wait_shim(int *uid) {
  int waited = 0;
  for (;;) {
    SceUInt to = WAIT_SLICE_US;
    int r = sceKernelWaitSema(*uid, 1, &to);
    if (r >= 0)
      return 0;
    if (r != WAIT_TIMEOUT_ERR)
      return -1;
    waited += 2;
    char b[64];
    if (wait_worth_logging(waited))
      so_log("[blocked] sem_wait for %ds, caller %s", waited, where(__builtin_return_address(0), b, sizeof(b)));
    log_wait_trace("sem_wait", waited);
  }
}

/* pthread_once: state 0 = not run, 1 = running, 2 = done */
static int once_shim(volatile int *control, void (*fn)(void)) {
  if (__sync_bool_compare_and_swap(control, 0, 1)) {
    fn();
    *control = 2;
  } else {
    while (*control != 2)
      sceKernelDelayThread(100);
  }
  return 0;
}

/* thread-specific data: our own table, indexed by key and Vita thread id */
#define TLS_KEYS 256
#define TLS_THREADS 64
static uint8_t tls_used[TLS_KEYS];
static int tls_tid[TLS_THREADS];
static void *tls_val[TLS_THREADS][TLS_KEYS];

static int tls_slot(int create) {
  int tid = (int)sceKernelGetThreadId();
  int free_i = -1;
  for (int i = 0; i < TLS_THREADS; i++) {
    if (tls_tid[i] == tid)
      return i;
    if (tls_tid[i] == 0 && free_i < 0)
      free_i = i;
  }
  if (!create || free_i < 0)
    return -1;
  sceKernelLockLwMutex(&lazy_guard, 1, NULL);
  if (tls_tid[free_i] == 0) {
    tls_tid[free_i] = tid;
    memset(tls_val[free_i], 0, sizeof(tls_val[free_i]));
  }
  sceKernelUnlockLwMutex(&lazy_guard, 1);
  return tls_tid[free_i] == tid ? free_i : -1;
}

static int key_create_shim(uint32_t *key, void (*dtor)(void *)) {
  (void)dtor;
  sceKernelLockLwMutex(&lazy_guard, 1, NULL);
  int k;
  for (k = 1; k < TLS_KEYS && tls_used[k]; k++)
    ;
  if (k < TLS_KEYS)
    tls_used[k] = 1;
  sceKernelUnlockLwMutex(&lazy_guard, 1);
  if (k >= TLS_KEYS)
    return BIONIC_EAGAIN;
  *key = (uint32_t)k;
  return 0;
}
static int key_delete_shim(uint32_t key) {
  if (key < TLS_KEYS)
    tls_used[key] = 0;
  return 0;
}
static void *getspecific_shim(uint32_t key) {
  if (key >= TLS_KEYS)
    return NULL;
  int i = tls_slot(0);
  return i < 0 ? NULL : tls_val[i][key];
}
static int setspecific_shim(uint32_t key, const void *v) {
  if (key >= TLS_KEYS)
    return 22;
  int i = tls_slot(1);
  if (i < 0)
    return BIONIC_ENOMEM;
  tls_val[i][key] = (void *)v;
  return 0;
}

/* threads: Vita kernel threads. pthread_t = the Vita thread id */
typedef struct {
  uint32_t stack_size;
  int32_t detached;
  uint32_t unused[4]; /* Android's pthread_attr_t is 24 bytes */
} bionic_attr;

typedef struct {
  void *(*fn)(void *);
  void *arg;
  int detached;
} thread_start_t;

static int thread_entry(SceSize args, void *argp) {
  (void)args;
  thread_start_t *s = *(thread_start_t **)argp;
  void *(*fn)(void *) = s->fn;
  void *arg = s->arg;
  int detached = s->detached;
  free(s);
  char b[64];
  so_log("[thread] %d starts %s", (int)sceKernelGetThreadId(), where((void *)fn, b, sizeof(b)));
  void *ret = fn(arg);
  so_log("[thread] %d finished", (int)sceKernelGetThreadId());
  if (detached)
    sceKernelExitDeleteThread(0);
  return (int)(intptr_t)ret;
}

static int attr_init_shim(bionic_attr *a) { memset(a, 0, sizeof(*a)); return 0; }
static int attr_destroy_shim(bionic_attr *a) { (void)a; return 0; }
static int attr_setstacksize_shim(bionic_attr *a, size_t sz) { a->stack_size = (uint32_t)sz; return 0; }
static int attr_setdetachstate_shim(bionic_attr *a, int st) { a->detached = (st == 1); return 0; }

static int create_shim(uint32_t *thread, const bionic_attr *attr, void *(*fn)(void *), void *arg) {
  uint32_t stack = (attr && attr->stack_size) ? attr->stack_size : 0x100000;
  if (stack < 0x100000)
    stack = 0x100000;
  stack = (stack + 0xfff) & ~0xfffu;

  thread_start_t *s = malloc(sizeof(thread_start_t));
  if (!s)
    return BIONIC_EAGAIN;
  s->fn = fn;
  s->arg = arg;
  s->detached = attr ? attr->detached : 0;

  SceUID t = sceKernelCreateThread("pthread", thread_entry, 0x10000100, stack, 0, 0, NULL);
  if (t < 0) {
    free(s);
    return BIONIC_EAGAIN;
  }
  *thread = (uint32_t)t;
  {
    char b[64];
    so_log("[thread] create id %d fn %s stack %u", (int)t, where((void *)fn, b, sizeof(b)), (unsigned)stack);
  }
  void *p = s;
  if (sceKernelStartThread(t, sizeof(p), &p) < 0) {
    free(s);
    return BIONIC_EAGAIN;
  }
  return 0;
}

static int join_shim(uint32_t thread, void **ret) {
  int stat = 0;
  int waited = 0;
  for (;;) {
    SceUInt to = WAIT_SLICE_US;
    int r = sceKernelWaitThreadEnd((SceUID)thread, &stat, &to);
    if (r != WAIT_TIMEOUT_ERR)
      break;
    waited += 2;
    char b[64];
    if (wait_worth_logging(waited))
      so_log("[blocked] pthread_join(%d) for %ds, caller %s", (int)thread, waited, where(__builtin_return_address(0), b, sizeof(b)));
  }
  sceKernelDeleteThread((SceUID)thread);
  if (ret)
    *ret = (void *)(intptr_t)stat;
  return 0;
}

static int detach_shim(uint32_t thread) { (void)thread; return 0; }
static uint32_t self_shim(void) { return (uint32_t)sceKernelGetThreadId(); }

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

#define S(name, fn) { name, (uintptr_t)(fn) }

const so_default_dynlib shim_dynlib[] = {
  /* data */
  S("__sF", sF_buf),
  S("stderr", &stderr_ptr),
  S("__stack_chk_guard", &stack_chk_guard),

  /* logging, errors */
  S("__android_log_print", android_log_print),
  S("__android_log_write", android_log_write),
  S("android_set_abort_message", ret0),
  S("abort", abort_shim),
  S("exit", exit_shim),
  S("__assert2", assert2_shim),
  S("__stack_chk_fail", stack_chk_fail_shim),

  /* fortify + EABI helpers */
  S("__memcpy_chk", memcpy_chk_shim),
  S("__memset_chk", memset_chk_shim),
  S("__read_chk", read_chk_shim),
  S("__strlen_chk", strlen_chk_shim),
  S("__strcpy_chk", strcpy_chk_shim),
  S("__strcat_chk", strcat_chk_shim),
  S("__strncpy_chk2", strncpy_chk2_shim),
  S("__vsnprintf_chk", vsnprintf_chk_shim),
  S("__FD_SET_chk", fd_chk_shim),
  S("__FD_CLR_chk", fd_chk_shim),
  S("__aeabi_memset", aeabi_memset),
  S("__aeabi_memset4", aeabi_memset),
  S("__aeabi_memset8", aeabi_memset),
  S("__aeabi_memclr", aeabi_memclr),
  S("__aeabi_memclr4", aeabi_memclr),
  S("__aeabi_memclr8", aeabi_memclr),
  S("__aeabi_memcpy", aeabi_memcpy),
  S("__aeabi_memcpy4", aeabi_memcpy),
  S("__aeabi_memcpy8", aeabi_memcpy),
  S("__aeabi_memmove", aeabi_memmove),
  S("__aeabi_memmove4", aeabi_memmove),
  S("__aeabi_memmove8", aeabi_memmove),
  S("__cxa_atexit", cxa_atexit_shim),
  S("__cxa_finalize", cxa_finalize_shim),
  S("__register_atfork", register_atfork_shim),
  S("dl_unwind_find_exidx", dl_unwind_find_exidx_shim),
  S("__ctype_get_mb_cur_max", ctype_get_mb_cur_max_shim),
  S("posix_memalign", posix_memalign_shim),
  S("strerror_r", strerror_r_xsi_shim),

  /* stdio output (std streams go to the log) */
  S("fprintf", fprintf_shim),
  S("vfprintf", vfprintf_shim),
  S("fwrite", fwrite_shim),
  S("fputc", fputc_shim),
  S("fflush", fflush_shim),
  S("fputwc", fputwc_shim),
  S("fileno", fileno_shim),
  S("isatty", isatty_shim),
  S("printf", printf_shim),
  S("puts", puts_shim),
  S("putchar", putchar_shim),
  S("__errno", errno_shim),
  S("__cxa_thread_atexit_impl", cxa_thread_atexit_shim),

  /* dynamic loading */
  S("dlopen", dlopen_shim),
  S("dlsym", dlsym_shim),
  S("dlclose", dlclose_shim),

  /* system info */
  S("sysconf", sysconf_shim),
  S("getauxval", getauxval_shim),
  S("gettid", gettid_shim),
  S("syscall", syscall_shim),
  S("setpriority", ret0),
  S("getenv", getenv_shim),
  S("realpath", realpath_shim),
  S("sched_yield", sched_yield_shim),

  /* time */
  S("time", time_shim),
  S("localtime", localtime_shim),
  S("gmtime", gmtime_shim),
  S("ctime", ctime_shim),
  S("gettimeofday", gettimeofday_shim),
  S("clock_gettime", clock_gettime_shim),
  S("nanosleep", nanosleep_shim),

  /* files */
  S("fopen", fopen_shim),
  S("inflate", assets_inflate),
  S("inflateInit2_", assets_inflate_init2),
  S("inflateEnd", assets_inflate_end),
  S("access", access_shim),
  S("mkdir", mkdir_shim),
  S("opendir", opendir_shim),
  S("remove", remove_shim),
  S("rename", rename_shim),
  S("malloc", malloc_shim),
  S("calloc", calloc_shim),
  S("realloc", realloc_shim),
  S("stat", stat_shim),
  S("lstat", stat_shim),
  S("fstat", fstat_shim),
  S("open", open_shim),
  S("__open_2", open2_shim),
  S("openat", ret_m1),
  S("readdir", readdir_shim),
  S("fdopendir", ret_null),
  S("fdopen", ret_null),
  S("fcntl", ret0),
  S("ioctl", ret_m1),
  S("select", ret_m1),
  S("fchmod", ret0),
  S("fchmodat", ret0),
  S("ftruncate", ret_m1),
  S("truncate", ret_m1),
  S("link", ret_m1),
  S("symlink", ret_m1),
  S("readlink", ret_m1),
  S("unlinkat", ret_m1),
  S("utimensat", ret_m1),
  S("sendfile", ret_m1),
  S("statvfs", ret_m1),
  S("pathconf", ret_m1),

  /* sockets (FMOD's network profiler only): all fail */
  S("socket", ret_m1),
  S("bind", ret_m1),
  S("listen", ret_m1),
  S("accept", ret_m1),
  S("connect", ret_m1),
  S("shutdown", ret_m1),
  S("setsockopt", ret_m1),
  S("getsockopt", ret_m1),
  S("sendto", ret_m1),
  S("recvfrom", ret_m1),
  S("getaddrinfo", ret_m1),
  S("freeaddrinfo", ret0),

  /* syslog */
  S("openlog", ret0),
  S("syslog", ret0),
  S("closelog", ret0),

  /* assets */
  S("AAssetManager_fromJava", AAssetManager_fromJava_shim),
  S("AAssetManager_open", AAssetManager_open_shim),
  S("AAsset_close", assets_close),
  S("AAsset_read", assets_read),
  S("AAsset_seek", assets_seek),
  S("AAsset_getLength", assets_length),

  /* threads */
  S("pthread_create", create_shim),
  S("pthread_join", join_shim),
  S("pthread_detach", detach_shim),
  S("pthread_self", self_shim),
  S("pthread_once", once_shim),
  S("pthread_attr_init", attr_init_shim),
  S("pthread_attr_destroy", attr_destroy_shim),
  S("pthread_attr_setstacksize", attr_setstacksize_shim),
  S("pthread_attr_setdetachstate", attr_setdetachstate_shim),
  S("pthread_mutex_init", mutex_init_shim),
  S("pthread_mutex_destroy", mutex_destroy_shim),
  S("pthread_mutex_lock", mutex_lock_shim),
  S("pthread_mutex_unlock", mutex_unlock_shim),
  S("pthread_mutex_trylock", mutex_trylock_shim),
  S("pthread_mutexattr_init", mutexattr_init_shim),
  S("pthread_mutexattr_settype", mutexattr_settype_shim),
  S("pthread_mutexattr_destroy", mutexattr_destroy_shim),
  S("pthread_cond_init", cond_init_shim),
  S("pthread_cond_destroy", cond_destroy_shim),
  S("pthread_cond_wait", cond_wait_shim),
  S("pthread_cond_timedwait", cond_timedwait_shim),
  S("pthread_cond_signal", cond_signal_shim),
  S("pthread_cond_broadcast", cond_broadcast_shim),
  S("pthread_key_create", key_create_shim),
  S("pthread_key_delete", key_delete_shim),
  S("pthread_getspecific", getspecific_shim),
  S("pthread_setspecific", setspecific_shim),
  S("sem_init", sem_init_shim),
  S("sem_destroy", sem_destroy_shim),
  S("sem_post", sem_post_shim),
  S("sem_wait", sem_wait_shim),
};

const int shim_dynlib_count = sizeof(shim_dynlib) / sizeof(shim_dynlib[0]);
