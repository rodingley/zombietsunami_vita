/* gfx_vita.c -- screen (vitaGL) and touch input for stage 3 */

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <stdlib.h>
#include <string.h>
#ifdef REAL_GL
#include <vitaGL.h>
#endif

#include "gfx_vita.h"
#include "so_platform.h"

static int file_exists(const char *p) {
  SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0);
  if (f < 0)
    return 0;
  sceIoClose(f);
  return 1;
}

#ifdef REAL_GL
int gfx_init(int w, int h) {
  if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx")) {
    so_log("ERROR: libshacccg.suprx is not in ur0:data/ -- the game's shaders cannot be compiled.");
    so_log("Install it first (the same file the other vitaGL ports need), then start again.");
    return -1;
  }
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, 0, 0, 0);
  vglUseExtraMem(GL_TRUE);
  so_log("vglInit %dx%d", w, h);
  /* note: vitaGL returns GL_FALSE (0) when everything went fine.
   * First value: room for vitaGL's simple glBegin/glEnd drawing, used by the
   * "report bugs" pop-up (popup.c).  With 0 there is no room at all and the
   * first glVertex writes to address 0 (crash).  16 KB is plenty (the pop-up
   * uses 8 vertices of 96 bytes per frame). */
  vglInitExtended(16 * 1024, w, h, 24 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
  SceKernelFreeMemorySizeInfo mi;
  mi.size = sizeof(mi);
  if (sceKernelGetFreeMemorySize(&mi) >= 0)
    so_log("free memory after vglInit: user %d KB, cdram %d KB, phycont %d KB",
           mi.size_user / 1024, mi.size_cdram / 1024, mi.size_phycont / 1024);
  return 0;
}

#include "ime.h"
/* while a system dialog (the Vita keyboard) is up it is drawn over the frame */
void gfx_swap(void) { vglSwapBuffers(ime_busy() ? GL_TRUE : GL_FALSE); }

void gfx_log_errors(int frame) {
  GLenum e = glGetError();
  if (e != 0)
    so_log("[gl] frame %d glGetError = 0x%x", frame, (unsigned)e);
}

#else
int gfx_init(int w, int h) { (void)w; (void)h; return 0; }
void gfx_swap(void) {}
void gfx_log_errors(int frame) { (void)frame; }
#endif

void input_init(void) {
  sceMotionStartSampling();
  sceMotionSetDeadband(0);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
}

#define MAX_PTR 6
#define REAL_SLOTS 5 /* slot 5 is used for button-made touches */
#define BTN_SLOT 5
static int active[MAX_PTR];       /* report id + 1, or 0 when the slot is free */
static float last_x[MAX_PTR], last_y[MAX_PTR];

/* Controls:
 *   CROSS   while a run is going on: finger down on the right side of the screen while held
 *           (quick tap = short hop, hold = long jump, quick repeated taps = hover/feather)
 *   START   Android "Back" (pauses/unpauses a run)
 *   SQUARE  fake shake (the earthquake mission) -- the real motion sensors work too
 *   PS button: the run is paused (nativeOnPause) when the app is deactivated, and stays paused
 *   Touch screen: passed straight to the game.  Everything else does nothing. */
static unsigned prev_buttons;
static int btn_hold;
static int run_paused;
static int last_in_game;
static touch_fn saved_touch; /* remembered so the pop-up can let go of fingers */
static void *saved_ctx;
static int ignore_touch;     /* set after the pop-up closes: ignore the screen until it is let go */

/* tell the game every finger it thinks is down was lifted somewhere off the screen
 * (so nothing counts as a tap) */
void input_release_touches(void) {
  for (int s = 0; s < REAL_SLOTS; s++) {
    if (active[s] && saved_touch) {
      saved_touch(saved_ctx, s, 2, -200.0f, -200.0f);
      saved_touch(saved_ctx, s, 1, -200.0f, -200.0f);
    }
    active[s] = 0;
  }
}

void input_ignore_until_released(void) {
  ignore_touch = 1;
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);
  prev_buttons = pad.buttons; /* a button still held is not a new press */
}

int input_poll(touch_fn touch, void *ctx) {
  int result = 0;
  saved_touch = touch;
  saved_ctx = ctx;
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);
  unsigned pressed = pad.buttons & ~prev_buttons;
  prev_buttons = pad.buttons;

  extern volatile int g_in_game;
  if (g_in_game != last_in_game) {
    last_in_game = g_in_game;
    run_paused = 0;
  }
  extern volatile int g_game_starts;
  static int seen_starts;
  if (g_game_starts != seen_starts) { /* a run (re)started: surely not paused */
    seen_starts = g_game_starts;
    run_paused = 0;
  }
  /* Is a menu on screen?  The game asks for advert videos every few frames in
   * menus and (almost) never during a run (see hv_rewarded_video in jni.c).
   * Needed because the game does not always say when a run is over. */
  extern volatile unsigned g_menu_heartbeat;
  static unsigned hb_hist[20];
  static int hb_pos;
  unsigned hb_now = g_menu_heartbeat;
  int menu_on_screen = hb_now - hb_hist[hb_pos] >= 3; /* 3+ asks in the last 20 frames */
  hb_hist[hb_pos] = hb_now;
  hb_pos = (hb_pos + 1) % 20;
  int in_run = g_in_game && !menu_on_screen;
  static int last_in_run = -1;
  if (in_run != last_in_run) { /* rare: logged so the log shows how the controls saw the game */
    if (last_in_run >= 0)
      so_log("[input] %s", in_run ? "run: X jumps, START pauses" : "menu: X and START do nothing");
    last_in_run = in_run;
  }

  /* START pauses / unpauses a run.  Outside a run it does nothing (on the main
   * menu Android's "Back" would ask whether to quit the game). */
  if ((pressed & SCE_CTRL_START) && g_in_game && (run_paused || in_run)) {
    result |= 2; /* Back */
    run_paused = !run_paused;
  }
#ifdef REAL_GL
  /* PS button / home screen */
  SceAppMgrSystemEvent ev;
  while (sceAppMgrReceiveSystemEvent(&ev) >= 0) {
    /* going to the home screen pauses the run; coming back does NOT resume it */
    if (ev.systemEvent == 0x10000002 /* ON_DEACTIVATE; not named in older SDK headers */) {
      /* during a run, open the game's own pause menu (same as Start/Back), so coming
       * back from the home screen shows the pause menu instead of jumping straight in */
      if (in_run && !run_paused) {
        result |= 2;
        run_paused = 1;
      }
    }
  }
#endif

  /* X = jump, only while actually running (in menus it used to press whatever
   * sits at that spot, e.g. MARKET).  Not while the boosters bar is up at the
   * start of a run either: the tap landed on a booster.  X held while the bar
   * goes away waits to be let go, so it does not jump by itself. */
  extern int boosters_bar_up(void);
  static int x_wait_release;
  int x_down = (pad.buttons & SCE_CTRL_CROSS) != 0;
  int bar_up = in_run && boosters_bar_up();
  static int last_bar_up;
  if (bar_up != last_bar_up) {
    so_log("[input] boosters bar %s", bar_up ? "up: X does nothing (tap the boosters on the screen)" : "gone: X jumps");
    last_bar_up = bar_up;
  }
  if (bar_up && x_down)
    x_wait_release = 1;
  else if (!x_down)
    x_wait_release = 0;
  int want = x_down && !x_wait_release && !bar_up && in_run && !run_paused;
  if (want && !btn_hold) {
    touch(ctx, BTN_SLOT, 0, 700.0f, 272.0f);
    btn_hold = 1;
  } else if (!want && btn_hold) {
    touch(ctx, BTN_SLOT, 1, 700.0f, 272.0f);
    btn_hold = 0;
  }

  SceTouchData td;
  memset(&td, 0, sizeof(td));
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);

  if (ignore_touch) {
    if (td.reportNum > 0)
      return result;
    ignore_touch = 0;
  }

  int seen[MAX_PTR] = {0};
  for (int i = 0; i < td.reportNum && i < 6; i++) {
    int id = td.report[i].id + 1;
    float x = td.report[i].x * (960.0f / 1920.0f);
    float y = td.report[i].y * (544.0f / 1088.0f);
    int slot = -1, freeslot = -1;
    for (int s = 0; s < REAL_SLOTS; s++) {
      if (active[s] == id) slot = s;
      if (!active[s] && freeslot < 0) freeslot = s;
    }
    if (slot < 0) {
      if (freeslot < 0) continue;
      active[freeslot] = id;
      slot = freeslot;
      touch(ctx, slot, 0, x, y);
      /* the on-screen pause button is in the top-right corner: tapping it pauses (X is then switched off);
       * tapping anything else while paused is assumed to be the pause menu (e.g. Resume) */
      if (in_run && x >= 960.0f - 150.0f && y <= 150.0f)
        run_paused = !run_paused; /* the same corner pauses, and (when already paused) resumes */
      else
        run_paused = 0;
    } else if (x != last_x[slot] || y != last_y[slot]) {
      touch(ctx, slot, 2, x, y);
    }
    last_x[slot] = x;
    last_y[slot] = y;
    seen[slot] = 1;
  }
  for (int s = 0; s < REAL_SLOTS; s++) {
    if (active[s] && !seen[s]) {
      touch(ctx, s, 1, last_x[s], last_y[s]);
      active[s] = 0;
    }
  }
  return result;
}

void gfx_draw_cursor(void) {}
int input_menu_mode(void) { return 0; }

/* acceleration in m/s^2 (Android style); SQUARE fakes a shake */
static int fake_shake;
void input_get_accel(float *xyz) {
  SceMotionState ms;
  memset(&ms, 0, sizeof(ms));
  sceMotionGetState(&ms);
  xyz[0] = ms.acceleration.x * 9.80665f;
  xyz[1] = ms.acceleration.y * 9.80665f;
  xyz[2] = ms.acceleration.z * 9.80665f;
  if (prev_buttons & SCE_CTRL_SQUARE) {
    if (fake_shake == 0)
      fake_shake = 24;
  }
  if (fake_shake > 0) {
    xyz[0] += ((fake_shake / 3) & 1) ? 30.0f : -30.0f;
    fake_shake--;
  }
}
