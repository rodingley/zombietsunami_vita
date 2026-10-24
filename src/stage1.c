/* stage1.c -- load the four 32-bit libraries and report what is missing */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_platform.h"
#include "stage1.h"

so_module game_mods[NUM_GAME_LIBS];

static const char *lib_names[NUM_GAME_LIBS] = {
  "libc++_shared.so", "libfmod.so", "libfmodstudio.so", "libcgame.so",
};

static const char *probe_symbols[] = {
  "JNI_OnLoad",
  "Java_net_mobigame_artemis_GameViewRenderer_nativeInit",
  "Java_net_mobigame_artemis_GameViewRenderer_nativeRender",
  "Java_net_mobigame_artemis_GameViewRenderer_nativeTouch",
  "_ZN4FMOD6Studio6System10initializeEijjPv",
};

int stage1_run(const char *lib_dir, const uintptr_t load_addrs[NUM_GAME_LIBS]) {
  char path[256];

  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    snprintf(path, sizeof(path), "%s/%s", lib_dir, lib_names[i]);
    int res = so_load(&game_mods[i], path, load_addrs[i]);
    if (res < 0) {
      so_log("LOAD FAILED %s (error %08x)", path, (unsigned)res);
      return -1;
    }
    so_module *m = &game_mods[i];
    so_log("loaded %-18s text %08x +%06x  data %08x +%06x  exidx %08x (%d)",
           m->name, (unsigned)m->text_base, (unsigned)m->text_size,
           (unsigned)m->data_base, (unsigned)m->data_size,
           (unsigned)m->exidx, m->num_exidx);
  }

  for (int i = 0; i < NUM_GAME_LIBS; i++)
    so_relocate(&game_mods[i]);
  so_log("relocated all modules");

  int table_count = shim_dynlib_count + gl_dynlib_count + direct_dynlib_count;
  so_default_dynlib *table = malloc(sizeof(so_default_dynlib) * (table_count ? table_count : 1));
  if (!table) {
    so_log("out of memory for the import table");
    return -1;
  }
  int tn = 0;
  for (int i = 0; i < shim_dynlib_count; i++)
    table[tn++] = shim_dynlib[i];
  for (int i = 0; i < gl_dynlib_count; i++)
    table[tn++] = gl_dynlib[i];
  for (int i = 0; i < direct_dynlib_count; i++)
    if (direct_dynlib[i].func)
      table[tn++] = direct_dynlib[i];
  so_log("import table: %d hand-written + %d direct", shim_dynlib_count, tn - shim_dynlib_count);

  /* libcgame gets extra logging wrappers around some FMOD calls */
  extern const so_default_dynlib fmod_log_dynlib[];
  extern const int fmod_log_dynlib_count;
  so_default_dynlib *table_game = malloc(sizeof(so_default_dynlib) * (tn + fmod_log_dynlib_count));
  memcpy(table_game, table, sizeof(so_default_dynlib) * tn);
  /* wrappers first so they win over any duplicates */
  memmove(table_game + fmod_log_dynlib_count, table_game, sizeof(so_default_dynlib) * tn);
  memcpy(table_game, fmod_log_dynlib, sizeof(so_default_dynlib) * fmod_log_dynlib_count);

  int missing = 0;
  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    int n = (i == NUM_GAME_LIBS - 1) ? so_resolve(&game_mods[i], table_game, tn + fmod_log_dynlib_count)
                                     : so_resolve(&game_mods[i], table, tn);
    so_log("resolved %-18s missing imports: %d", game_mods[i].name, n);
    missing += n;
  }

  extern void patches_apply(void);
  patches_apply();

  for (int i = 0; i < NUM_GAME_LIBS; i++)
    so_flush_caches(&game_mods[i]);

  for (unsigned s = 0; s < sizeof(probe_symbols) / sizeof(probe_symbols[0]); s++) {
    for (int i = 0; i < NUM_GAME_LIBS; i++) {
      uintptr_t a = so_symbol(&game_mods[i], probe_symbols[s]);
      if (a)
        so_log("symbol %s in %s at %08x (offset %06x)", probe_symbols[s],
               game_mods[i].name, (unsigned)a, (unsigned)(a - game_mods[i].text_base));
    }
  }

  free(table);
  so_log("total missing imports: %d", missing);
  return missing;
}

void stage2a_run_inits(void) {
  imports_init();
  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    so_log("running init array of %s", game_mods[i].name);
    so_initialize(&game_mods[i]);
    so_log("init array of %s finished", game_mods[i].name);
  }
  so_log("ALL INITS DONE");
}
