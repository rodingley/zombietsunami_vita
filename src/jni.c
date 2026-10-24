/* jni.c -- a fake Java VM for the game.
 *
 * The game's native code was written to be called from Java and also calls
 * back into Java (to ask for the device name, show ads, open URLs ...).
 * There is no Java on the Vita, so this file pretends to be one:
 *   - classes, strings, arrays and method ids are small C structs
 *   - every JNIEnv function exists; the ones nobody has implemented yet only
 *     write a line to the log and return 0
 *   - every call from the game into Java is logged (first few times per
 *     method) and answered by a small handler table (java_handlers below)
 *
 * The log therefore tells us exactly which Java features the game uses.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_fake.h"
#include "popup.h"
#include "so_platform.h"

#define JMAGIC 0x4a4e4958u

enum { K_CLASS = 1, K_STRING, K_OBJECT, K_ARRAY };

typedef union {
  int32_t i;
  int64_t j;
  float f;
  double d;
  void *l;
} jv; /* same size/layout as the JNI jvalue union */

struct jobj {
  uint32_t magic;
  int kind;
  char *name;  /* class name, or string contents (UTF-8) */
  jobj *cls;   /* class of an object */
  void *data;  /* array / direct buffer storage */
  int len;     /* array length / buffer capacity */
  int elem;    /* array element size */
};

typedef struct {
  uint32_t magic;
  jobj *cls;
  char name[64];
  char sig[200];
  int is_static;
  int ncalls;
} jmethod;

typedef struct {
  uint32_t magic;
  jobj *cls;
  char name[64];
  char sig[64];
  int is_static;
} jfield;

static intptr_t jni_unimpl(int idx);
static void jni_dump_stats_impl(void);
void jni_dump_stats(void) { jni_dump_stats_impl(); }
#include "jni_gen.inc"

static void *jni_table[JNI_NUM_FUNCS];
static void *jni_env_ptr = jni_table;
static void *vm_table[8];
static void *vm_ptr = vm_table;

/* ------------------------------------------------------------------ */
/* object helpers                                                       */
/* ------------------------------------------------------------------ */

static jobj *obj_new(int kind, const char *name, jobj *cls) {
  jobj *o = calloc(1, sizeof(jobj));
  if (!o)
    so_fatal("out of memory (jni object)");
  o->magic = JMAGIC;
  o->kind = kind;
  o->name = name ? strdup(name) : NULL;
  o->cls = cls;
  return o;
}

static int is_obj(const void *p) {
  return p && ((const jobj *)p)->magic == JMAGIC;
}

#define MAX_CLASSES 256
static jobj *classes[MAX_CLASSES];
static int num_classes;

jobj *jni_find_class(const char *name) {
  for (int i = 0; i < num_classes; i++)
    if (strcmp(classes[i]->name, name) == 0)
      return classes[i];
  if (num_classes >= MAX_CLASSES)
    so_fatal("too many java classes");
  jobj *c = obj_new(K_CLASS, name, NULL);
  classes[num_classes++] = c;
  return c;
}

jobj *jni_new_object(const char *class_name) {
  return obj_new(K_OBJECT, NULL, jni_find_class(class_name));
}

jobj *jni_new_string(const char *utf8) {
  return obj_new(K_STRING, utf8 ? utf8 : "", jni_find_class("java/lang/String"));
}

static const char *cname(const jobj *o) {
  if (!is_obj(o))
    return "?";
  if (o->kind == K_CLASS)
    return o->name;
  if (o->cls)
    return o->cls->name;
  return "?";
}

/* ------------------------------------------------------------------ */
/* method / field ids                                                   */
/* ------------------------------------------------------------------ */

#define MAX_METHODS 1024
static jmethod *methods[MAX_METHODS];
static int num_methods;

static void jni_dump_stats_impl(void) {
  for (int i = 0; i < num_methods; i++)
    if (methods[i]->ncalls > 3)
      so_log("[jni-stats] %s.%s called %d times", cname(methods[i]->cls), methods[i]->name,
             methods[i]->ncalls);
}

static jmethod *get_method(jobj *cls, const char *name, const char *sig, int is_static) {
  for (int i = 0; i < num_methods; i++) {
    jmethod *m = methods[i];
    if (m->cls == cls && m->is_static == is_static && !strcmp(m->name, name) &&
        !strcmp(m->sig, sig))
      return m;
  }
  if (num_methods >= MAX_METHODS)
    so_fatal("too many java methods");
  jmethod *m = calloc(1, sizeof(jmethod));
  m->magic = JMAGIC;
  m->cls = cls;
  m->is_static = is_static;
  snprintf(m->name, sizeof(m->name), "%s", name);
  snprintf(m->sig, sizeof(m->sig), "%s", sig);
  methods[num_methods++] = m;
  so_log("[jni] %sMethodID %s.%s%s", is_static ? "static " : "", cname(cls), m->name, m->sig);
  return m;
}

/* ------------------------------------------------------------------ */
/* handlers for calls the game makes INTO Java                          */
/* ------------------------------------------------------------------ */

typedef jv (*java_handler)(jmethod *m, jobj *obj, jv *args);

static jv hv_zero(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}

static jv hv_load_class(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o;
  jv r;
  memset(&r, 0, sizeof(r));
  if (is_obj(a[0].l) && ((jobj *)a[0].l)->kind == K_STRING) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", ((jobj *)a[0].l)->name);
    for (char *p = buf; *p; p++)
      if (*p == '.')
        *p = '/';
    r.l = jni_find_class(buf);
  }
  return r;
}

static jv hv_class_loader(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.l = jni_new_object("java/lang/ClassLoader");
  return r;
}

static jv hv_sdk_version(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.i = 28;
  return r;
}

static jv hv_renderer(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.l = jni_new_string("PowerVR SGX543MP4+");
  return r;
}

static jv hv_build_number(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.l = jni_new_string("224");
  return r;
}

static jv hv_bundle_version(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.l = jni_new_string("4.7.3");
  return r;
}

static jv hv_rate48k(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; jv r; memset(&r, 0, sizeof(r)); r.i = 48000; return r; }
static jv hv_block1k(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; jv r; memset(&r, 0, sizeof(r)); r.i = 1024; return r; }
static jv hv_true(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; jv r; memset(&r, 0, sizeof(r)); r.i = 1; return r; }

volatile int g_in_game;
volatile int g_game_starts; /* counts trackEventStartGame calls (new run / restart) */ /* 1 while a run is going on (set from the game's tracking calls) */
static jv hv_game_start(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; g_in_game = 1; g_game_starts++; jv r; memset(&r, 0, sizeof(r)); return r; }
/* The game asks "is there an advert video to offer?" every few frames while a
 * menu (main menu, shop, game-over screen...) is on screen, and almost never
 * during a run.  That is the most reliable "we are in a menu" signal we have:
 * the game does not always tell us when a run ends (quitting from the pause
 * menu says nothing).  The answer stays "no". */
volatile unsigned g_menu_heartbeat;
static jv hv_rewarded_video(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  g_menu_heartbeat++;
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}
/* the game's loading is finished: get the files the menus need first ready
 * in memory in the background (asset_cache.c).  The game's own message comes
 * a little before the loading screen goes away, while the memory card has
 * nothing else to do; setNativeLoadingDone is the fallback. */
void acache_prewarm(void);
void acache_capture_start(void);
static jv hv_crash_log(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o;
  const char *msg = (a && is_obj(a[0].l) && ((jobj *)a[0].l)->kind == K_STRING) ? ((jobj *)a[0].l)->name : NULL;
  if (msg && strstr(msg, "LoadGameAsync() done"))
    acache_prewarm();
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}
volatile int g_loading_done; /* the game has finished loading (stage2b.c: daily reward check) */
static jv hv_loading_done(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o; (void)a;
  g_loading_done = 1;
  acache_prewarm();
  acache_capture_start();
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}
static jv hv_game_over(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; g_in_game = 0; jv r; memset(&r, 0, sizeof(r)); return r; }

static jv hv_int(jmethod *m, jobj *o, jv *a, int value) {
  (void)m; (void)o; (void)a;
  jv r;
  memset(&r, 0, sizeof(r));
  r.i = value;
  return r;
}

static jv hv_str(const char *text) {
  jv r;
  memset(&r, 0, sizeof(r));
  r.l = jni_new_string(text);
  return r;
}

static jv hv_device_model(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; return hv_str("PlayStation Vita"); }
static jv hv_memory_class(jmethod *m, jobj *o, jv *a) { return hv_int(m, o, a, 256); }
static jv hv_empty_string(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; return hv_str(""); }
static jv hv_lang(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; return hv_str("en"); }

/* MobiActivity.openUrl(String): the Vita has no web browser to hand pages to.
 * The menu's top-right button (the Bluesky one, now showing the game's warning
 * sign -- see game_art.c ga_menu_icon) opens the "report bugs" pop-up instead. */
static jv hv_open_url(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o;
  jv r;
  memset(&r, 0, sizeof(r));
  const char *url = (a && is_obj(a[0].l) && ((jobj *)a[0].l)->kind == K_STRING) ? ((jobj *)a[0].l)->name : NULL;
  if (url && strstr(url, "bsky.app"))
    popup_request();
  return r;
}


/* MobiActivity.purchase / purchaseWithGoogle(String sku, boolean, String):
 * buying with real money.  There is no store on the Vita, and the game waits
 * (behind its "please wait" pop-up, which cannot be closed) until the store
 * answers.  So every such purchase fails at once, the way a cancelled one does
 * on Android: the answer (MobiGooglePlayBilling.nativeOnPurchaseFailed) is
 * given at the start of the next frame, on the game's thread, as Android does
 * (stage2b.c).  Nothing is bought and the game carries on. */
static char failed_sku[96];
static volatile int failed_pending;
static jv hv_purchase(jmethod *m, jobj *o, jv *a) {
  (void)m; (void)o;
  const char *sku = (a && is_obj(a[0].l) && ((jobj *)a[0].l)->kind == K_STRING) ? ((jobj *)a[0].l)->name : "";
  snprintf(failed_sku, sizeof(failed_sku), "%s", sku);
  failed_pending = 1;
  so_log("[shop] \"%s\" costs real money: the purchase is answered with \"failed\" (no store on the Vita)", failed_sku);
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}

int jni_take_failed_purchase(char *sku, unsigned n) {
  if (!failed_pending)
    return 0;
  failed_pending = 0;
  snprintf(sku, n, "%s", failed_sku);
  return 1;
}

/* GameGLSurfaceView.openIMEKeyboard / closeIMEKeyboard: the game's promo code
 * box wants the on-screen keyboard -- the Vita's own one (ime.c) */
#include "ime.h"
static jv hv_ime_open(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; ime_request_open(); jv r; memset(&r, 0, sizeof(r)); return r; }
static jv hv_ime_close(jmethod *m, jobj *o, jv *a) { (void)m; (void)o; (void)a; ime_request_close(); jv r; memset(&r, 0, sizeof(r)); return r; }

const char *jni_string_utf(void *s) {
  return (s && is_obj((jobj *)s) && ((jobj *)s)->kind == K_STRING) ? ((jobj *)s)->name : NULL;
}

static const struct {
  const char *name;
  java_handler fn;
} java_handlers[] = {
  { "openIMEKeyboard", hv_ime_open },
  { "closeIMEKeyboard", hv_ime_close },
  { "purchase", hv_purchase },
  { "purchaseWithGoogle", hv_purchase },
  { "loadClass", hv_load_class },
  { "findClass", hv_load_class },
  { "getDeviceModel", hv_device_model },
  { "getMemoryClass", hv_memory_class },
  { "getLanguage", hv_lang },
  { "getCountry", hv_empty_string },
  { "getClassLoader", hv_class_loader },
  { "GetAndroidSdkVersion", hv_sdk_version },
  { "getRendererString", hv_renderer },
  { "getBuildNumberAsString", hv_build_number },
  { "getBundleVersion", hv_bundle_version },
  { "trackEventStartGame", hv_game_start },
  { "trackEventGameOver", hv_game_over },
  { "IsConnected", hv_zero },
  { "HasRewardedVideo", hv_rewarded_video },
  { "setNativeLoadingDone", hv_loading_done },
  { "LogMessageToCrashReporter", hv_crash_log },
  { "getOutputSampleRate", hv_rate48k },
  { "getOutputBlockSize", hv_block1k },
  { "checkInit", hv_true },
  { "getDisplayRotation", hv_zero },
  { "openUrl", hv_open_url },
};

static void describe_args(const char *sig, const jv *a, char *out, size_t n) {
  size_t used = 0;
  int k = 0;
  const char *p = sig + 1;
  out[0] = '\0';
  while (*p && *p != ')' && k < 12) {
    char t = *p;
    char tmp[120];
    tmp[0] = '\0';
    if (t == 'L') {
      while (*p != ';')
        p++;
      if (a[k].l && is_obj(a[k].l) && ((jobj *)a[k].l)->kind == K_STRING)
        snprintf(tmp, sizeof(tmp), "\"%.80s\"", ((jobj *)a[k].l)->name);
      else
        snprintf(tmp, sizeof(tmp), "obj%s", a[k].l ? "" : "=null");
    } else if (t == '[') {
      while (*p == '[')
        p++;
      if (*p == 'L')
        while (*p != ';')
          p++;
      snprintf(tmp, sizeof(tmp), "arr");
    } else if (t == 'J') {
      snprintf(tmp, sizeof(tmp), "%lld", (long long)a[k].j);
    } else if (t == 'D') {
      snprintf(tmp, sizeof(tmp), "%g", a[k].d);
    } else if (t == 'F') {
      snprintf(tmp, sizeof(tmp), "%g", (double)a[k].f);
    } else {
      snprintf(tmp, sizeof(tmp), "%d", (int)a[k].i);
    }
    p++;
    k++;
    size_t l = strlen(tmp);
    if (used + l + 2 < n) {
      memcpy(out + used, tmp, l);
      used += l;
      out[used++] = ' ';
      out[used] = '\0';
    }
  }
}

/* Decode the arguments (from a va_list or a jvalue array), log the call
 * and run the handler. */
static jv do_call(jmethod *m, jobj *obj, va_list *ap, const jv *argv) {
  jv args[16];
  memset(args, 0, sizeof(args));
  const char *p = m->sig + 1;
  int k = 0;
  while (*p && *p != ')' && k < 16) {
    char t = *p;
    if (t == 'L') {
      while (*p != ';')
        p++;
    } else if (t == '[') {
      while (*p == '[')
        p++;
      if (*p == 'L')
        while (*p != ';')
          p++;
      t = 'L';
    }
    if (argv) {
      args[k] = argv[k];
    } else if (t == 'J') {
      args[k].j = va_arg(*ap, int64_t);
    } else if (t == 'D' || t == 'F') {
      args[k].d = va_arg(*ap, double);
      if (t == 'F')
        args[k].f = (float)args[k].d;
    } else if (t == 'L') {
      args[k].l = va_arg(*ap, void *);
    } else {
      args[k].i = va_arg(*ap, int);
    }
    p++;
    k++;
  }

  if (m->ncalls < 3) {
    char desc[400];
    describe_args(m->sig, args, desc, sizeof(desc));
    so_log("[jni] call %s.%s%s ( %s)", cname(m->cls), m->name, m->sig, desc);
  }
  m->ncalls++;

  for (unsigned i = 0; i < sizeof(java_handlers) / sizeof(java_handlers[0]); i++)
    if (!strcmp(java_handlers[i].name, m->name))
      return java_handlers[i].fn(m, obj, args);
  jv r;
  memset(&r, 0, sizeof(r));
  return r;
}

static jv call_v(jmethod *m, jobj *o, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  jv r = do_call(m, o, &cp, NULL);
  va_end(cp);
  return r;
}

/* ------------------------------------------------------------------ */
/* typed Call*Method families (instance, nonvirtual, static)            */
/* ------------------------------------------------------------------ */

#define CALL_FAMILY(NAME, RET, FIELD)                                                             \
  static RET Call##NAME##Method(void *env, jobj *obj, jmethod *m, ...) {                          \
    (void)env;                                                                                    \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    jv r = do_call(m, obj, &ap, NULL);                                                            \
    va_end(ap);                                                                                   \
    return r.FIELD;                                                                               \
  }                                                                                               \
  static RET Call##NAME##MethodV(void *env, jobj *obj, jmethod *m, va_list ap) {                  \
    (void)env;                                                                                    \
    return call_v(m, obj, ap).FIELD;                                                      \
  }                                                                                               \
  static RET Call##NAME##MethodA(void *env, jobj *obj, jmethod *m, const jv *args) {              \
    (void)env;                                                                                    \
    return do_call(m, obj, NULL, args).FIELD;                                                     \
  }                                                                                               \
  static RET CallNonvirtual##NAME##Method(void *env, jobj *obj, jobj *cls, jmethod *m, ...) {     \
    (void)env; (void)cls;                                                                         \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    jv r = do_call(m, obj, &ap, NULL);                                                            \
    va_end(ap);                                                                                   \
    return r.FIELD;                                                                               \
  }                                                                                               \
  static RET CallNonvirtual##NAME##MethodV(void *env, jobj *obj, jobj *cls, jmethod *m,           \
                                           va_list ap) {                                          \
    (void)env; (void)cls;                                                                         \
    return call_v(m, obj, ap).FIELD;                                                      \
  }                                                                                               \
  static RET CallNonvirtual##NAME##MethodA(void *env, jobj *obj, jobj *cls, jmethod *m,           \
                                           const jv *args) {                                      \
    (void)env; (void)cls;                                                                         \
    return do_call(m, obj, NULL, args).FIELD;                                                     \
  }                                                                                               \
  static RET CallStatic##NAME##Method(void *env, jobj *cls, jmethod *m, ...) {                    \
    (void)env;                                                                                    \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    jv r = do_call(m, cls, &ap, NULL);                                                            \
    va_end(ap);                                                                                   \
    return r.FIELD;                                                                               \
  }                                                                                               \
  static RET CallStatic##NAME##MethodV(void *env, jobj *cls, jmethod *m, va_list ap) {            \
    (void)env;                                                                                    \
    return call_v(m, cls, ap).FIELD;                                                      \
  }                                                                                               \
  static RET CallStatic##NAME##MethodA(void *env, jobj *cls, jmethod *m, const jv *args) {        \
    (void)env;                                                                                    \
    return do_call(m, cls, NULL, args).FIELD;                                                     \
  }

CALL_FAMILY(Object, void *, l)
CALL_FAMILY(Boolean, int, i)
CALL_FAMILY(Byte, int, i)
CALL_FAMILY(Char, int, i)
CALL_FAMILY(Short, int, i)
CALL_FAMILY(Int, int, i)
CALL_FAMILY(Long, int64_t, j)
CALL_FAMILY(Float, float, f)
CALL_FAMILY(Double, double, d)

/* void needs its own copy (no value to return) */
#define CALL_VOID_FAMILY()                                                                        \
  static void CallVoidMethod(void *env, jobj *obj, jmethod *m, ...) {                             \
    (void)env;                                                                                    \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    do_call(m, obj, &ap, NULL);                                                                   \
    va_end(ap);                                                                                   \
  }                                                                                               \
  static void CallVoidMethodV(void *env, jobj *obj, jmethod *m, va_list ap) {                     \
    (void)env;                                                                                    \
    call_v(m, obj, ap);                                                                   \
  }                                                                                               \
  static void CallVoidMethodA(void *env, jobj *obj, jmethod *m, const jv *args) {                 \
    (void)env;                                                                                    \
    do_call(m, obj, NULL, args);                                                                  \
  }                                                                                               \
  static void CallNonvirtualVoidMethod(void *env, jobj *obj, jobj *cls, jmethod *m, ...) {        \
    (void)env; (void)cls;                                                                         \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    do_call(m, obj, &ap, NULL);                                                                   \
    va_end(ap);                                                                                   \
  }                                                                                               \
  static void CallNonvirtualVoidMethodV(void *env, jobj *obj, jobj *cls, jmethod *m,              \
                                        va_list ap) {                                             \
    (void)env; (void)cls;                                                                         \
    call_v(m, obj, ap);                                                                           \
  }                                                                                               \
  static void CallNonvirtualVoidMethodA(void *env, jobj *obj, jobj *cls, jmethod *m,              \
                                        const jv *args) {                                         \
    (void)env; (void)cls;                                                                         \
    do_call(m, obj, NULL, args);                                                                  \
  }                                                                                               \
  static void CallStaticVoidMethod(void *env, jobj *cls, jmethod *m, ...) {                       \
    (void)env;                                                                                    \
    va_list ap;                                                                                   \
    va_start(ap, m);                                                                              \
    do_call(m, cls, &ap, NULL);                                                                   \
    va_end(ap);                                                                                   \
  }                                                                                               \
  static void CallStaticVoidMethodV(void *env, jobj *cls, jmethod *m, va_list ap) {               \
    (void)env;                                                                                    \
    call_v(m, cls, ap);                                                                   \
  }                                                                                               \
  static void CallStaticVoidMethodA(void *env, jobj *cls, jmethod *m, const jv *args) {           \
    (void)env;                                                                                    \
    do_call(m, cls, NULL, args);                                                                  \
  }

CALL_VOID_FAMILY()

/* ------------------------------------------------------------------ */
/* the other JNIEnv functions that are implemented                       */
/* ------------------------------------------------------------------ */

static int jni_unimpl_count[JNI_NUM_FUNCS];

static intptr_t jni_unimpl(int idx) {
  if (jni_unimpl_count[idx]++ < 3)
    so_log("[jni] UNIMPLEMENTED JNIEnv function #%d %s", idx, jni_names[idx]);
  return 0;
}

static int GetVersion(void *env) { (void)env; return 0x00010006; }

static jobj *FindClass(void *env, const char *name) {
  (void)env;
  jobj *c = jni_find_class(name);
  so_log("[jni] FindClass %s", name);
  return c;
}

static jobj *GetObjectClass(void *env, jobj *o) {
  (void)env;
  if (is_obj(o) && o->kind == K_CLASS)
    return jni_find_class("java/lang/Class");
  if (is_obj(o) && o->cls)
    return o->cls;
  return jni_find_class("java/lang/Object");
}

static jobj *GetSuperclass(void *env, jobj *c) { (void)env; (void)c; return jni_find_class("java/lang/Object"); }
static int IsInstanceOf(void *env, jobj *o, jobj *c) { (void)env; (void)o; (void)c; return 1; }
static int IsSameObject(void *env, jobj *a, jobj *b) { (void)env; return a == b; }
static void *ret_self(void *env, void *o) { (void)env; return o; }
static void ret_void1(void *env, void *o) { (void)env; (void)o; }
static int ret_zero_env(void *env) { (void)env; return 0; }
static void *ret_null_env(void *env) { (void)env; return NULL; }
static int ensure_capacity(void *env, int n) { (void)env; (void)n; return 0; }
static int push_frame(void *env, int n) { (void)env; (void)n; return 0; }
static void *pop_frame(void *env, void *r) { (void)env; return r; }

static jmethod *GetMethodID(void *env, jobj *cls, const char *name, const char *sig) {
  (void)env;
  return get_method(cls, name, sig, 0);
}
static jmethod *GetStaticMethodID(void *env, jobj *cls, const char *name, const char *sig) {
  (void)env;
  return get_method(cls, name, sig, 1);
}

static jfield *get_field(jobj *cls, const char *name, const char *sig, int is_static) {
  jfield *f = calloc(1, sizeof(jfield));
  f->magic = JMAGIC;
  f->cls = cls;
  f->is_static = is_static;
  snprintf(f->name, sizeof(f->name), "%s", name);
  snprintf(f->sig, sizeof(f->sig), "%s", sig);
  so_log("[jni] %sFieldID %s.%s %s", is_static ? "static " : "", cname(cls), name, sig);
  return f;
}
static jfield *GetFieldID(void *env, jobj *cls, const char *name, const char *sig) {
  (void)env;
  return get_field(cls, name, sig, 0);
}
static jfield *GetStaticFieldID(void *env, jobj *cls, const char *name, const char *sig) {
  (void)env;
  return get_field(cls, name, sig, 1);
}
static void *GetObjectField(void *env, jobj *o, jfield *f) {
  (void)env; (void)o;
  if (f && f->sig[0] == 'L' && !strcmp(f->sig, "Ljava/lang/String;"))
    return jni_new_string("Vita");
  return NULL;
}
static int GetIntField(void *env, jobj *o, jfield *f) { (void)env; (void)o; (void)f; return 0; }
static int64_t GetLongField(void *env, jobj *o, jfield *f) { (void)env; (void)o; (void)f; return 0; }
static float GetFloatField(void *env, jobj *o, jfield *f) { (void)env; (void)o; (void)f; return 0.0f; }
static double GetDoubleField(void *env, jobj *o, jfield *f) { (void)env; (void)o; (void)f; return 0.0; }
static void SetField(void *env, jobj *o, jfield *f, ...) { (void)env; (void)o; (void)f; }

/* strings */
static jobj *NewStringUTF(void *env, const char *s) { (void)env; return jni_new_string(s); }
static int GetStringUTFLength(void *env, jobj *s) { (void)env; return is_obj(s) && s->name ? (int)strlen(s->name) : 0; }
static int GetStringLength(void *env, jobj *s) { return GetStringUTFLength(env, s); }
static const char *GetStringUTFChars(void *env, jobj *s, uint8_t *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return is_obj(s) && s->name ? s->name : "";
}
static void ReleaseStringUTFChars(void *env, jobj *s, const char *c) { (void)env; (void)s; (void)c; }

/* arrays */
static int GetArrayLength(void *env, jobj *a) { (void)env; return is_obj(a) ? a->len : 0; }

static jobj *new_array(int elem, int len) {
  jobj *a = obj_new(K_ARRAY, NULL, jni_find_class("[array"));
  a->elem = elem;
  a->len = len;
  a->data = calloc(len > 0 ? len : 1, elem);
  return a;
}
static jobj *NewBooleanArray(void *env, int n) { (void)env; return new_array(1, n); }
static jobj *NewByteArray(void *env, int n) { (void)env; return new_array(1, n); }
static jobj *NewCharArray(void *env, int n) { (void)env; return new_array(2, n); }
static jobj *NewShortArray(void *env, int n) { (void)env; return new_array(2, n); }
static jobj *NewIntArray(void *env, int n) { (void)env; return new_array(4, n); }
static jobj *NewLongArray(void *env, int n) { (void)env; return new_array(8, n); }
static jobj *NewFloatArray(void *env, int n) { (void)env; return new_array(4, n); }
static jobj *NewDoubleArray(void *env, int n) { (void)env; return new_array(8, n); }
static jobj *NewObjectArray(void *env, int n, jobj *cls, jobj *init) {
  (void)env; (void)cls;
  jobj *a = new_array(4, n);
  for (int i = 0; i < n; i++)
    ((jobj **)a->data)[i] = init;
  return a;
}
static jobj *GetObjectArrayElement(void *env, jobj *a, int i) {
  (void)env;
  return (is_obj(a) && i >= 0 && i < a->len) ? ((jobj **)a->data)[i] : NULL;
}
static void SetObjectArrayElement(void *env, jobj *a, int i, jobj *v) {
  (void)env;
  if (is_obj(a) && i >= 0 && i < a->len)
    ((jobj **)a->data)[i] = v;
}
static void *GetArrayElements(void *env, jobj *a, uint8_t *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return is_obj(a) ? a->data : NULL;
}
static void ReleaseArrayElements(void *env, jobj *a, void *p, int mode) { (void)env; (void)a; (void)p; (void)mode; }
static void GetArrayRegion(void *env, jobj *a, int start, int len, void *buf) {
  (void)env;
  if (is_obj(a) && start >= 0 && start + len <= a->len)
    memcpy(buf, (uint8_t *)a->data + (size_t)start * a->elem, (size_t)len * a->elem);
}
static void SetArrayRegion(void *env, jobj *a, int start, int len, const void *buf) {
  (void)env;
  if (is_obj(a) && start >= 0 && start + len <= a->len)
    memcpy((uint8_t *)a->data + (size_t)start * a->elem, buf, (size_t)len * a->elem);
}

/* misc */
typedef struct { const char *n, *s; void *f; } jnative;
static int RegisterNatives(void *env, jobj *cls, const jnative *m, int cnt) {
  (void)env;
  for (int i = 0; i < cnt; i++)
    so_log("[jni] RegisterNatives %s.%s%s -> %p", cname(cls), m[i].n, m[i].s, m[i].f);
  return 0;
}
static int GetJavaVM(void *env, void **vm) { (void)env; *vm = &vm_ptr; return 0; }
static jobj *NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
  (void)env;
  jobj *o = obj_new(K_OBJECT, NULL, jni_find_class("java/nio/ByteBuffer"));
  o->data = addr;
  o->len = (int)cap;
  return o;
}
static void *GetDirectBufferAddress(void *env, jobj *o) { (void)env; return is_obj(o) ? o->data : NULL; }
static int64_t GetDirectBufferCapacity(void *env, jobj *o) { (void)env; return is_obj(o) ? o->len : -1; }
static jobj *AllocObject(void *env, jobj *cls) { (void)env; return obj_new(K_OBJECT, NULL, cls); }
static jobj *NewObject(void *env, jobj *cls, jmethod *m, ...) {
  (void)env; (void)m;
  return obj_new(K_OBJECT, NULL, cls);
}
static jobj *NewObjectV(void *env, jobj *cls, jmethod *m, va_list ap) { (void)ap; return NewObject(env, cls, m); }
static jobj *NewObjectA(void *env, jobj *cls, jmethod *m, const jv *a) { (void)a; return NewObject(env, cls, m); }
static int ThrowNew(void *env, jobj *cls, const char *msg) {
  (void)env;
  so_log("[jni] ThrowNew %s: %s", cname(cls), msg ? msg : "");
  return 0;
}
static void FatalError(void *env, const char *msg) { (void)env; so_fatal("JNI FatalError: %s", msg); }
static int GetObjectRefType(void *env, void *o) { (void)env; (void)o; return 1; }

/* ------------------------------------------------------------------ */
/* JavaVM                                                               */
/* ------------------------------------------------------------------ */

static int vm_get_env(void *vm, void **penv, int version) {
  (void)vm; (void)version;
  *penv = &jni_env_ptr;
  return 0;
}
static int vm_attach(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  *penv = &jni_env_ptr;
  return 0;
}
static int vm_detach(void *vm) { (void)vm; return 0; }
static int vm_destroy(void *vm) { (void)vm; return 0; }

void *jni_vm(void) { return &vm_ptr; }
void *jni_env(void) { return &jni_env_ptr; }

void jni_init(void) {
  memcpy(jni_table, jni_stub_table, sizeof(jni_table));
#define SET(NAME, FN) jni_table[JI_##NAME] = (void *)(FN)
  SET(GetVersion, GetVersion);
  SET(FindClass, FindClass);
  SET(GetSuperclass, GetSuperclass);
  SET(IsInstanceOf, IsInstanceOf);
  SET(IsSameObject, IsSameObject);
  SET(ExceptionOccurred, ret_null_env);
  SET(ExceptionDescribe, ret_zero_env);
  SET(ExceptionClear, ret_zero_env);
  SET(ExceptionCheck, ret_zero_env);
  SET(FatalError, FatalError);
  SET(ThrowNew, ThrowNew);
  SET(PushLocalFrame, push_frame);
  SET(PopLocalFrame, pop_frame);
  SET(NewGlobalRef, ret_self);
  SET(NewLocalRef, ret_self);
  SET(NewWeakGlobalRef, ret_self);
  SET(DeleteGlobalRef, ret_void1);
  SET(DeleteLocalRef, ret_void1);
  SET(DeleteWeakGlobalRef, ret_void1);
  SET(EnsureLocalCapacity, ensure_capacity);
  SET(AllocObject, AllocObject);
  SET(NewObject, NewObject);
  SET(NewObjectV, NewObjectV);
  SET(NewObjectA, NewObjectA);
  SET(GetObjectClass, GetObjectClass);
  SET(GetMethodID, GetMethodID);
  SET(GetStaticMethodID, GetStaticMethodID);
  SET(GetFieldID, GetFieldID);
  SET(GetStaticFieldID, GetStaticFieldID);

#define SET_CALLS(T)                                                         \
  SET(Call##T##Method, Call##T##Method);                                     \
  SET(Call##T##MethodV, Call##T##MethodV);                                   \
  SET(Call##T##MethodA, Call##T##MethodA);                                   \
  SET(CallNonvirtual##T##Method, CallNonvirtual##T##Method);                 \
  SET(CallNonvirtual##T##MethodV, CallNonvirtual##T##MethodV);               \
  SET(CallNonvirtual##T##MethodA, CallNonvirtual##T##MethodA);               \
  SET(CallStatic##T##Method, CallStatic##T##Method);                         \
  SET(CallStatic##T##MethodV, CallStatic##T##MethodV);                       \
  SET(CallStatic##T##MethodA, CallStatic##T##MethodA)
  SET_CALLS(Object); SET_CALLS(Boolean); SET_CALLS(Byte); SET_CALLS(Char);
  SET_CALLS(Short); SET_CALLS(Int); SET_CALLS(Long); SET_CALLS(Float);
  SET_CALLS(Double); SET_CALLS(Void);

  SET(GetObjectField, GetObjectField);
  SET(GetBooleanField, GetIntField);
  SET(GetByteField, GetIntField);
  SET(GetCharField, GetIntField);
  SET(GetShortField, GetIntField);
  SET(GetIntField, GetIntField);
  SET(GetLongField, GetLongField);
  SET(GetFloatField, GetFloatField);
  SET(GetDoubleField, GetDoubleField);
  SET(GetStaticObjectField, GetObjectField);
  SET(GetStaticBooleanField, GetIntField);
  SET(GetStaticByteField, GetIntField);
  SET(GetStaticCharField, GetIntField);
  SET(GetStaticShortField, GetIntField);
  SET(GetStaticIntField, GetIntField);
  SET(GetStaticLongField, GetLongField);
  SET(GetStaticFloatField, GetFloatField);
  SET(GetStaticDoubleField, GetDoubleField);
  SET(SetObjectField, SetField);
  SET(SetBooleanField, SetField);
  SET(SetByteField, SetField);
  SET(SetCharField, SetField);
  SET(SetShortField, SetField);
  SET(SetIntField, SetField);
  SET(SetLongField, SetField);
  SET(SetFloatField, SetField);
  SET(SetDoubleField, SetField);
  SET(SetStaticObjectField, SetField);
  SET(SetStaticBooleanField, SetField);
  SET(SetStaticByteField, SetField);
  SET(SetStaticCharField, SetField);
  SET(SetStaticShortField, SetField);
  SET(SetStaticIntField, SetField);
  SET(SetStaticLongField, SetField);
  SET(SetStaticFloatField, SetField);
  SET(SetStaticDoubleField, SetField);

  SET(NewStringUTF, NewStringUTF);
  SET(GetStringUTFLength, GetStringUTFLength);
  SET(GetStringLength, GetStringLength);
  SET(GetStringUTFChars, GetStringUTFChars);
  SET(ReleaseStringUTFChars, ReleaseStringUTFChars);
  SET(GetArrayLength, GetArrayLength);
  SET(NewObjectArray, NewObjectArray);
  SET(GetObjectArrayElement, GetObjectArrayElement);
  SET(SetObjectArrayElement, SetObjectArrayElement);
  SET(NewBooleanArray, NewBooleanArray);
  SET(NewByteArray, NewByteArray);
  SET(NewCharArray, NewCharArray);
  SET(NewShortArray, NewShortArray);
  SET(NewIntArray, NewIntArray);
  SET(NewLongArray, NewLongArray);
  SET(NewFloatArray, NewFloatArray);
  SET(NewDoubleArray, NewDoubleArray);
#define SET_ARRAY_OPS(T)                                  \
  SET(Get##T##ArrayElements, GetArrayElements);           \
  SET(Release##T##ArrayElements, ReleaseArrayElements);   \
  SET(Get##T##ArrayRegion, GetArrayRegion);               \
  SET(Set##T##ArrayRegion, SetArrayRegion)
  SET_ARRAY_OPS(Boolean); SET_ARRAY_OPS(Byte); SET_ARRAY_OPS(Char); SET_ARRAY_OPS(Short);
  SET_ARRAY_OPS(Int); SET_ARRAY_OPS(Long); SET_ARRAY_OPS(Float); SET_ARRAY_OPS(Double);
  SET(GetPrimitiveArrayCritical, GetArrayElements);
  SET(ReleasePrimitiveArrayCritical, ReleaseArrayElements);

  SET(RegisterNatives, RegisterNatives);
  SET(GetJavaVM, GetJavaVM);
  SET(NewDirectByteBuffer, NewDirectByteBuffer);
  SET(GetDirectBufferAddress, GetDirectBufferAddress);
  SET(GetDirectBufferCapacity, GetDirectBufferCapacity);
  SET(GetObjectRefType, GetObjectRefType);
  SET(MonitorEnter, ret_zero_env);
  SET(MonitorExit, ret_zero_env);
#undef SET

  vm_table[3] = (void *)vm_destroy;
  vm_table[4] = (void *)vm_attach;
  vm_table[5] = (void *)vm_detach;
  vm_table[6] = (void *)vm_get_env;
  vm_table[7] = (void *)vm_attach;
}
