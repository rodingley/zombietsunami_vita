/* stage2b.c -- start the game's Java-facing entry points with a fake Java VM.
 *
 * Order, as Android would do it:
 *   1. JNI_OnLoad of the libraries (they remember the JavaVM)
 *   2. MobiActivity setup natives (folders, screen density, safe area)
 *   3. GameViewRenderer.nativeInit / nativeResize
 *   4. a few nativeRender calls
 * Every step is logged BEFORE it runs, so the last line of the log shows
 * where a crash happened.
 */

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "config.h"
#include "gfx_vita.h"
#include "jni_fake.h"
#include "popup.h"
#include "assets.h"
#include "ime.h"
#include "so_platform.h"
#include "stage1.h"

#define LIBCGAME 3

static void *lookup(int lib, const char *name) {
  uintptr_t a = so_symbol(&game_mods[lib], name);
  if (!a)
    so_log("symbol not found: %s", name);
  return (void *)a;
}

static void call_onload(int lib) {
  void *f = lookup(lib, "JNI_OnLoad");
  if (!f)
    return;
  so_log(">> JNI_OnLoad of %s", game_mods[lib].name);
  int ver = ((int (*)(void *, void *))f)(jni_vm(), NULL);
  so_log("<< JNI_OnLoad of %s returned 0x%x", game_mods[lib].name, ver);
}

typedef struct {
  void *env, *renderer;
  void (*fn)(void *, void *, int, int, float, float);
} touch_ctx;


static void send_touch(void *vctx, int id, int action, float x, float y) {
  touch_ctx *c = vctx;
  if (c->fn)
    c->fn(c->env, c->renderer, id, action, x, y);
}

static void log_scale(const char *tag) {
  /* debugging: libcgame globals that decide UI scale (offsets inside the .so) */
  volatile uint32_t *dpi = (volatile uint32_t *)(0x9B000000u + 0x395834u);
  volatile uint32_t *sc = (volatile uint32_t *)(0x9B000000u + 0x3bd374u);
  volatile uint32_t *st = (volatile uint32_t *)(0x9B000000u + 0x3bd364u);
  float fsc, fdpi;
  uint32_t a = *sc, b = *dpi;
  memcpy(&fsc, &a, 4);
  memcpy(&fdpi, &b, 4);
  so_log("[scale] %s: dpi=%g (0x%08x) scale=%g (0x%08x) struct364: %08x %08x %08x %08x %08x", tag, (double)fdpi,
         (unsigned)b, (double)fsc, (unsigned)a, (unsigned)st[0], (unsigned)st[1], (unsigned)st[2], (unsigned)st[3],
         (unsigned)st[4]);
}

void stage2b_run(void) {
  if (gfx_init(960, 544) < 0) {
    so_log("graphics init FAILED -- stopping here (see the message above)");
    return;
  }
  input_init();
  popup_preload();
  credit_preload();
  jni_init();
  void *env = jni_env();

  call_onload(1); /* libfmod */
  call_onload(LIBCGAME);

  jobj *activity_cls = jni_find_class("net/mobigame/artemis/MobiActivity");
  jobj *activity = jni_new_object("net/mobigame/artemis/MobiActivity");
  jobj *renderer = jni_new_object("net/mobigame/artemis/GameViewRenderer");
  const char *prefix = "Java_net_mobigame_artemis_";
  char name[160];
  void *f;

  snprintf(name, sizeof(name), "%sMobiActivity_nativeSetFilesDir", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeSetFilesDir");
    ((void (*)(void *, void *, void *))f)(env, activity_cls, jni_new_string(DATA_PATH "/files"));
  }
  snprintf(name, sizeof(name), "%sMobiActivity_nativeSetExternalFilesDir", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeSetExternalFilesDir");
    ((void (*)(void *, void *, void *))f)(env, activity_cls, jni_new_string(DATA_PATH "/files"));
  }
  snprintf(name, sizeof(name), "%sMobiActivity_nativeSetDisplayDpi", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeSetDisplayDpi");
    /* the game stores this raw and later divides it as an INTEGER by 136 to get
     * its UI scale, so it must be an int.  231 -> scale 1.70 -> the game lays out
     * a 565x320 screen, the smallest height its menus are designed for (320, like
     * every phone).  272 (scale 2, 480x272) cut off the bottom of menu backgrounds. */
    ((void (*)(void *, void *, int))f)(env, activity_cls, 231);
  }
  snprintf(name, sizeof(name), "%sMobiActivity_nativeSetSafeAreaInsets", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeSetSafeAreaInsets");
    ((void (*)(void *, void *, int, int, int, int))f)(env, activity_cls, 0, 0, 0, 0);
  }
  snprintf(name, sizeof(name), "%sMobiActivity_nativeMakeFilesDirIfNeeded", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeMakeFilesDirIfNeeded");
    ((void (*)(void *, void *))f)(env, activity_cls);
  }

  /* The Java side asks the native engine for its OpenGL version before the
   * renderer starts.  That call also creates the engine's render object, so
   * nativeInit crashes (NULL object) if it is skipped. */
  snprintf(name, sizeof(name), "%sGameGLSurfaceView_nativeGetOpenGLVersion", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    jobj *glview = jni_new_object("net/mobigame/artemis/GameGLSurfaceView");
    so_log(">> nativeGetOpenGLVersion");
    ((void (*)(void *, void *))f)(env, glview);
    so_log("<< nativeGetOpenGLVersion returned");
  }

  snprintf(name, sizeof(name), "%sGameViewRenderer_nativeInit", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    log_scale("before nativeInit");
    so_log(">> nativeInit(960, 544)");
    ((void (*)(void *, void *, void *, int, int))f)(env, renderer, activity, 960, 544);
    so_log("<< nativeInit returned");
    log_scale("after nativeInit");
  }
  snprintf(name, sizeof(name), "%sGameViewRenderer_nativeResize", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    so_log(">> nativeResize(960, 544)");
    ((void (*)(void *, void *, int, int))f)(env, renderer, 960, 544);
    so_log("<< nativeResize returned");
    log_scale("after nativeResize");
  }
  touch_ctx tc = { env, renderer, NULL };
  snprintf(name, sizeof(name), "%sGameViewRenderer_nativeTouch", prefix);
  tc.fn = lookup(LIBCGAME, name);

  snprintf(name, sizeof(name), "%sGameViewRenderer_nativeRender", prefix);
  if ((f = lookup(LIBCGAME, name))) {
    /* Run frames for as long as the game runs (no button combination quits it). */
    void (*back_fn)(void *, void *) = NULL;
    snprintf(name, sizeof(name), "%sMobiActivity_nativeOnBackPressed", prefix);
    back_fn = (void (*)(void *, void *))so_symbol(&game_mods[LIBCGAME], name);
    void (*pause_fn)(void *, void *) = NULL, (*resume_fn)(void *, void *) = NULL;
    snprintf(name, sizeof(name), "%sGameViewRenderer_nativeOnPause", prefix);
    pause_fn = (void (*)(void *, void *))so_symbol(&game_mods[LIBCGAME], name);
    snprintf(name, sizeof(name), "%sGameViewRenderer_nativeOnResume", prefix);
    resume_fn = (void (*)(void *, void *))so_symbol(&game_mods[LIBCGAME], name);
    void (*accel_fn)(void *, void *, float, float, float) = NULL;
    snprintf(name, sizeof(name), "%sMobiActivity_nativeSetAcceleration", prefix);
    accel_fn = (void (*)(void *, void *, float, float, float))so_symbol(&game_mods[LIBCGAME], name);
    /* real-money purchases are answered "failed" (see hv_purchase in jni.c).
     * The answer goes to the game's purchase listener, which it creates while
     * loading; it is only given when that listener is really there. */
    void (*purchase_failed_fn)(void *, void *, void *) = (void (*)(void *, void *, void *))so_symbol(
        &game_mods[LIBCGAME], "Java_net_mobigame_artemis_billing_MobiGooglePlayBilling_nativeOnPurchaseFailed");
    void **purchase_listener = NULL;
    {
      uintptr_t base = game_mods[LIBCGAME].text_base;
      if (*(volatile uintptr_t *)(base + 0x38ec80) == base + 0x3bc4ac) /* this game version's listener pointer */
        purchase_listener = (void **)(base + 0x3bc4ac);
      else
        so_log("[shop] purchase listener not found: purchases will not be answered");
    }
    /* the daily reward (3 diamonds a day, by the Vita's own clock).  The game
     * keeps it switched off until Mobigame's server sends its settings, and only
     * looks for a new day right after the server answers -- which never happens
     * here.  So, once loading is done and then once a minute,
     * the daily-reward setting is switched on and the game's own new-day check
     * is run; the game then shows its own "Daily reward" pop-up as usual.
     * (Nothing else reads that setting.)  Only done when this game version's
     * code is exactly where expected. */
    void *(*settings_get)(void) = NULL, *(*daily_get)(void) = NULL;
    void (*daily_check)(void *) = NULL;
    uint64_t daily_last = 0;
    int daily_logged = 0;
    {
      uintptr_t base = game_mods[LIBCGAME].text_base;
      static const uint16_t sig_settings[] = {0x4801, 0x4478, 0x6800, 0x4770}; /* settings object getter */
      static const uint16_t sig_daily[] = {0x4801, 0x4478, 0x6840, 0x4770};    /* daily reward object getter */
      static const uint16_t sig_check[] = {0xb5f0, 0xaf03, 0xf84d, 0xbd04};    /* its new-day check */
      if (!memcmp((const void *)(base + 0xaf10c), sig_settings, sizeof(sig_settings)) &&
          !memcmp((const void *)(base + 0x9baa8), sig_daily, sizeof(sig_daily)) &&
          !memcmp((const void *)(base + 0x9bea4), sig_check, sizeof(sig_check))) {
        settings_get = (void *(*)(void))(base + 0xaf10c + 1);
        daily_get = (void *(*)(void))(base + 0x9baa8 + 1);
        daily_check = (void (*)(void *))(base + 0x9bea4 + 1);
      } else {
        so_log("[daily] daily reward code not found: left as it is");
      }
    }
    uint64_t t_last = sceKernelGetProcessTimeWide(), t_render = 0, t_swap = 0;
    uint64_t t_prev_end = t_last, worst_frame = 0, worst_game = 0;
    extern volatile uint64_t g_load_read_us, g_load_unzip_us, g_load_upload_us;
    uint64_t ld_read0 = g_load_read_us, ld_unzip0 = g_load_unzip_us, ld_upload0 = g_load_upload_us;
    int fps_frames = 0, hitches = 0, slow_logged = 0;
    uint64_t upload_frame0 = g_load_upload_us;
    g_game_thread = sceKernelGetThreadId();
    for (int i = 0;; i++) {
      int logit = (i < 5 || i % 600 == 0);
      if (i < 400 && i % 100 == 0)
        log_scale("frame");
      if (i % 1800 == 1799)
        jni_dump_stats();
      /* read the controls first, so a tap is seen by this frame */
      extern volatile int g_in_game;
      extern volatile int so_log_quiet;
      if (g_in_game && !so_log_quiet) {
        so_log("first run started: from now on only the fps lines are logged (less lag)");
        so_log_quiet = 1;
      }
      /* the Vita keyboard / message for the promo code box (ime.c) */
      ime_frame(env);
      /* while the "report bugs" pop-up or a system dialog is showing, the game gets no input */
      if (!popup_frame_start() && !ime_busy()) {
        int in = input_poll(send_touch, &tc);
        if ((in & 2) && back_fn)
          back_fn(env, activity_cls);
        if ((in & 4) && pause_fn)
          pause_fn(env, renderer);
      }

      {
        char sku[96];
        if (jni_take_failed_purchase(sku, sizeof(sku))) {
          if (purchase_failed_fn && purchase_listener && *purchase_listener) {
            purchase_failed_fn(env, jni_find_class("net/mobigame/artemis/billing/MobiGooglePlayBilling"),
                               jni_new_string(sku));
            so_log("[shop] the game was told the purchase of \"%s\" failed", sku);
          } else {
            so_log("[shop] could not answer the purchase of \"%s\" (no purchase listener)", sku);
          }
        }
      }

      {
        extern volatile int g_loading_done;
        uint64_t now = sceKernelGetProcessTimeWide();
        if (daily_check && g_loading_done && (!daily_last || now - daily_last >= 60 * 1000000ull)) {
          daily_last = now ? now : 1;
          char *settings = (char *)settings_get();
          unsigned char *daily = (unsigned char *)daily_get();
          if (settings && daily) {
            float *enabled = (float *)(settings + 76);
            if (*enabled == 0.0f)
              *enabled = 1.0f;
            daily_check(daily);
            static int daily_first = 1;
            if (daily_first) {
              daily_first = 0;
              if (!daily[0])
                so_log("[daily] checked: no reward waiting right now (a new one comes each day)");
            }
            if (daily[0] && !daily_logged) {
              so_log("[daily] a daily reward is waiting (the game shows it in the main menu)");
              daily_logged = 1;
            } else if (!daily[0]) {
              daily_logged = 0;
            }
          }
        }
      }

      if (accel_fn) {
        float a[3];
        input_get_accel(a);
        accel_fn(env, activity_cls, a[0], a[1], a[2]);
      }
      if (logit)
        so_log(">> nativeRender #%d", i);
      uint64_t a = sceKernelGetProcessTimeWide();
      ((void (*)(void *, void *))f)(env, renderer);
      uint64_t b = sceKernelGetProcessTimeWide();
      gfx_draw_cursor();
      credit_draw();
      popup_draw();
      gfx_swap();
      uint64_t c = sceKernelGetProcessTimeWide();
      t_render += b - a;
      t_swap += c - b;
      fps_frames++;
      /* smoothness: a frame that took more than ~1.5 screen refreshes is a visible hitch */
      uint64_t frame_us = c - t_prev_end;
      t_prev_end = c;
      if (frame_us > worst_frame)
        worst_frame = frame_us;
      if (b - a > worst_game)
        worst_game = b - a;
      if (frame_us > 25000)
        hitches++;
      /* a frame the game itself took long over: say what it was doing */
      {
        char files[160];
        assets_frame_report(files, sizeof(files));
        uint64_t up = g_load_upload_us - upload_frame0;
        upload_frame0 = g_load_upload_us;
        if (b - a > 100000 && slow_logged < 300) {
          slow_logged++;
          size_t used = strlen(files);
          if (up >= 1000 && used < sizeof(files))
            snprintf(files + used, sizeof(files) - used, "%s pictures to the GPU %u ms", used ? "," : " --",
                     (unsigned)(up / 1000));
          so_log("[fps] one slow frame: the game took %u ms%s", (unsigned)((b - a) / 1000), files);
        }
      }
      if (logit) {
        so_log("<< nativeRender #%d returned", i);
        gfx_log_errors(i);
      }
      if (c - t_last >= 10 * 1000000ull) {
        uint64_t ld_read = g_load_read_us, ld_unzip = g_load_unzip_us, ld_upload = g_load_upload_us;
        so_log("[fps] %.1f fps, game %.1f ms (worst %.1f), swap/vsync %.1f ms per frame, %d hitches (worst frame %.1f ms)%s"
               " | loading: read %u ms, unzip %u ms, to GPU %u ms",
               fps_frames * 1e6 / (double)(c - t_last), t_render / 1000.0 / fps_frames, worst_game / 1000.0,
               t_swap / 1000.0 / fps_frames, hitches, worst_frame / 1000.0, g_in_game ? " [run]" : " [menu]",
               (unsigned)((ld_read - ld_read0) / 1000), (unsigned)((ld_unzip - ld_unzip0) / 1000),
               (unsigned)((ld_upload - ld_upload0) / 1000));
        ld_read0 = ld_read;
        ld_unzip0 = ld_unzip;
        ld_upload0 = ld_upload;
        static int windows;
        if (windows++ % 6 == 0) { /* once a minute: how full the game's memory is */
          struct mallinfo mi = mallinfo();
          so_log("[fps] memory: %u MB of the 128 MB heap in use", (unsigned)(mi.uordblks / (1024 * 1024)));
        }
        t_last = c;
        t_render = t_swap = 0;
        fps_frames = hitches = 0;
        worst_frame = worst_game = 0;
      }
    }
  }
  so_log("STAGE 2B STEPS DONE");
}
