/* opensl_vita.c -- a tiny fake Android OpenSL ES that plays through sceAudioOut.
 *
 * FMOD (inside the game) loads "libOpenSLES.so" with dlopen and talks to it
 * through function-pointer tables.  We hand it our own tables instead.  Only
 * what FMOD needs is real: engine, output mix, one PCM buffer-queue player.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <psp2/audioout.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "so_platform.h"

#define SL_OK 0
#define SL_UNSUPPORTED 12
#define SL_PARAM_INVALID 2

#define MAX_QUEUE 32
#define GRAIN 1024

typedef struct sl_obj sl_obj;
typedef struct {
  const void *const *vt;
  sl_obj *obj;
} itf_t;

typedef struct {
  const uint8_t *data; /* FMOD's buffer: it must not be touched until we call back */
  uint32_t size;
} qent_t;

enum { K_ENGINE = 1, K_MIX, K_PLAYER };
enum { ID_ENGINE = 1, ID_PLAY, ID_BQ, ID_CONFIG, ID_RECORD, ID_VOLUME, ID_ANDROIDEFFECT, ID_OUTPUTMIX };

struct sl_obj {
  itf_t self, engine, play, bq, config, generic;
  int kind;
  /* player */
  int channels, bits, is_float, rate;
  volatile int state; /* 1 stopped, 2 paused, 3 playing */
  void (*cb)(void *, void *);
  void *cb_ctx;
  qent_t q[MAX_QUEUE];
  int q_head, q_count;
  uint32_t cur_off; /* byte offset into q[q_head] */
  uint32_t played_bufs;
  int enq_logged;
  uint32_t underruns, grains, changed, checked_bufs;
  volatile uint32_t cb_max_us, gap_max_us;
  uint32_t logged_grains;
  SceUID mutex, data_sema, space_sema;
  SceUID thread, feeder;
  volatile int quit;
};

/* ---- interface ids: dlsym returns the address of a variable that holds a pointer */
typedef struct {
  uint32_t id;
  uint32_t pad[3];
} iid_t;
static iid_t iids[] = {{0}, {ID_ENGINE}, {ID_PLAY}, {ID_BQ}, {ID_CONFIG}, {ID_RECORD}, {ID_VOLUME}, {ID_ANDROIDEFFECT}, {ID_OUTPUTMIX}};
static const void *iid_vars[9];

static int iid_of(const void *p) {
  if (!p)
    return 0;
  for (int i = 1; i < 9; i++)
    if (p == &iids[i] || p == &iid_vars[i])
      return i;
  /* maybe a pointer to the variable's contents */
  for (int i = 1; i < 9; i++)
    if (*(const void *const *)p == (const void *)&iids[i])
      return i;
  return 0;
}

static void lock(sl_obj *o) { sceKernelLockMutex(o->mutex, 1, NULL); }
static void unlock(sl_obj *o) { sceKernelUnlockMutex(o->mutex, 1); }

/* ---- playback ------------------------------------------------------------
 *
 * Two threads, so the game's sound engine never makes the speaker wait:
 *  - the feeder takes each buffer FMOD hands us, converts it into a ring of
 *    ready-to-play samples and gives the buffer straight back (FMOD then mixes
 *    the next one, possibly right there inside our callback);
 *  - the output thread only copies ready samples from the ring to the Vita's
 *    audio hardware.  It never runs FMOD code, so it is never late.
 * The ring holds ~0.1 s, which absorbs slow mixing moments.
 */
#define RING 6144            /* frames (stereo) */
#define PREFILL (GRAIN * 2)  /* frames buffered before the sound starts */
#define BASS_CUT_DB 0.0f     /* how much quieter the deep bass gets (0 = original sound) */
#define BASS_FREQ 150.0f     /* below this the bass is turned down */

static int16_t ring[RING * 2];
static volatile uint32_t ring_w, ring_r; /* frame counters, only ever increase */

static int feeder_thread(SceSize args, void *argp) {
  (void)args;
  sl_obj *o = *(sl_obj **)argp;
  const int bps = (o->bits / 8) * o->channels; /* bytes per source frame */
  const double step = (double)o->rate / 48000.0;
  double pos = 0.0;          /* source position, relative to the current buffer */
  float last_l = 0, last_r = 0; /* last frame of the previous buffer (for resampling) */
  while (!o->quit) {
    if (o->state != 3) {
      sceKernelDelayThread(2000);
      continue;
    }
    lock(o);
    if (o->q_count == 0) {
      unlock(o);
      SceUInt to = 5000;
      sceKernelWaitSema(o->data_sema, 1, &to);
      continue;
    }
    qent_t e = o->q[o->q_head];
    unlock(o);
    uint32_t nfr = e.size / bps;
    uint32_t need = (uint32_t)((double)nfr / step) + 2;
    /* wait for room in the ring */
    while (!o->quit && RING - (ring_w - ring_r) < need) {
      SceUInt to = 5000;
      sceKernelWaitSema(o->space_sema, 1, &to);
    }
    /* convert: output frame k sits at source position pos (0 = first frame of this buffer,
     * -1 = last frame of the previous one) */
    uint32_t w = ring_w;
    while (pos < (double)nfr - 1.0 + 1e-9 || (step == 1.0 && pos < (double)nfr)) {
      int i1 = (int)(pos + 1.0) - 1; /* floor(pos) for pos >= -1 */
      float fr = (float)(pos - (double)i1);
      float l0, r0, l1, r1;
      if (i1 < 0) { l0 = last_l; r0 = last_r; }
      else if (o->is_float) {
        const float *f = (const float *)(e.data + i1 * bps);
        l0 = f[0] * 32767.f; r0 = (o->channels > 1 ? f[1] : f[0]) * 32767.f;
      } else {
        const int16_t *f = (const int16_t *)(e.data + i1 * bps);
        l0 = f[0]; r0 = o->channels > 1 ? f[1] : f[0];
      }
      if (fr > 0.0f && i1 + 1 < (int)nfr) {
        if (o->is_float) {
          const float *f = (const float *)(e.data + (i1 + 1) * bps);
          l1 = f[0] * 32767.f; r1 = (o->channels > 1 ? f[1] : f[0]) * 32767.f;
        } else {
          const int16_t *f = (const int16_t *)(e.data + (i1 + 1) * bps);
          l1 = f[0]; r1 = o->channels > 1 ? f[1] : f[0];
        }
      } else {
        l1 = l0; r1 = r0;
      }
      float fl = l0 + (l1 - l0) * fr, frr = r0 + (r1 - r0) * fr;
      if (fl > 32767.f) fl = 32767.f; else if (fl < -32768.f) fl = -32768.f;
      if (frr > 32767.f) frr = 32767.f; else if (frr < -32768.f) frr = -32768.f;
      uint32_t k = w % RING;
      ring[k * 2] = (int16_t)fl;
      ring[k * 2 + 1] = (int16_t)frr;
      w++;
      pos += step;
    }
    pos -= (double)nfr;
    if (nfr > 0) {
      if (o->is_float) {
        const float *f = (const float *)(e.data + (nfr - 1) * bps);
        last_l = f[0] * 32767.f; last_r = (o->channels > 1 ? f[1] : f[0]) * 32767.f;
      } else {
        const int16_t *f = (const int16_t *)(e.data + (nfr - 1) * bps);
        last_l = f[0]; last_r = o->channels > 1 ? f[1] : f[0];
      }
    }
    __sync_synchronize();
    ring_w = w;
    /* hand the buffer back to FMOD */
    lock(o);
    if (o->q_count > 0 && o->q[o->q_head].data == e.data) {
      o->q[o->q_head].data = NULL;
      o->q_head = (o->q_head + 1) % MAX_QUEUE;
      o->q_count--;
      o->played_bufs++;
    }
    void (*cb)(void *, void *) = o->cb;
    void *ctx = o->cb_ctx;
    unlock(o);
    if (cb) {
      uint64_t t0 = sceKernelGetProcessTimeWide();
      cb(&o->bq, ctx);
      uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
      if (dt > o->cb_max_us)
        o->cb_max_us = dt;
    }
    if (o->grains - o->logged_grains >= 235) {
      o->logged_grains = o->grains;
      so_log("[fps] audio: %u grains, %u underruns, ring %u frames, slowest mix %u us, slowest output gap %u us",
             (unsigned)o->grains, (unsigned)o->underruns, (unsigned)(ring_w - ring_r), (unsigned)o->cb_max_us,
             (unsigned)o->gap_max_us);
      o->cb_max_us = 0;
      o->gap_max_us = 0;
    }
  }
  return 0;
}

static int audio_thread(SceSize args, void *argp) {
  (void)args;
  sl_obj *o = *(sl_obj **)argp;
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  so_log("[audio] port %08x opened, source %d Hz -> 48000 Hz, %d ch, %d bit%s", (unsigned)port, o->rate,
         o->channels, o->bits, o->is_float ? " float" : "");
  if (port < 0)
    return 0;
  /* The hardware keeps reading a buffer after sceAudioOutOutput returns, so we
   * must never overwrite the one it is still playing: rotate between three. */
  static int16_t outbufs[3][GRAIN * 2] __attribute__((aligned(64)));
  int cur = 0;

  /* bass turned down with a low-shelf filter; everything above BASS_FREQ is untouched */
  const float PI_F = 3.14159265f;
  float A = powf(10.0f, BASS_CUT_DB / 40.0f);
  float w0 = 2.0f * PI_F * BASS_FREQ / 48000.0f, cw = cosf(w0), sw = sinf(w0);
  float alpha = sw / 2.0f * sqrtf(2.0f), sa = 2.0f * sqrtf(A) * alpha;
  float a0 = (A + 1) + (A - 1) * cw + sa;
  const float b0 = A * ((A + 1) - (A - 1) * cw + sa) / a0;
  const float b1 = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
  const float b2 = A * ((A + 1) - (A - 1) * cw - sa) / a0;
  const float a1 = -2 * ((A - 1) + (A + 1) * cw) / a0;
  const float a2 = ((A + 1) + (A - 1) * cw - sa) / a0;
  float zl1 = 0, zl2 = 0, zr1 = 0, zr2 = 0;
  const int use_filter = BASS_CUT_DB != 0.0f;

  int started = 0;
  uint32_t late_max = 0;
  uint64_t t_prev = 0;
  while (!o->quit) {
    if (o->state != 3) {
      started = 0;
      sceKernelDelayThread(2000);
      continue;
    }
    uint32_t avail = ring_w - ring_r;
    if (!started) {
      if (avail < PREFILL) {
        sceKernelDelayThread(1000);
        continue;
      }
      started = 1;
    }
    __sync_synchronize();
    int16_t *out = outbufs[cur];
    cur = (cur + 1) % 3;
    uint32_t take = avail < GRAIN ? avail : GRAIN;
    uint32_t r = ring_r;
    for (uint32_t n = 0; n < take; n++) {
      uint32_t k = (r + n) % RING;
      out[n * 2] = ring[k * 2];
      out[n * 2 + 1] = ring[k * 2 + 1];
    }
    if (take < GRAIN) {
      memset(&out[take * 2], 0, (GRAIN - take) * 4);
      o->underruns++;
    }
    __sync_synchronize();
    ring_r = r + take;
    sceKernelSignalSema(o->space_sema, 1);

    if (use_filter) {
      for (int n = 0; n < GRAIN; n++) {
        float x = out[n * 2], y = b0 * x + zl1;
        zl1 = b1 * x - a1 * y + zl2;
        zl2 = b2 * x - a2 * y;
        if (y > 32767.f) y = 32767.f; else if (y < -32768.f) y = -32768.f;
        out[n * 2] = (int16_t)y;
        x = out[n * 2 + 1];
        y = b0 * x + zr1;
        zr1 = b1 * x - a1 * y + zr2;
        zr2 = b2 * x - a2 * y;
        if (y > 32767.f) y = 32767.f; else if (y < -32768.f) y = -32768.f;
        out[n * 2 + 1] = (int16_t)y;
      }
    }

    o->grains++;
    if (late_max > o->gap_max_us)
      o->gap_max_us = late_max;
    late_max = 0;
    uint64_t t = sceKernelGetProcessTimeWide();
    if (t_prev && (uint32_t)(t - t_prev) > late_max)
      late_max = (uint32_t)(t - t_prev);
    sceAudioOutOutput(port, out);
    t_prev = sceKernelGetProcessTimeWide();
  }
  sceAudioOutReleasePort(port);
  return 0;
}

/* ---- SLBufferQueueItf ------------------------------------------------------ */
static uint32_t bq_enqueue(itf_t *s, const void *buf, uint32_t size) {
  sl_obj *o = s->obj;
  lock(o);
  if (o->q_count >= MAX_QUEUE) {
    unlock(o);
    return 5; /* SL_RESULT_BUFFER_INSUFFICIENT */
  }
  int idx = (o->q_head + o->q_count) % MAX_QUEUE;
  o->q[idx].data = buf;
  o->q[idx].size = size;
  if (o->enq_logged < 12) {
    o->enq_logged++;
    so_log("[audio] Enqueue %p %u bytes (%u frames), queued %d", buf, (unsigned)size,
           (unsigned)(size / ((o->bits / 8) * o->channels)), o->q_count);
  }
  o->q_count++;
  unlock(o);
  sceKernelSignalSema(o->data_sema, 1);
  return SL_OK;
}
static uint32_t bq_clear(itf_t *s) {
  sl_obj *o = s->obj;
  lock(o);
  while (o->q_count > 0) {
    o->q[o->q_head].data = NULL;
    o->q_head = (o->q_head + 1) % MAX_QUEUE;
    o->q_count--;
  }
  o->cur_off = 0;
  unlock(o);
  return SL_OK;
}
static uint32_t bq_getstate(itf_t *s, uint32_t *st) {
  sl_obj *o = s->obj;
  lock(o);
  st[0] = o->q_count;
  st[1] = o->played_bufs;
  unlock(o);
  return SL_OK;
}
static uint32_t bq_register(itf_t *s, void (*cb)(void *, void *), void *ctx) {
  sl_obj *o = s->obj;
  lock(o);
  o->cb = cb;
  o->cb_ctx = ctx;
  unlock(o);
  return SL_OK;
}
static const void *const bq_vt[] = {bq_enqueue, bq_clear, bq_getstate, bq_register};

/* ---- SLPlayItf ------------------------------------------------------------- */
static uint32_t play_set(itf_t *s, uint32_t st) {
  s->obj->state = (int)st;
  return SL_OK;
}
static uint32_t play_get(itf_t *s, uint32_t *st) {
  *st = (uint32_t)s->obj->state;
  return SL_OK;
}
static uint32_t play_pos(itf_t *s, uint32_t *ms) {
  (void)s;
  *ms = 0;
  return SL_OK;
}
static uint32_t ok_stub(void) { return SL_OK; }
static const void *const play_vt[] = {play_set, play_get, ok_stub, play_pos, ok_stub, ok_stub,
                                      ok_stub, ok_stub, ok_stub, ok_stub, ok_stub, ok_stub};

/* generic: android config, volume, ... all accept and ignore */
static const void *const generic_vt[] = {ok_stub, ok_stub, ok_stub, ok_stub, ok_stub, ok_stub,
                                         ok_stub, ok_stub, ok_stub, ok_stub, ok_stub, ok_stub,
                                         ok_stub, ok_stub, ok_stub, ok_stub};

/* ---- SLObjectItf ----------------------------------------------------------- */
static uint32_t obj_realize(itf_t *s, uint32_t async) {
  (void)async;
  sl_obj *o = s->obj;
  if (o->kind == K_PLAYER && !o->thread) {
    sl_obj *p = o;
    /* output: highest priority, tiny job.  feeder: runs FMOD's mixing, needs a big stack */
    o->thread = sceKernelCreateThread("slaudio", audio_thread, 64 /* highest user priority */, 0x10000, 0, 0, NULL);
    o->feeder = sceKernelCreateThread("slfeed", feeder_thread, 0x10000100 - 10, 0x100000, 0, 0, NULL);
    if (o->thread >= 0 && o->feeder >= 0) {
      sceKernelStartThread(o->feeder, sizeof(p), &p);
      sceKernelStartThread(o->thread, sizeof(p), &p);
    } else {
      so_log("[audio] thread create failed %08x %08x", (unsigned)o->thread, (unsigned)o->feeder);
    }
  }
  return SL_OK;
}
static uint32_t obj_getstate(itf_t *s, uint32_t *st) {
  (void)s;
  *st = 2; /* realized */
  return SL_OK;
}
static uint32_t obj_getif(itf_t *s, const void *iid, void **out) {
  sl_obj *o = s->obj;
  int id = iid_of(iid);
  *out = NULL;
  switch (id) {
    case ID_ENGINE:
      if (o->kind == K_ENGINE) { *out = &o->engine; return SL_OK; }
      break;
    case ID_PLAY:
      if (o->kind == K_PLAYER) { *out = &o->play; return SL_OK; }
      break;
    case ID_BQ:
      if (o->kind == K_PLAYER) { *out = &o->bq; return SL_OK; }
      break;
    case ID_CONFIG:
    case ID_VOLUME:
    case ID_ANDROIDEFFECT:
    case ID_OUTPUTMIX:
      *out = &o->generic;
      return SL_OK;
    default:
      break;
  }
  so_log("[audio] GetInterface: unsupported id %d", id);
  return SL_UNSUPPORTED;
}
static uint32_t obj_destroy(itf_t *s) {
  sl_obj *o = s->obj;
  if (o->kind == K_PLAYER && o->thread > 0) {
    o->quit = 1;
    sceKernelWaitThreadEnd(o->thread, NULL, NULL);
    sceKernelDeleteThread(o->thread);
    o->thread = 0;
    if (o->feeder > 0) {
      sceKernelWaitThreadEnd(o->feeder, NULL, NULL);
      sceKernelDeleteThread(o->feeder);
      o->feeder = 0;
    }
  }
  /* the object memory is leaked on purpose: FMOD might still hold pointers */
  return SL_OK;
}
static const void *const obj_vt[] = {obj_realize, ok_stub, obj_getstate, obj_getif, ok_stub,
                                     ok_stub, obj_destroy, ok_stub, ok_stub, ok_stub};

/* ---- SLEngineItf ------------------------------------------------------------ */
static sl_obj *new_obj(int kind) {
  sl_obj *o = calloc(1, sizeof(*o));
  o->kind = kind;
  o->self = (itf_t){obj_vt, o};
  o->generic = (itf_t){generic_vt, o};
  o->play = (itf_t){play_vt, o};
  o->bq = (itf_t){bq_vt, o};
  o->mutex = sceKernelCreateMutex("slmutex", 0x2 /* recursive */, 0, NULL);
  o->data_sema = sceKernelCreateSema("sldata", 0, 0, 1, NULL);
  o->space_sema = sceKernelCreateSema("slspace", 0, 0, 1, NULL);
  o->state = 1;
  return o;
}

static uint32_t eng_outputmix(itf_t *s, void **pobj, uint32_t n, const void *ids, const void *req) {
  (void)s; (void)n; (void)ids; (void)req;
  sl_obj *o = new_obj(K_MIX);
  *pobj = &o->self;
  return SL_OK;
}

static uint32_t eng_player(itf_t *s, void **pobj, const uint32_t *src, const void *snk, uint32_t n,
                           const void *ids, const void *req) {
  (void)s; (void)snk; (void)n; (void)ids; (void)req;
  /* SLDataSource { void *pLocator; void *pFormat; } */
  const uint32_t *fmt = (const uint32_t *)(uintptr_t)src[1];
  sl_obj *o = new_obj(K_PLAYER);
  o->channels = 2;
  o->bits = 16;
  o->rate = 48000;
  if (fmt) {
    uint32_t type = fmt[0];
    o->channels = (int)fmt[1];
    o->rate = (int)(fmt[2] / 1000);
    o->bits = (int)fmt[3];
    if (type == 4 /* PCM_EX */ && fmt[7] == 3 /* float */)
      o->is_float = 1;
    if (o->channels < 1 || o->channels > 2) {
      so_log("[audio] player with %d channels not supported", o->channels);
      o->channels = 2;
    }
    if (o->bits != 16 && !(o->is_float && o->bits == 32)) {
      so_log("[audio] unsupported sample format: type %u bits %d", (unsigned)type, o->bits);
      o->bits = 16;
    }
  }
  so_log("[audio] CreateAudioPlayer: %d ch %d Hz %d bit%s", o->channels, o->rate, o->bits,
         o->is_float ? " float" : "");
  *pobj = &o->self;
  return SL_OK;
}

static uint32_t eng_recorder(void) { return SL_UNSUPPORTED; }

static const void *const eng_vt[] = {ok_stub, ok_stub, eng_player, eng_recorder, ok_stub, ok_stub,
                                     ok_stub, eng_outputmix, ok_stub, ok_stub, ok_stub, ok_stub,
                                     ok_stub, ok_stub, ok_stub};

/* ---- slCreateEngine ---------------------------------------------------------- */
static uint32_t sl_create_engine(void **pengine, uint32_t n, const void *opt, uint32_t nif,
                                 const void *ids, const void *req) {
  (void)n; (void)opt; (void)nif; (void)ids; (void)req;
  sl_obj *o = new_obj(K_ENGINE);
  o->engine = (itf_t){eng_vt, o};
  *pengine = &o->self;
  so_log("[audio] slCreateEngine ok");
  return SL_OK;
}

/* ---- what dlopen / dlsym hand out ---------------------------------------------- */
static int fake_handle;

void *opensl_dlopen(const char *name) {
  if (name && strstr(name, "OpenSLES")) {
    so_log("dlopen(\"%s\") -> fake OpenSL ES", name);
    return &fake_handle;
  }
  return NULL;
}

void *opensl_dlsym(void *handle, const char *name) {
  if (handle != &fake_handle || !name)
    return NULL;
  static const char *const names[] = {"", "SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
                                      "SL_IID_ANDROIDCONFIGURATION", "SL_IID_RECORD", "SL_IID_VOLUME",
                                      "SL_IID_ANDROIDEFFECT", "SL_IID_OUTPUTMIX"};
  if (!strcmp(name, "slCreateEngine"))
    return (void *)sl_create_engine;
  for (int i = 1; i < 9; i++)
    if (!strcmp(name, names[i])) {
      iid_vars[i] = &iids[i];
      return &iid_vars[i];
    }
  /* any other SL_IID_* : give it a unique dummy id so GetInterface says "unsupported" */
  if (!strncmp(name, "SL_IID_", 7)) {
    static iid_t dummy[16];
    static const void *dummy_var[16];
    static int nd;
    if (nd < 16) {
      dummy[nd].id = 100 + nd;
      dummy_var[nd] = &dummy[nd];
      return &dummy_var[nd++];
    }
  }
  so_log("dlsym(OpenSL, \"%s\") -> NULL", name);
  return NULL;
}
