#ifndef STAGE1_H
#define STAGE1_H

#include "so_util.h"

#define NUM_GAME_LIBS 4

extern so_module game_mods[NUM_GAME_LIBS];
/* hand-written replacements (imports.c) and generated direct links (direct_imports.c) */
extern const so_default_dynlib shim_dynlib[];
extern const int shim_dynlib_count;
extern const so_default_dynlib direct_dynlib[];
extern const int direct_dynlib_count;
extern const so_default_dynlib gl_dynlib[];
extern const int gl_dynlib_count;
void imports_init(void);

/* Loads libc++_shared, libfmod, libfmodstudio and libcgame from lib_dir,
 * relocates and links them, and logs everything that could not be
 * resolved. Does NOT run any of the game's code.
 * Returns the total number of unresolved imports, or -1 on a load error. */
int stage1_run(const char *lib_dir, const uintptr_t load_addrs[NUM_GAME_LIBS]);

/* Stage 2a: runs the .init_array (static constructors) of every library,
 * in order libc++, fmod, fmodstudio, cgame. Call after stage1_run(). */
void stage2a_run_inits(void);

/* Stage 2b: fake Java VM, JNI_OnLoad, nativeInit, a few frames */
void stage2b_run(void);

#endif
