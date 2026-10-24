/* so_util.c -- load Android ARM32 .so files into memory and link them
 *
 * Based on so_util.c from gtasa_vita by Andy Nguyen (MIT licence, see
 * LICENSE-so_util-AndyNguyen). Changes: platform layer split out, segments
 * grouped by permission (the original assumed one code + one data segment,
 * which does not match these libraries), several modules can be loaded
 * and linked, missing imports are reported, unwind table lookup added.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "so_platform.h"
#include "so_util.h"

typedef uint32_t so_word; /* a pointer-sized word in the target image */

#define MAX_REPORTED 1024

static so_module *head = NULL, *tail = NULL;

void so_flush_caches(so_module *mod) {
  so_plat_flush(mod->text_base, mod->text_size);
}

static void zero_fill(void *dst, size_t size) {
  static const uint8_t zero[SO_PAGE];
  for (size_t off = 0; off < size; off += SO_PAGE) {
    size_t n = size - off < SO_PAGE ? size - off : SO_PAGE;
    so_plat_write((uint8_t *)dst + off, zero, n);
  }
}

int so_load(so_module *mod, const char *filename, uintptr_t load_addr) {
  int res = 0;
  int file_blockid = -1;
  void *so_data = NULL;
  size_t so_size = 0;

  memset(mod, 0, sizeof(*mod));
  mod->text_blockid = mod->data_blockid = -1;

  res = so_plat_read_file(filename, &so_data, &so_size, &file_blockid);
  if (res < 0)
    return res;

  if (so_size < sizeof(Elf32_Ehdr) || memcmp(so_data, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_file;
  }

  Elf32_Ehdr *ehdr = (Elf32_Ehdr *)so_data;
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_machine != EM_ARM) {
    so_log("%s: not a 32-bit ARM ELF", filename);
    res = -1;
    goto err_free_file;
  }

  Elf32_Phdr *phdr = (Elf32_Phdr *)((uintptr_t)so_data + ehdr->e_phoff);
  Elf32_Shdr *shdr = (Elf32_Shdr *)((uintptr_t)so_data + ehdr->e_shoff);
  const char *shstr = (const char *)so_data + shdr[ehdr->e_shstrndx].sh_offset;

  /* Work out the two blocks. */
  uint32_t min_vaddr = 0xffffffffu;
  uint32_t first_rw = 0xffffffffu;
  uint32_t text_end = 0, data_end = 0;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != PT_LOAD)
      continue;
    uint32_t start = phdr[i].p_vaddr;
    uint32_t end = phdr[i].p_vaddr + phdr[i].p_memsz;
    if (start < min_vaddr)
      min_vaddr = start;
    if (phdr[i].p_flags & PF_W) {
      if (start < first_rw)
        first_rw = start;
      if (end > data_end)
        data_end = end;
    } else {
      if (end > text_end)
        text_end = end;
    }
    /* file offset and address must agree within a page */
    if ((phdr[i].p_offset & (SO_PAGE - 1)) != (phdr[i].p_vaddr & (SO_PAGE - 1))) {
      so_log("%s: segment %d offset/vaddr misaligned", filename, i);
      res = -3;
      goto err_free_file;
    }
  }

  if (min_vaddr != 0 || first_rw == 0xffffffffu || text_end == 0) {
    so_log("%s: unexpected segment layout (min_vaddr=%x)", filename, min_vaddr);
    res = -3;
    goto err_free_file;
  }

  uint32_t data_start = SO_PAGE_DOWN(first_rw);
  if (SO_PAGE_UP(text_end) > data_start) {
    so_log("%s: code and data share a page (text_end=%x data_start=%x)",
           filename, text_end, data_start);
    res = -3;
    goto err_free_file;
  }
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_W) &&
        phdr[i].p_vaddr < data_start) {
      so_log("%s: writable segment below data start", filename);
      res = -3;
      goto err_free_file;
    }
  }

  size_t text_size = data_start;
  size_t data_size = SO_PAGE_UP(data_end) - data_start;

  res = so_plat_alloc("so_text", text_size, load_addr, 1,
                      &mod->text_blockid, &mod->text_base);
  if (res < 0)
    goto err_free_file;
  if (mod->text_base != load_addr)
    so_log("%s: wanted text at %08x, got %08x", filename,
           (unsigned)load_addr, (unsigned)mod->text_base);

  res = so_plat_alloc("so_data", data_size, mod->text_base + text_size, 0,
                      &mod->data_blockid, &mod->data_base);
  if (res < 0)
    goto err_free_text;
  if (mod->data_base != mod->text_base + text_size) {
    so_log("%s: data block not contiguous with text", filename);
    res = -4;
    goto err_free_data;
  }

  mod->text_size = text_size;
  mod->data_size = data_size;

  zero_fill((void *)mod->text_base, text_size);
  zero_fill((void *)mod->data_base, data_size);

  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type == PT_LOAD) {
      so_plat_write((void *)(mod->text_base + phdr[i].p_vaddr),
                    (uint8_t *)so_data + phdr[i].p_offset, phdr[i].p_filesz);
    } else if (phdr[i].p_type == PT_ARM_EXIDX) {
      mod->exidx = mod->text_base + phdr[i].p_vaddr;
      mod->num_exidx = phdr[i].p_memsz / 8;
    }
  }

  for (int i = 0; i < ehdr->e_shnum; i++) {
    const char *sh_name = shstr + shdr[i].sh_name;
    uintptr_t sh_addr = mod->text_base + shdr[i].sh_addr;
    size_t sh_size = shdr[i].sh_size;
    if (strcmp(sh_name, ".dynamic") == 0) {
      mod->dynamic = (Elf32_Dyn *)sh_addr;
      mod->num_dynamic = sh_size / sizeof(Elf32_Dyn);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstr = (char *)sh_addr;
    } else if (strcmp(sh_name, ".dynsym") == 0) {
      mod->dynsym = (Elf32_Sym *)sh_addr;
      mod->num_dynsym = sh_size / sizeof(Elf32_Sym);
    } else if (strcmp(sh_name, ".rel.dyn") == 0) {
      mod->reldyn = (Elf32_Rel *)sh_addr;
      mod->num_reldyn = sh_size / sizeof(Elf32_Rel);
    } else if (strcmp(sh_name, ".rel.plt") == 0) {
      mod->relplt = (Elf32_Rel *)sh_addr;
      mod->num_relplt = sh_size / sizeof(Elf32_Rel);
    } else if (strcmp(sh_name, ".init_array") == 0) {
      mod->init_array = (uint32_t *)sh_addr;
      mod->num_init_array = sh_size / sizeof(uint32_t);
    } else if (strcmp(sh_name, ".hash") == 0) {
      mod->hash = (uint32_t *)sh_addr;
    }
  }

  if (!mod->dynamic || !mod->dynstr || !mod->dynsym || !mod->reldyn || !mod->relplt) {
    so_log("%s: missing a required dynamic section", filename);
    res = -2;
    goto err_free_data;
  }

  for (int i = 0; i < mod->num_dynamic; i++) {
    if (mod->dynamic[i].d_tag == DT_SONAME)
      mod->soname = mod->dynstr + mod->dynamic[i].d_un.d_ptr;
  }
  if (!mod->soname) {
    so_log("%s: no SONAME", filename);
    res = -2;
    goto err_free_data;
  }

  strncpy(mod->name, mod->soname, sizeof(mod->name) - 1);

  so_plat_free(file_blockid);

  if (!head && !tail) {
    head = tail = mod;
  } else {
    tail->next = mod;
    tail = mod;
  }
  return 0;

err_free_data:
  so_plat_free(mod->data_blockid);
err_free_text:
  so_plat_free(mod->text_blockid);
err_free_file:
  so_plat_free(file_blockid);
  return res;
}

int so_relocate(so_module *mod) {
  int total = mod->num_reldyn + mod->num_relplt;
  for (int i = 0; i < total; i++) {
    Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i]
                                         : &mod->relplt[i - mod->num_reldyn];
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
    so_word *ptr = (so_word *)(mod->text_base + rel->r_offset);
    so_word val;

    int type = ELF32_R_TYPE(rel->r_info);
    switch (type) {
      case R_ARM_ABS32:
        if (sym->st_shndx != SHN_UNDEF) {
          val = *ptr + (so_word)mod->text_base + sym->st_value;
          so_plat_write(ptr, &val, sizeof(val));
        }
        break;

      case R_ARM_RELATIVE:
        val = *ptr + (so_word)mod->text_base;
        so_plat_write(ptr, &val, sizeof(val));
        break;

      case R_ARM_GLOB_DAT:
      case R_ARM_JUMP_SLOT:
        if (sym->st_shndx != SHN_UNDEF) {
          val = (so_word)mod->text_base + sym->st_value;
          so_plat_write(ptr, &val, sizeof(val));
        }
        break;

      default:
        so_fatal("%s: unknown relocation type %d", mod->name, type);
    }
  }
  return 0;
}

static uint32_t so_hash(const uint8_t *name) {
  uint32_t h = 0, g;
  while (*name) {
    h = (h << 4) + *name++;
    if ((g = (h & 0xf0000000u)) != 0)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
  if (mod->hash) {
    uint32_t hash = so_hash((const uint8_t *)symbol);
    uint32_t nbucket = mod->hash[0];
    uint32_t *bucket = &mod->hash[2];
    uint32_t *chain = &bucket[nbucket];
    for (uint32_t i = bucket[hash % nbucket]; i; i = chain[i]) {
      if (mod->dynsym[i].st_shndx == SHN_UNDEF)
        continue;
      if (strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
        return mod->text_base + mod->dynsym[i].st_value;
    }
    return 0;
  }

  /* No SysV hash table (only .gnu.hash): scan the symbol table. */
  for (int i = 0; i < mod->num_dynsym; i++) {
    if (mod->dynsym[i].st_shndx == SHN_UNDEF)
      continue;
    if (strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
      return mod->text_base + mod->dynsym[i].st_value;
  }
  return 0;
}

/* Look in the libraries this module lists as DT_NEEDED. */
static uintptr_t so_resolve_link(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_dynamic; i++) {
    if (mod->dynamic[i].d_tag != DT_NEEDED)
      continue;
    const char *needed = mod->dynstr + mod->dynamic[i].d_un.d_ptr;
    for (so_module *curr = head; curr; curr = curr->next) {
      if (curr != mod && curr->soname && strcmp(curr->soname, needed) == 0) {
        uintptr_t link = so_symbol(curr, symbol);
        if (link)
          return link;
      }
    }
  }
  return 0;
}

int so_resolve(so_module *mod, const so_default_dynlib *table, int table_count) {
  static const char *reported[MAX_REPORTED];
  int num_reported = 0;
  int missing = 0;
  int total = mod->num_reldyn + mod->num_relplt;

  for (int i = 0; i < total; i++) {
    Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i]
                                         : &mod->relplt[i - mod->num_reldyn];
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
    so_word *ptr = (so_word *)(mod->text_base + rel->r_offset);
    int type = ELF32_R_TYPE(rel->r_info);

    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
      continue;
    if (sym->st_shndx != SHN_UNDEF)
      continue;

    const char *name = mod->dynstr + sym->st_name;
    uintptr_t target = 0;

    /* Our own replacements win over anything found in the other libraries
     * only when listed in the table; otherwise use the libraries. */
    for (int j = 0; j < table_count; j++) {
      if (strcmp(name, table[j].symbol) == 0) {
        target = table[j].func;
        break;
      }
    }
    if (!target)
      target = so_resolve_link(mod, name);

    if (target) {
      so_word val = (so_word)target;
      if (type == R_ARM_ABS32)
        val += *ptr;
      so_plat_write(ptr, &val, sizeof(val));
      continue;
    }

    /* An unresolved WEAK import must read as NULL (code tests it before use) */
    if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
      so_word val = (type == R_ARM_ABS32) ? *ptr : 0;
      so_plat_write(ptr, &val, sizeof(val));
      so_log("weak import set to NULL: %s : %s", mod->name, name);
      continue;
    }

    /* unresolved: report each name once per module */
    int seen = 0;
    for (int j = 0; j < num_reported; j++) {
      if (strcmp(reported[j], name) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      if (num_reported < MAX_REPORTED)
        reported[num_reported++] = name;
      missing++;
      so_log("MISSING %s : %s", mod->name, name);
    }
  }

  return missing;
}

void so_initialize(so_module *mod) {
  /* Every call is logged first, so a crash shows which constructor died. */
  for (int i = 0; i < mod->num_init_array; i++) {
    uint32_t fn = mod->init_array[i];
    if (fn && fn != 0xffffffffu) {
      so_log("init %s [%d/%d] %08x (offset %06x)", mod->name, i + 1,
             mod->num_init_array, (unsigned)fn, (unsigned)(fn - mod->text_base));
      ((void (*)(void))(uintptr_t)fn)();
    }
  }
}

uintptr_t so_find_exidx(uintptr_t pc, int *count) {
  for (so_module *m = head; m; m = m->next) {
    if (pc >= m->text_base && pc < m->text_base + m->text_size) {
      if (count)
        *count = m->num_exidx;
      return m->exidx;
    }
  }
  return 0;
}
