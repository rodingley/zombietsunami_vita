/* gl_stubs.c -- fake OpenGL ES for stage 2b.
 *
 * Nothing is drawn yet. The functions here only answer the questions the
 * game asks while starting up (which GL version? did the shader compile?
 * is the framebuffer complete?) so that its initialisation can proceed.
 * Every other gl* function is a do-nothing stub (gl_stub in imports.c).
 * Real graphics (vitaGL) come in a later stage.
 */

#include <stdint.h>
#include <string.h>

#include "so_util.h"

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_VIEWPORT 0x0BA2
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_VARYING_VECTORS 0x8DFC
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

static const char *gl_get_string(unsigned name) {
  switch (name) {
    case GL_VENDOR: return "Sony Computer Entertainment";
    case GL_RENDERER: return "PowerVR SGX543MP4+";
    case GL_VERSION: return "OpenGL ES 2.0 (vita stub)";
    case GL_EXTENSIONS:
      return "GL_OES_packed_depth_stencil GL_OES_depth_texture GL_IMG_texture_compression_pvrtc";
    default: return "";
  }
}

static void gl_get_integerv(unsigned pname, int *p) {
  switch (pname) {
    case GL_VIEWPORT: p[0] = 0; p[1] = 0; p[2] = 960; p[3] = 544; break;
    case GL_MAX_TEXTURE_SIZE: p[0] = 4096; break;
    case GL_MAX_VERTEX_ATTRIBS: p[0] = 16; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: p[0] = 8; break;
    case GL_MAX_VERTEX_UNIFORM_VECTORS: p[0] = 128; break;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: p[0] = 64; break;
    case GL_MAX_VARYING_VECTORS: p[0] = 8; break;
    default: p[0] = 0; break;
  }
}

static int gl_get_error(void) { return 0; }

static unsigned next_object = 1;
static unsigned gl_create(void) { return next_object++; }

static void gl_gen(int n, unsigned *ids) {
  for (int i = 0; i < n; i++)
    ids[i] = next_object++;
}

static void gl_get_shaderiv(unsigned sh, unsigned pname, int *p) {
  (void)sh;
  *p = (pname == GL_COMPILE_STATUS) ? 1 : 0;
}

static void gl_get_programiv(unsigned pr, unsigned pname, int *p) {
  (void)pr;
  *p = (pname == GL_LINK_STATUS) ? 1 : 0;
}

static void gl_get_shader_info_log(unsigned sh, int max, int *len, char *buf) {
  (void)sh;
  if (len)
    *len = 0;
  if (max > 0 && buf)
    buf[0] = '\0';
}

static int gl_loc(void) { return 0; }
static unsigned gl_check_fb(unsigned target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }

#define S(name, fn) { name, (uintptr_t)(fn) }

const so_default_dynlib gl_dynlib[] = {
  S("glGetString", gl_get_string),
  S("glGetIntegerv", gl_get_integerv),
  S("glGetError", gl_get_error),
  S("glCreateShader", gl_create),
  S("glCreateProgram", gl_create),
  S("glGenTextures", gl_gen),
  S("glGenFramebuffers", gl_gen),
  S("glGenRenderbuffers", gl_gen),
  S("glGetShaderiv", gl_get_shaderiv),
  S("glGetProgramiv", gl_get_programiv),
  S("glGetShaderInfoLog", gl_get_shader_info_log),
  S("glGetUniformLocation", gl_loc),
  S("glGetAttribLocation", gl_loc),
  S("glCheckFramebufferStatus", gl_check_fb),
};

const int gl_dynlib_count = sizeof(gl_dynlib) / sizeof(gl_dynlib[0]);

/* statistics counter that the real-GL build fills in (gl_real.c) */
volatile uint64_t g_load_upload_us;
