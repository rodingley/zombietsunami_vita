/* gl_real.c -- real OpenGL through vitaGL (stage 3).
 *
 * Almost every gl* function the game calls goes straight to vitaGL
 * (tools/gen_direct.sh links them). Only the two calls below are wrapped,
 * so that a shader which fails to compile or link shows up in log.txt.
 */

#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "so_platform.h"
#include "so_util.h"

static int shader_count, shader_fail, program_count, program_fail;

static void zt_compile_shader(GLuint sh) {
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  shader_count++;
  if (!ok) {
    char buf[600];
    GLsizei n = 0;
    buf[0] = 0;
    glGetShaderInfoLog(sh, sizeof(buf) - 1, &n, buf);
    buf[n >= 0 && n < (GLsizei)sizeof(buf) ? n : 0] = 0;
    if (shader_fail++ < 12)
      so_log("[gl] shader %u FAILED to compile: %s", (unsigned)sh, buf);
  } else if (shader_count <= 3) {
    so_log("[gl] shader %u compiled ok", (unsigned)sh);
  }
}

static void zt_link_program(GLuint pr) {
  glLinkProgram(pr);
  GLint ok = 0;
  glGetProgramiv(pr, GL_LINK_STATUS, &ok);
  program_count++;
  if (!ok) {
    if (program_fail++ < 12)
      so_log("[gl] program %u FAILED to link", (unsigned)pr);
  } else if (program_count <= 3) {
    so_log("[gl] program %u linked ok", (unsigned)pr);
  }
}

/* time spent handing textures to the GPU, added up (shown in the [fps] lines) */
volatile uint64_t g_load_upload_us;


static void zt_tex_image(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                         GLenum format, GLenum type, const void *data) {
  uint64_t t = sceKernelGetProcessTimeWide();
  glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
  g_load_upload_us += sceKernelGetProcessTimeWide() - t;
}

static void zt_tex_sub_image(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, const void *data) {
  uint64_t t = sceKernelGetProcessTimeWide();
  glTexSubImage2D(target, level, x, y, width, height, format, type, data);
  g_load_upload_us += sceKernelGetProcessTimeWide() - t;
}

static void zt_compressed_tex_image(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                                    GLint border, GLsizei size, const void *data) {
  uint64_t t = sceKernelGetProcessTimeWide();
  glCompressedTexImage2D(target, level, internalformat, width, height, border, size, data);
  g_load_upload_us += sceKernelGetProcessTimeWide() - t;
}

#define S(name, fn) { name, (uintptr_t)(fn) }

const so_default_dynlib gl_dynlib[] = {
  S("glCompileShader", zt_compile_shader),
  S("glLinkProgram", zt_link_program),
  S("glTexImage2D", zt_tex_image),
  S("glTexSubImage2D", zt_tex_sub_image),
  S("glCompressedTexImage2D", zt_compressed_tex_image),
};

const int gl_dynlib_count = sizeof(gl_dynlib) / sizeof(gl_dynlib[0]);
