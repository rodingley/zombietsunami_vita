/* crash.c -- writes the CPU state to the log when the game crashes.
 *
 * Only built with  cmake -DCRASH_HANDLER=ON ..  because it needs a newer
 * kubridge.skprx (one that has kuKernelRegisterExceptionHandler).
 */

#include <stdint.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/types.h>

#include "so_platform.h"
#include "stage1.h"

typedef struct {
  SceUInt32 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
  SceUInt32 sp, lr, pc;
  SceUInt64 vfpRegisters[32];
  SceUInt32 SPSR, FPSCR, FPEXC, FSR, FAR, exceptionType;
} ZtExceptionContext;

typedef void (*ZtExceptionHandler)(ZtExceptionContext *);
typedef struct {
  SceSize size;
} ZtExceptionOpt;

extern int kuKernelRegisterExceptionHandler(SceUInt32 type, ZtExceptionHandler handler,
                                            ZtExceptionHandler *old, ZtExceptionOpt *opt);

static const char *where(uint32_t a, char *buf, size_t n) {
  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    uintptr_t b = game_mods[i].text_base;
    if (a >= b && a < b + game_mods[i].text_size) {
      snprintf(buf, n, "%s+0x%x", game_mods[i].name, (unsigned)(a - b));
      return buf;
    }
  }
  snprintf(buf, n, "0x%08x", (unsigned)a);
  return buf;
}

static volatile int in_handler;

static void crash_handler(ZtExceptionContext *c) {
  char b[64];
  if (in_handler++)
    sceKernelExitProcess(0);
  extern volatile int so_log_quiet;
  so_log_quiet = 0;
  so_log("=== CRASH (%s) ===",
         c->exceptionType == 0 ? "data abort" : c->exceptionType == 1 ? "prefetch abort" : "undefined instruction");
  so_log("pc  = %s", where(c->pc, b, sizeof(b)));
  so_log("lr  = %s", where(c->lr, b, sizeof(b)));
  so_log("sp  = %08x   fault address = %08x   FSR = %08x   SPSR = %08x", c->sp, c->FAR, c->FSR, c->SPSR);
  so_log("r0-r3  = %08x %08x %08x %08x", c->r0, c->r1, c->r2, c->r3);
  so_log("r4-r7  = %08x %08x %08x %08x", c->r4, c->r5, c->r6, c->r7);
  so_log("r8-r12 = %08x %08x %08x %08x %08x", c->r8, c->r9, c->r10, c->r11, c->r12);

  /* poor man's backtrace: stack words that point into the game's code */
  int found = 0;
  const uint32_t *sp = (const uint32_t *)(uintptr_t)c->sp;
  if (c->sp >= 0x1000 && (c->sp & 3) == 0) {
    for (int i = 0; i < 4096 && found < 32; i++) {
      uint32_t v = sp[i];
      for (int m = 0; m < NUM_GAME_LIBS; m++) {
        uintptr_t base = game_mods[m].text_base;
        if (v >= base && v < base + game_mods[m].text_size) {
          so_log("stack[+%03x] = %s", i * 4, where(v, b, sizeof(b)));
          found++;
          break;
        }
      }
    }
  }
  so_log("=== END OF CRASH REPORT ===");
  so_log_flush();
  sceKernelExitProcess(0);
}

void crash_handler_install(void) {
  int a = kuKernelRegisterExceptionHandler(0, crash_handler, NULL, NULL);
  int b = kuKernelRegisterExceptionHandler(1, crash_handler, NULL, NULL);
  int c = kuKernelRegisterExceptionHandler(2, crash_handler, NULL, NULL);
  so_log("crash handler installed (%08x %08x %08x)", (unsigned)a, (unsigned)b, (unsigned)c);
}
