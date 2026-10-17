/* main.c -- Zombie Tsunami for PS Vita, stage 3 (real graphics + touch) */

#include <vitasdk.h>
#include <kubridge.h>

#include <stdint.h>

#if defined(__has_include)
#if __has_include(<psp2/vshbridge.h>)
#include <psp2/vshbridge.h>
#define HAVE_VSHBRIDGE_HEADER 1
#endif
#endif
#ifndef HAVE_VSHBRIDGE_HEADER
int _vshKernelSearchModuleByName(const char *name, int *unk);
#endif

#include "config.h"
#include "so_platform.h"
#include "stage1.h"

void so_log_reset(void);
#ifdef CRASH_HANDLER
void crash_handler_install(void);
#endif

/* game code allocates from this heap */
int _newlib_heap_size_user = 128 * 1024 * 1024;

/* the game's code needs far more stack than the main thread has */
static int game_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  stage2b_run();
  return 0;
}

static void run_game_thread(void) {
  SceUID t = sceKernelCreateThread("game", game_thread, 0x10000100, 16 * 1024 * 1024, 0, 0, NULL);
  if (t < 0) {
    so_log("cannot create the game thread (%08x)", (unsigned)t);
    return;
  }
  sceKernelStartThread(t, 0, NULL);
  /* watchdog: if the game stops writing to the log but this line keeps
   * appearing, the game is stuck (hung); if it stops too, the app crashed */
  int seconds = 0;
  for (;;) {
    SceUInt to = 5 * 1000 * 1000;
    int r = sceKernelWaitThreadEnd(t, NULL, &to);
    if (r >= 0)
      break;
    seconds += 5;
    if (seconds <= 30 || seconds % 60 == 0)
      so_log("[watchdog] game thread still running after %d s", seconds);
    /* debugging aid: on purpose crash once so the Vita writes a dump that
     * shows what every thread of the game is doing */
    if (0 && seconds == 50) {
      so_log("[watchdog] making a crash dump on purpose now (this is expected)");
      sceKernelDelayThread(300 * 1000);
      *(volatile int *)0 = 1;
    }
  }
}

int main(int argc, char *argv[]) {
  so_log_reset();
  so_log("Zombie Tsunami Vita");

  int unk[2];
  if (_vshKernelSearchModuleByName("kubridge", unk) < 0)
    so_fatal("kubridge.skprx is not installed");
  so_log("kubridge found");
  /* run the CPU/GPU at full speed (the default is only 333 MHz) */
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);
  so_log("clocks: cpu %d MHz, bus %d MHz, gpu %d MHz", scePowerGetArmClockFrequency(),
         scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
#ifdef CRASH_HANDLER
  crash_handler_install();
#endif
  sceIoMkdir(DATA_PATH "/files", 0777);

  const uintptr_t addrs[NUM_GAME_LIBS] = {
    LOAD_ADDR_LIBCXX, LOAD_ADDR_FMOD, LOAD_ADDR_FMODSTUDIO, LOAD_ADDR_GAME,
  };

  int missing = stage1_run(LIB_PATH, addrs);
  if (missing < 0) {
    so_log("STAGE 2A FAILED while loading");
  } else {
    so_log("loader done (%d imports still missing)", missing);
    stage2a_run_inits();
    so_log("STAGE 2A DONE");
    run_game_thread();
    so_log("STAGE 2B DONE");
  }

  so_log_flush();
  sceKernelExitProcess(0);
  return 0;
}
