/* host_test.c -- run the real stage 1 on a PC against the real .so files.
 *
 * usage: host_test <dir with the four .so files>
 * Prints machine-readable lines starting with "CHECK" for the Python checker.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/so_platform.h"
#include "../src/stage1.h"

typedef uint32_t w32;

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; printf("CHECK FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int in_module(const so_module *m, uintptr_t a) {
  return a >= m->text_base && a < m->data_base + m->data_size;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <libdir>\n", argv[0]);
    return 1;
  }
  const uintptr_t addrs[NUM_GAME_LIBS] = {0x98000000, 0x99000000, 0x9A000000, 0x9B000000};
  int missing = stage1_run(argv[1], addrs);
  EXPECT(missing >= 0, "stage1_run failed to load");
  if (missing < 0)
    return 1;

  for (int i = 0; i < NUM_GAME_LIBS; i++) {
    so_module *m = &game_mods[i];

    /* 1. every R_ARM_RELATIVE target must now point inside this module */
    int rel_bad = 0, rel_n = 0;
    for (int r = 0; r < m->num_reldyn; r++) {
      if (ELF32_R_TYPE(m->reldyn[r].r_info) != R_ARM_RELATIVE)
        continue;
      rel_n++;
      w32 v = *(w32 *)(m->text_base + m->reldyn[r].r_offset);
      /* a relative reloc may legitimately hold a plain integer, but the vast
       * majority must land inside the image; count the outliers */
      if (!in_module(m, v))
        rel_bad++;
    }
    printf("CHECK relative %s total=%d outside_image=%d\n", m->name, rel_n, rel_bad);
    EXPECT(rel_n > 0 && rel_bad * 100 < rel_n, "%s: too many relative relocs outside image", m->name);

    /* 2. init_array entries must point into the text block */
    int init_bad = 0;
    for (int k = 0; k < m->num_init_array; k++) {
      w32 fn = m->init_array[k];
      if (fn && fn != 0xffffffffu && !(fn >= m->text_base && fn < m->text_base + m->text_size))
        init_bad++;
    }
    printf("CHECK init_array %s entries=%d outside_text=%d\n", m->name, m->num_init_array, init_bad);
    EXPECT(init_bad == 0, "%s: init_array entries outside text", m->name);

    /* 3. every resolved import slot must point into a module or into our table */
    int slot_n = 0, slot_cross = 0, slot_unresolved = 0;
    int total = m->num_reldyn + m->num_relplt;
    for (int r = 0; r < total; r++) {
      Elf32_Rel *rel = r < m->num_reldyn ? &m->reldyn[r] : &m->relplt[r - m->num_reldyn];
      int type = ELF32_R_TYPE(rel->r_info);
      if (type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
        continue;
      Elf32_Sym *sym = &m->dynsym[ELF32_R_SYM(rel->r_info)];
      if (sym->st_shndx != SHN_UNDEF)
        continue;
      slot_n++;
      w32 v = *(w32 *)(m->text_base + rel->r_offset);
      int found = 0;
      for (int j = 0; j < NUM_GAME_LIBS; j++)
        if (j != i && v >= game_mods[j].text_base && v < game_mods[j].text_base + game_mods[j].text_size)
          found = 1;
      if (found)
        slot_cross++;
      else if (in_module(m, v))
        slot_unresolved++; /* still holds the original in-image value */
    }
    printf("CHECK imports %s undefined_slots=%d resolved_to_other_lib=%d left_unresolved=%d\n",
           m->name, slot_n, slot_cross, slot_unresolved);
  }

  /* 4. symbol lookup offsets for the Python checker to compare with readelf */
  const char *probes[] = {"JNI_OnLoad", "Java_net_mobigame_artemis_GameViewRenderer_nativeInit",
                          "_ZN4FMOD6Studio6System10initializeEijjPv", "FMOD_System_Create"};
  for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++)
    for (int i = 0; i < NUM_GAME_LIBS; i++) {
      uintptr_t a = so_symbol(&game_mods[i], probes[p]);
      if (a)
        printf("CHECK sym %s %s 0x%x\n", game_mods[i].name, probes[p], (unsigned)(a - game_mods[i].text_base));
    }

  /* 5. unwind lookup */
  int cnt = 0;
  uintptr_t ex = so_find_exidx(game_mods[3].text_base + 0x1000, &cnt);
  printf("CHECK exidx libcgame 0x%x count=%d\n", (unsigned)(ex - game_mods[3].text_base), cnt);

  printf("CHECK total_missing %d failures %d\n", missing, failures);
  return failures ? 1 : 0;
}
