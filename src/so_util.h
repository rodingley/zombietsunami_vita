#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include "elf.h"

#define SO_PAGE 0x1000u
#define SO_PAGE_UP(x) (((x) + (SO_PAGE - 1)) & ~(SO_PAGE - 1))
#define SO_PAGE_DOWN(x) ((x) & ~(SO_PAGE - 1))

/* One loaded Android .so. The whole image is mapped in two blocks:
 *   text block: every non-writable segment, at text_base (ELF vaddr 0)
 *   data block: every writable segment, directly after the text block
 * so all ELF virtual addresses stay valid as text_base + vaddr. */
typedef struct so_module {
  struct so_module *next;

  int text_blockid, data_blockid;
  uintptr_t text_base, data_base;
  size_t text_size, data_size;

  Elf32_Dyn *dynamic;
  Elf32_Sym *dynsym;
  Elf32_Rel *reldyn;
  Elf32_Rel *relplt;
  uint32_t *init_array; /* 32-bit function pointers, as in the file */
  uint32_t *hash;

  uintptr_t exidx;      /* ARM unwind table, for dl_unwind_find_exidx */
  int num_exidx;

  int num_dynamic;
  int num_dynsym;
  int num_reldyn;
  int num_relplt;
  int num_init_array;

  char *soname;
  char *dynstr;
  char name[64];
} so_module;

/* A symbol the game's libraries import that we provide ourselves. */
typedef struct {
  const char *symbol;
  uintptr_t func;
} so_default_dynlib;

int so_load(so_module *mod, const char *filename, uintptr_t load_addr);
int so_relocate(so_module *mod);
/* Returns the number of imports that could not be resolved (logged). */
int so_resolve(so_module *mod, const so_default_dynlib *table, int table_count);
void so_initialize(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *symbol);
void so_flush_caches(so_module *mod);

/* Finds the unwind table of the module containing pc. Returns 0 if none. */
uintptr_t so_find_exidx(uintptr_t pc, int *count);

#endif
