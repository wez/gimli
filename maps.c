/*
 * Copyright (c) 2007-2012 Message Systems, Inc. All rights reserved
 * For licensing information, see:
 * https://bitbucket.org/wez/gimli/src/tip/LICENSE
 */
#include "impl.h"
#include "gimli_dwarf.h"

static int sort_compare_mapping(const void *A, const void *B)
{
  struct gimli_object_mapping *a = *(struct gimli_object_mapping**)A;
  struct gimli_object_mapping *b = *(struct gimli_object_mapping**)B;

  if (a->base < b->base) {
    return -1;
  }
  if (a->base > b->base) {
    return 1;
  }

  return a->len - b->len;
}

static int search_compare_mapping(const void *addrp, const void *L)
{
  gimli_addr_t addr = *(gimli_addr_t*)addrp;
  struct gimli_object_mapping *m = *(struct gimli_object_mapping**)L;

  if (addr < m->base) {
    return -1;
  }
  if (addr < m->base + m->len) {
    return 0;
  }
  return 1;
}

void gimli_show_memory_map(gimli_proc_t proc)
{
  int i;
  /* print out the maps, coalesce adjacent maps for the same object */
  printf("\nMEMORY MAP: (executable, shared objects and named mmaps)\n");
  i = 0;
  while (i < proc->nmaps) {
    int j;
    struct gimli_object_mapping *map = proc->mappings[i];
    uint64_t upper = map->base + map->len;

    for (j = i + 1; j < proc->nmaps; j++) {
      struct gimli_object_mapping *om = proc->mappings[j];

      if (om->objfile == map->objfile && om->base == upper) {
        upper = om->base + om->len;
        i = j;
        continue;
      }
      break;
    }

    printf(PTRFMT " - " PTRFMT " %s\n",
        map->base, upper, map->objfile->objname);
    i++;
  }
  printf("\n\n");
}

struct gimli_object_mapping *gimli_mapping_for_addr(gimli_proc_t proc, gimli_addr_t addr)
{
  struct gimli_object_mapping **mptr, *m;

  if (proc->maps_changed) {
    /* (re)sort the list of maps */
    qsort(proc->mappings, proc->nmaps, sizeof(struct gimli_object_mapping*),
        sort_compare_mapping);
    proc->maps_changed = 0;
  }

  mptr = bsearch(&addr, proc->mappings, proc->nmaps, sizeof(struct gimli_object_mapping*),
      search_compare_mapping);

  if (mptr) {
    m = *mptr;
    if (addr < m->base + m->len) {
      return m;
    }
  }
  return NULL;
}

const char *gimli_data_sym_name(gimli_proc_t proc, gimli_addr_t addr, char *buf, int buflen)
{
  struct gimli_object_mapping *m;
  struct gimli_symbol *s;

  m = gimli_mapping_for_addr(proc, (gimli_addr_t)addr);
  if (m) {
    s = find_symbol_for_addr(m->objfile, addr);
    if (s) {
      if (addr == s->addr) {
        snprintf(buf, buflen-1, "%s`%s", m->objfile->objname, s->name);
      } else {
        snprintf(buf, buflen-1, "%s`%s+%" PRIx64,
            m->objfile->objname, s->name, (uint64_t)(addr - s->addr));
      }
    } else {
      /* just identify the containing module; the caller will typically
       * annotate with the address */
      snprintf(buf, buflen-1, "%s", m->objfile->objname);
    }
    return buf;
  }
  return "";
}

const char *gimli_pc_sym_name(gimli_proc_t proc, gimli_addr_t addr, char *buf, int buflen)
{
  struct gimli_object_mapping *m;
  struct gimli_symbol *s;

  m = gimli_mapping_for_addr(proc, (gimli_addr_t)addr);
  if (m) {
    s = find_symbol_for_addr(m->objfile, addr);
    if (s) {
      if (addr == s->addr) {
        snprintf(buf, buflen-1, "%s`%s", m->objfile->objname, s->name);
      } else {
        snprintf(buf, buflen-1, "%s`%s+%" PRIx64,
            m->objfile->objname, s->name, (uint64_t)(addr - s->addr));
      }
    } else {
      snprintf(buf, buflen-1, "%s`" PTRFMT, m->objfile->objname, addr);
    }
    return buf;
  }
  return "";
}

struct gimli_object_mapping *gimli_add_mapping(
  gimli_proc_t proc,
  const char *objname, gimli_addr_t base, unsigned long len,
  unsigned long offset)
{
  struct gimli_object_mapping *m = calloc(1, sizeof(*m));

  m->proc = proc; // FIXME: refcnt
  m->base = base;
  m->len = len;
  m->offset = offset;
  if (debug) {
    fprintf(stderr, "MAP: " PTRFMT " - " PTRFMT " [off=0x%" PRIx64 "] %s\n",
        m->base, m->base + m->len, m->offset, objname);
  }
  m->objfile = gimli_find_object(proc, objname);
  if (!m->objfile) {
    m->objfile = gimli_add_object(proc, objname, base);
  }

  /* add to our collection */
  proc->mappings = realloc(proc->mappings, (proc->nmaps + 1) * sizeof(m));
  proc->mappings[proc->nmaps++] = m;
  proc->maps_changed = 1;

  return m;
}

gimli_mapped_object_t gimli_find_object(
  gimli_proc_t proc,
  const char *objname)
{
  gimli_mapped_object_t f;

  if (objname == NULL) {
    return proc->first_file;
  }

  if (gimli_hash_find(proc->files, objname, (void**)&f)) {
    return f;
  }

  return NULL;
}

void gimli_mapped_object_addref(gimli_mapped_object_t file)
{
  file->refcnt++;
}

static void destroy_cu(struct gimli_dwarf_cu *cu)
{
  if (cu->left) destroy_cu(cu->left);
  if (cu->right) destroy_cu(cu->right);
  free(cu);
}

void gimli_mapped_object_delete(gimli_mapped_object_t file)
{
  if (--file->refcnt) return;

  if (file->symhash) {
    gimli_hash_destroy(file->symhash);
  }
  if (file->symtab) {
    free(file->symtab);
  }
  if (file->sections) {
    gimli_hash_destroy(file->sections);
  }
  if (file->elf) {
    gimli_object_file_destroy(file->elf);
  }
  if (file->aux_elf) {
    gimli_object_file_destroy(file->aux_elf);
  }
  if (file->lines) {
    free(file->lines);
  }
  if (file->types) {
    gimli_type_collection_delete(file->types);
  }
  if (file->die_to_type) {
    gimli_hash_destroy(file->die_to_type);
  }
  if (file->abbr.map) {
    gimli_hash_destroy(file->abbr.map);
  }
  if (file->debug_info.cus) {
    destroy_cu(file->debug_info.cus);
  }
  free(file->arange);
  gimli_dw_fde_destroy(file);
  gimli_slab_destroy(&file->dieslab);
  gimli_slab_destroy(&file->attrslab);

  free(file->objname);
  free(file);
}

void gimli_destroy_mapped_object_hash(void *item)
{
  gimli_mapped_object_t file = item;

  gimli_mapped_object_delete(file);
}

static void destroy_section(void *ptr)
{
  struct gimli_section_data *data = ptr;

  free(data->name);
  free(data);
}

gimli_mapped_object_t gimli_add_object(
  gimli_proc_t proc,
  const char *objname, gimli_addr_t base)
{
  gimli_mapped_object_t f = gimli_find_object(proc, objname);
  struct gimli_symbol *sym;
  char *name = NULL;

  if (f) return f;

  f = calloc(1, sizeof(*f));
  f->refcnt = 1;
  f->objname = strdup(objname);
  f->sections = gimli_hash_new(destroy_section);
  gimli_slab_init(&f->dieslab, sizeof(struct gimli_dwarf_die), "die");
  gimli_slab_init(&f->attrslab, sizeof(struct gimli_dwarf_attr), "attr");

  gimli_hash_insert(proc->files, f->objname, f);

  if (proc->first_file == NULL) {
    proc->first_file = f;
  }

#ifndef __MACH__
  f->elf = gimli_elf_open(f->objname);
  if (f->elf) {
    f->elf->gobject = f;
    /* need to determine the base address offset for this object */
    f->base_addr = (intptr_t)base - f->elf->vaddr;
    if (debug) {
      printf("ELF: %s %d base=" PTRFMT " vaddr=" PTRFMT " base_addr=" PTRFMT "\n",
        f->objname, f->elf->e_type, base, f->elf->vaddr, f->base_addr);
    }

    gimli_process_elf(f);
  }
#endif

  return f;
}




/* vim:ts=2:sw=2:et:
 */

