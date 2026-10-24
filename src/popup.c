/* popup.c -- the "report bugs" pop-up opened by the main menu's top-right button.
 *
 * The game asks Android to open a web page (MobiActivity.openUrl) when that
 * button is tapped; jni.c turns that into popup_request().  The pop-up is drawn
 * with vitaGL's simple (fixed-function) drawing after the game has drawn its
 * frame.  Every bit of GL state that is touched is put back afterwards, because
 * the game remembers what it last set and does not set it again.
 *
 * It is laid out exactly like the game's own pop-ups (numbers from libcgame):
 *   - the game's white pop-up panel (menu "new_popup_bg", drawn at 1.67), centred
 *     where the game puts its pop-ups: (226, 155) points = (452, 263.5) pixels;
 *     the game's layout is 480x320 points stretched to the screen, 1.7 px a point,
 *     and its sheets have 2 pixels a point (0.85 px a sheet pixel here)
 *   - its X button (menu "new_popup_button_close") with its top-left corner at
 *     (162, -148) points from the centre
 *   - the badge corner of the event pop-ups, centred at (-180, -110) points: the
 *     game's warning sign, as big as the game's event badges, with a black
 *     outline (no shadow) like those badges: theirs are 6-11 sheet pixels, ~6 px here
 *   - the blue banner of the mission pop-ups (events "pop_up_info_bis", tinted
 *     blue, at 0.95), with its shine sweeping across; the game has it at (0, 60)
 *     points, here at (0, 45) so the contact list fits under it
 * The panel, X, warning sign, banner and shine are cut out of the game's own
 * sheets on the memory card (game_art.c) in the background while the game
 * starts; only our text (app0:popup_text.png, tools/make_popup.py) is shipped.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include "art_patch.h"
#include "game_art.h"
#include "gfx_vita.h"
#include "popup.h"
#include "so_platform.h"

#define TEXT_FILE "app0:popup_text.png"
#define SCREEN_W 960.0f
#define SCREEN_H 544.0f
#define HIGHLIGHT_FRAMES 5 /* after letting go: the lit-up X stays this long ... */
#define FADE_FRAMES 6      /* ... then the window fades away over this many frames */

/* the game's pop-up layout on the 960x544 screen */
#define CX 452.0f  /* pop-up centre */
#define CY 263.5f
#define PT 1.7f    /* screen pixels per layout point */
#define PX 0.85f   /* screen pixels per sheet pixel */
#define BX0 80     /* the panel picture (and popup_text.png) covers x 80..824, y 0..530 */
#define BY0 0
#define BW 744
#define BH 530
#define BANNER_Y (CY + 45 * PT)   /* banner centre (make_popup.py BANNER_PT) */
#define BANNER_S (0.95f * PX)     /* banner scale (sheet pixels -> screen) */
/* the X button's touch area: the game's close button reacts in (-10,-10) 53x55
 * points from its top-left corner (libcgame 0x19f01c) */
#define X_HIT_X0 (CX + (162 - 10) * PT)
#define X_HIT_Y0 (CY + (-148 - 10) * PT)
#define X_HIT_X1 (X_HIT_X0 + 53 * PT)
#define X_HIT_Y1 (X_HIT_Y0 + 55 * PT)

static volatile int requested;
static int showing, frames;
static int wait_release; /* the finger that opened it is still down: ignore it until lifted */
static int touch_held;   /* a finger is on the X button (it is shown pressed) */
static int touch_armed;  /* the finger now down first touched the X */
static int touch_prev;   /* a finger was down last frame */
static int closing;      /* frames since the press was let go, 0 = not closing */

void popup_request(void) { requested = 1; }

#ifdef REAL_GL
#include <vitaGL.h>

static GLuint tex_dim, tex_base, tex_text, tex_banner, tex_shine, tex_x, tex_x_lit;
static float pop_scale = 1.0f; /* the opening "pop": everything is drawn scaled round the pop-up's centre */
static SceUInt64 opened_at;

/* The pictures are put together in the background while the game starts
 * (art_thread) and turned into textures on the main thread (upload_art). */
static volatile int art_state; /* 0 being made, 1 made, 2 textures ready, -1 failed */
static ga_canvas c_base, c_banner, c_x, c_x_lit;
static int base_ok, banner_ok, x_ok;
static int banner_x0, banner_y0, x_x0, x_y0; /* where the banner / X pictures sit on the screen */
static ga_img shine;                          /* events module 190, the banner's shine */
static unsigned char *text_px;
static int text_w, text_h;

static GLuint make_texture(const void *rgba, int w, int h) {
  GLint prev_active = GL_TEXTURE0, prev_tex = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glActiveTexture((GLenum)prev_active);
  return t;
}

static void straight_alpha(uint8_t *p, int n) {
  for (int i = 0; i < n; i++, p += 4)
    if (p[3] && p[3] < 255)
      for (int k = 0; k < 3; k++) {
        int v = p[k] * 255 / p[3];
        p[k] = (uint8_t)(v > 255 ? 255 : v);
      }
}

static int alloc_canvas(ga_canvas *c, int w, int h) {
  c->w = w;
  c->h = h;
  c->px = calloc((size_t)w * h, 4);
  return c->px != NULL;
}

/* the game's pop-up panel: menu frame 146 ("new_popup_bg" draws it at 1.67):
 * module 342 (white) x15 round its centre, then the four edges */
static void draw_panel(const ga_img *m) {
  static const struct { int i; float x, y, sx, sy; } piece[5] = {
    { 0, -10, 0, 15, 15 }, { 1, -206, 133, 1, 1 }, { 2, -240, -176, 1, 1 }, { 3, -245, -133, 1, 1 }, { 4, 210, -133, 1, 1 },
  };
  const float k = 1.67f * PX;
  for (int p = 0; p < 5; p++) {
    const ga_img *im = &m[piece[p].i];
    float cx = piece[p].x + im->w / 2.0f, cy = piece[p].y + im->h / 2.0f;
    float hw = im->w * piece[p].sx / 2, hh = im->h * piece[p].sy / 2;
    ga_blit(&c_base, im, CX - BX0 + (cx - hw) * k, CY - BY0 + (cy - hh) * k, CX - BX0 + (cx + hw) * k,
            CY - BY0 + (cy + hh) * k, 0, 0, 0);
  }
}

static int art_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  if (!alloc_canvas(&c_base, BW, BH)) {
    art_state = -1;
    return sceKernelExitDeleteThread(0);
  }
  /* panel and X button: the game's menu sheet */
  static const int menu_mods[6] = { 342, 340, 335, 339, 341, 336 };
  ga_img menu[6];
  if (ga_cut("menu_16bpp", "menu", menu_mods, 6, 2, menu) == 0) {
    draw_panel(menu);
    base_ok = 1;
    float x0 = CX + 162 * PT, y0 = CY - 148 * PT; /* top-left corner of the X */
    x_x0 = (int)x0;
    x_y0 = (int)y0;
    int w = (int)ceilf(x0 - x_x0 + menu[5].w * PX) + 1, h = (int)ceilf(y0 - x_y0 + menu[5].h * PX) + 1;
    if (alloc_canvas(&c_x, w, h) && alloc_canvas(&c_x_lit, w, h)) {
      ga_blit(&c_x, &menu[5], x0 - x_x0, y0 - x_y0, x0 - x_x0 + menu[5].w * PX, y0 - x_y0 + menu[5].h * PX, 0, 0, 0);
      ga_blit(&c_x_lit, &menu[5], x0 - x_x0, y0 - x_y0, x0 - x_x0 + menu[5].w * PX, y0 - x_y0 + menu[5].h * PX, 0, 0, 1);
      x_ok = 1;
    }
    for (int i = 0; i < 6; i++)
      ga_free(&menu[i]);
  } else {
    /* the game's sheet could not be read: a plain white card, so the text can still be read */
    for (int y = 14; y < 515; y++)
      for (int x = 104 - BX0; x < 811 - BX0; x++)
        memset(c_base.px + ((size_t)y * BW + x) * 4, 255, 4);
    base_ok = 1;
  }
  /* the badge: the game's warning sign, as big as its event badges (icons x0.8;
   * the disco ball is 157 sheet pixels wide) */
  ga_draw_warning(&c_base, CX - 180 * PT - BX0, CY - 110 * PT - BY0, 157 * 0.8f * PX / 124.0f, 6.0f);
  /* the banner and its shine: the game's events sheet, module 130 (a white bar,
   * tinted blue by the animation: colour 4e96bf) and module 190 */
  static const int ev_mods[2] = { 130, 190 };
  ga_img ev[2];
  if (ga_cut("events_ui_16bpp", "events_ui", ev_mods, 2, 2, ev) == 0) {
    float x0 = CX - 338.5f * BANNER_S, y0 = BANNER_Y - 37.5f * BANNER_S;
    float x1 = x0 + ev[0].w * BANNER_S, y1 = y0 + ev[0].h * BANNER_S;
    banner_x0 = (int)x0;
    banner_y0 = (int)y0;
    if (alloc_canvas(&c_banner, (int)ceilf(x1) - banner_x0 + 1, (int)ceilf(y1) - banner_y0 + 1)) {
      ga_blit(&c_banner, &ev[0], x0 - banner_x0, y0 - banner_y0, x1 - banner_x0, y1 - banner_y0, 0, 0, 0);
      static const int tint[3] = { 0x4e, 0x96, 0xbf };
      for (int i = 0; i < c_banner.w * c_banner.h; i++)
        for (int k = 0; k < 3; k++)
          c_banner.px[4 * i + k] = (uint8_t)(c_banner.px[4 * i + k] * tint[k] / 255);
      banner_ok = 1;
    }
    ga_free(&ev[0]);
    shine = ev[1];
  }
  /* our words */
  text_px = png_load_rgba(TEXT_FILE, &text_w, &text_h);
  if (!text_px)
    so_log("[popup] could not read " TEXT_FILE);
  so_log("[popup] pictures made from the game's own art in %u ms (panel %s, banner %s)",
         (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), x_ok ? "ok" : "plain", banner_ok ? "ok" : "none");
  art_state = 1;
  return sceKernelExitDeleteThread(0);
}

static GLuint canvas_texture(ga_canvas *c) {
  if (!c->px)
    return 0;
  straight_alpha(c->px, c->w * c->h);
  GLuint t = make_texture(c->px, c->w, c->h);
  free(c->px);
  c->px = NULL;
  return t;
}

/* main thread: the finished pictures become textures */
static void upload_art(void) {
  tex_base = canvas_texture(&c_base);
  if (x_ok) {
    tex_x = canvas_texture(&c_x);
    tex_x_lit = canvas_texture(&c_x_lit);
  }
  if (banner_ok)
    tex_banner = canvas_texture(&c_banner);
  if (shine.px) {
    int sw = shine.w + 2 * shine.pad, sh = shine.h + 2 * shine.pad;
    straight_alpha(shine.px, sw * sh);
    tex_shine = make_texture(shine.px, sw, sh);
  }
  if (text_px) {
    tex_text = make_texture(text_px, text_w, text_h);
    free(text_px);
    text_px = NULL;
  }
  /* the game's pop-up backdrop: hazy blue, the colour its pop-ups ask for
   * (libcgame 0x19f3de -> 0x1835d0: 20, 50, 200 at 175 of 255) */
  static const unsigned char dim[4] = { 20, 50, 200, 175 };
  tex_dim = make_texture(dim, 1, 1);
  art_state = 2;
}

void popup_preload(void) {
  /* the menu button's warning sign must be ready before the game opens the menu sheet */
  art_patch_prepare();
  SceUID t = sceKernelCreateThread("popup_art", art_thread, 0x10000100 + 20, 0x20000, 0, 0, NULL);
  if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
    so_log("[popup] could not start making the pictures");
    art_state = -1;
  }
}

static int open_popup(void) {
  if (art_state < 0)
    return 0;
  opened_at = sceKernelGetProcessTimeWide();
  so_log("[popup] opened");
  return 1;
}

static void close_popup(void) { so_log("[popup] closed"); }

static void quad_uv(GLuint tex, float x, float y, float w, float h, float alpha, float u0, float v0, float u1,
                    float v1) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glColor4f(1.0f, 1.0f, 1.0f, alpha);
  glBegin(GL_TRIANGLE_STRIP);
  glTexCoord2f(u0, v0);
  glVertex2f(x, y);
  glTexCoord2f(u1, v0);
  glVertex2f(x + w, y);
  glTexCoord2f(u0, v1);
  glVertex2f(x, y + h);
  glTexCoord2f(u1, v1);
  glVertex2f(x + w, y + h);
  glEnd();
}

static void quad(GLuint tex, float x, float y, float w, float h, float alpha) {
  quad_uv(tex, x, y, w, h, alpha, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* a picture of the pop-up at screen position (x, y), size (w, h), scaled with the pop */
static void pop_quad(GLuint tex, float x, float y, float w, float h, float alpha, float u0, float v0, float u1,
                     float v1) {
  if (!tex)
    return;
  quad_uv(tex, CX + (x - CX) * pop_scale, CY + (y - CY) * pop_scale, w * pop_scale, h * pop_scale, alpha, u0, v0, u1,
          v1);
}

/* the banner's shine: events frames 23-26 (animation "pop_up_info_bis"), three
 * stretched copies of module 190 sweeping from left to right; between key
 * frames everything moves smoothly.  Times are in game frames (60 a second). */
static void draw_shine(float alpha) {
  static const struct { float x, sx; int a; } key[4][3] = {
    { { -380, 1.33f, 0x69 }, { -390, 1.33f, 0x69 }, { -390, 1.33f, 0x69 } },
    { { -205, 5.0f, 0x69 }, { -212, 2.0f, 0x69 }, { -235, 5.0f, 0x69 } },
    { { 227, 5.0f, 0x69 }, { 240, 2.0f, 0x69 }, { 237, 5.0f, 0x69 } },
    { { 330, 1.6f, 0x69 }, { 342, 1.51f, 0x7d }, { 340, 1.6f, 0x69 } },
  };
  static const int dur[4] = { 28, 55, 22, 88 };
  if (!tex_shine || !tex_banner)
    return;
  float t = fmodf((float)(sceKernelGetProcessTimeWide() - opened_at) * 60.0f / 1000000.0f, 193.0f);
  int i = 0;
  while (i < 3 && t >= dur[i]) {
    t -= dur[i];
    i++;
  }
  float f = i < 3 ? t / dur[i] : 0.0f;
  int j = i < 3 ? i + 1 : 3;
  /* only over the banner itself */
  float bx0 = CX - 338.5f * BANNER_S, bx1 = CX + 338.5f * BANNER_S;
  float by0 = BANNER_Y - 37.5f * BANNER_S, by1 = BANNER_Y + 37.5f * BANNER_S;
  float sw = shine.w + 2.0f * shine.pad, sh = shine.h + 2.0f * shine.pad;
  for (int p = 0; p < 3; p++) {
    float x = key[i][p].x + (key[j][p].x - key[i][p].x) * f;
    float sx = key[i][p].sx + (key[j][p].sx - key[i][p].sx) * f;
    float a = (key[i][p].a + (key[j][p].a - key[i][p].a) * f) / 255.0f;
    float cx = x + shine.w / 2.0f, cy = -11 + shine.h / 2.0f; /* scaled round its own centre */
    float w = shine.w * sx, h = shine.h * 3.16f;
    float x0 = CX + (cx - w / 2) * BANNER_S, x1 = CX + (cx + w / 2) * BANNER_S;
    float y0 = BANNER_Y + (cy - h / 2) * BANNER_S, y1 = BANNER_Y + (cy + h / 2) * BANNER_S;
    float u0 = shine.pad / sw, u1 = (shine.pad + shine.w) / sw, v0 = shine.pad / sh, v1 = (shine.pad + shine.h) / sh;
    if (x0 < bx0) { u0 += (u1 - u0) * (bx0 - x0) / (x1 - x0); x0 = bx0; }
    if (x1 > bx1) { u1 -= (u1 - u0) * (x1 - bx1) / (x1 - x0); x1 = bx1; }
    if (y0 < by0) { v0 += (v1 - v0) * (by0 - y0) / (y1 - y0); y0 = by0; }
    if (y1 > by1) { v1 -= (v1 - v0) * (y1 - by1) / (y1 - y0); y1 = by1; }
    if (x1 > x0 && y1 > y0)
      pop_quad(tex_shine, x0, y0, x1 - x0, y1 - y0, alpha * a, u0, v0, u1, v1);
  }
}

/* Everything the overlays (pop-up, credit) change is remembered first and put
 * back afterwards: the game remembers what it last set and does not set it again. */
typedef struct {
  GLint fbo, program, active, tex0, mode, vp[4], bs_rgb, bd_rgb, bs_a, bd_a, env_mode;
  GLfloat color[4];
  GLboolean blend, depth, scissor, cull, stencil, alpha_test, tex2d, tex2d_1;
} gl_saved;

static void overlay_begin(gl_saved *g) {
  memset(g, 0, sizeof(*g));
  g->active = GL_TEXTURE0;
  g->mode = GL_MODELVIEW;
  g->vp[2] = 960;
  g->vp[3] = 544;
  g->bs_rgb = g->bs_a = GL_ONE;
  g->bd_rgb = g->bd_a = GL_ZERO;
  g->env_mode = GL_MODULATE;
  g->color[0] = g->color[1] = g->color[2] = g->color[3] = 1.0f;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g->fbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &g->program);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &g->active);
  glGetIntegerv(GL_MATRIX_MODE, &g->mode);
  glGetIntegerv(GL_VIEWPORT, g->vp);
  glGetIntegerv(GL_BLEND_SRC_RGB, &g->bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &g->bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &g->bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &g->bd_a);
  glGetFloatv(GL_CURRENT_COLOR, g->color);
  g->blend = glIsEnabled(GL_BLEND);
  g->depth = glIsEnabled(GL_DEPTH_TEST);
  g->scissor = glIsEnabled(GL_SCISSOR_TEST);
  g->cull = glIsEnabled(GL_CULL_FACE);
  g->stencil = glIsEnabled(GL_STENCIL_TEST);
  g->alpha_test = glIsEnabled(GL_ALPHA_TEST);

  if (g->fbo)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glUseProgram(0);
  /* a second texture unit left switched on would make vitaGL's simple drawing multitexture */
  glActiveTexture(GL_TEXTURE1);
  g->tex2d_1 = glIsEnabled(GL_TEXTURE_2D);
  if (g->tex2d_1)
    glDisable(GL_TEXTURE_2D);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &g->tex0);
  g->tex2d = glIsEnabled(GL_TEXTURE_2D);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &g->env_mode);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); /* colour x texture, for fades */
  glViewport(0, 0, 960, 544);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_ALPHA_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_TEXTURE_2D);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, SCREEN_W, SCREEN_H, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
}

static void overlay_end(const gl_saved *g) {
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode((GLenum)g->mode);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, g->env_mode);
  if (!g->tex2d)
    glDisable(GL_TEXTURE_2D);
  glColor4f(g->color[0], g->color[1], g->color[2], g->color[3]);
  glBindTexture(GL_TEXTURE_2D, (GLuint)g->tex0); /* unit 0 is still the active one here */
  if (g->tex2d_1) {
    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_TEXTURE_2D);
  }
  glActiveTexture((GLenum)g->active);
  glBlendFuncSeparate((GLenum)g->bs_rgb, (GLenum)g->bd_rgb, (GLenum)g->bs_a, (GLenum)g->bd_a);
  if (!g->blend) glDisable(GL_BLEND);
  if (g->depth) glEnable(GL_DEPTH_TEST);
  if (g->scissor) glEnable(GL_SCISSOR_TEST);
  if (g->cull) glEnable(GL_CULL_FACE);
  if (g->stencil) glEnable(GL_STENCIL_TEST);
  if (g->alpha_test) glEnable(GL_ALPHA_TEST);
  glUseProgram((GLuint)g->program);
  if (g->fbo)
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g->fbo);
  glViewport(g->vp[0], g->vp[1], g->vp[2], g->vp[3]);
}

void popup_draw(void) {
  if (art_state == 1)
    upload_art();
  if (!showing || art_state != 2)
    return;
  gl_saved g;
  overlay_begin(&g);

  /* a quick "pop" like the game's own windows: grow from 85% and fade in over ~8 frames */
  float t = frames >= 8 ? 1.0f : frames / 8.0f;
  float ease = 1.0f - (1.0f - t) * (1.0f - t);
  float scale = 0.85f + 0.15f * ease + 0.04f * sinf(t * 3.14159f);
  float alpha = ease;
  if (closing > HIGHLIGHT_FRAMES) { /* closing: shrink a little and fade out */
    float f = (closing - HIGHLIGHT_FRAMES) / (float)FADE_FRAMES;
    if (f > 1.0f)
      f = 1.0f;
    alpha *= 1.0f - f;
    scale *= 1.0f - 0.06f * f;
  }
  pop_scale = scale;
  quad(tex_dim, 0.0f, 0.0f, SCREEN_W, SCREEN_H, alpha);
  pop_quad(tex_base, BX0, BY0, BW, BH, alpha, 0, 0, 1, 1);
  if (tex_banner)
    pop_quad(tex_banner, banner_x0, banner_y0, c_banner.w, c_banner.h, alpha, 0, 0, 1, 1);
  draw_shine(alpha);
  pop_quad(tex_text, BX0, BY0, BW, BH, alpha, 0, 0, 1, 1);
  /* while held (and briefly after letting go) the X is lit up, like the game's buttons */
  GLuint x = (touch_held || closing) && tex_x_lit ? tex_x_lit : tex_x;
  pop_quad(x, x_x0, x_y0, c_x.w, c_x.h, alpha, 0, 0, 1, 1);
  frames++;

  overlay_end(&g);
}

/* ---- the credit line on the start-up loading screen ----
 * "PS Vita port by rodi - v1.0" in the game's own font (app0:credit.png, made by
 * tools/make_credit.py), along the bottom of the screen while the game loads.
 * It fades in, and fades out once the game says loading is done
 * (setNativeLoadingDone, jni.c). */
#define CREDIT_FILE "app0:credit.png"
#define CREDIT_FADE_IN 15
#define CREDIT_FADE_OUT 20
static GLuint tex_credit;
static int credit_w, credit_h, credit_frames, credit_out = -1;

void credit_preload(void) {
  int w = 0, h = 0;
  unsigned char *px = png_load_rgba(CREDIT_FILE, &w, &h);
  if (!px) {
    so_log("[credit] could not read " CREDIT_FILE);
    return;
  }
  tex_credit = make_texture(px, w, h);
  free(px);
  credit_w = w;
  credit_h = h;
}

void credit_draw(void) {
  extern volatile int g_loading_done;
  if (!tex_credit || credit_out >= CREDIT_FADE_OUT)
    return;
  if (g_loading_done && credit_out < 0)
    credit_out = 0;
  float a = credit_frames >= CREDIT_FADE_IN ? 1.0f : credit_frames / (float)CREDIT_FADE_IN;
  if (credit_out >= 0)
    a *= 1.0f - credit_out / (float)CREDIT_FADE_OUT;
  credit_frames++;
  if (credit_out >= 0)
    credit_out++;
  gl_saved g;
  overlay_begin(&g);
  /* bottom centre, on whole pixels so it stays sharp */
  float x = (float)((960 - credit_w) / 2), y = (float)(544 - credit_h - 6);
  quad(tex_credit, x, y, (float)credit_w, (float)credit_h, a);
  overlay_end(&g);
}

#else
void credit_preload(void) {}
void credit_draw(void) {}
void popup_preload(void) {}
static int open_popup(void) { return 0; }
static void close_popup(void) {}
void popup_draw(void) {}
#endif

int popup_is_open(void) { return showing; }

int popup_frame_start(void) {
  SceTouchData td;
  memset(&td, 0, sizeof(td));
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
  int touching = td.reportNum > 0;

  if (!showing) {
    if (!requested)
      return 0;
    requested = 0;
    if (!open_popup())
      return 0;
    showing = 1;
    frames = closing = touch_held = touch_armed = 0;
    touch_prev = touching;
    /* whatever is held right now (the finger that pressed the button) must be let go first */
    wait_release = touching;
    input_release_touches();
    return 1;
  }

  if (closing) {
    if (++closing > HIGHLIGHT_FRAMES + FADE_FRAMES) {
      showing = 0;
      close_popup();
      /* a finger / button still down must not reach the game */
      input_ignore_until_released();
    }
    return 1;
  }

  /* only tapping the X closes it: a finger pressed on the X lights it up,
   * letting go there closes (sliding off first cancels, like the game's
   * buttons).  The Vita's buttons do nothing while it is open (the game gets no
   * input either, see stage2b.c). */
  int let_go_x = 0;
  if (wait_release) {
    if (!touching)
      wait_release = 0;
  } else if (frames > 8) {
    int on_x = 0;
    for (int i = 0; i < (int)td.reportNum && i < 6; i++) {
      float x = td.report[i].x * 0.5f, y = td.report[i].y * 0.5f;
      if (x >= X_HIT_X0 && x < X_HIT_X1 && y >= X_HIT_Y0 && y < X_HIT_Y1)
        on_x = 1;
    }
    if (touching && !touch_prev)
      touch_armed = on_x; /* a new press: it counts only if it starts on the X */
    if (touching) {
      touch_held = touch_armed && on_x;
    } else {
      let_go_x = touch_held;
      touch_held = touch_armed = 0;
    }
  }
  touch_prev = touching;
  if (let_go_x)
    closing = 1;
  return 1;
}
