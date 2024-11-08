/*
 * Copyright (c) 2007-2012 Message Systems, Inc. All rights reserved
 * For licensing information, see:
 * https://bitbucket.org/wez/gimli/src/tip/LICENSE
 */
#include "impl.h"

struct gimli_symbol *gimli_add_symbol(gimli_mapped_object_t f,
  const char *name, gimli_addr_t addr, uint32_t size)
{
  struct gimli_symbol *s;
  char buf[1024];

  if (f->symcount + 1 >= f->symallocd) {
    f->symallocd = f->symallocd ? f->symallocd * 2 : 1024;
    f->symtab = realloc(f->symtab, f->symallocd * sizeof(*s));
  }

  f->symchanged = 1;
  s = &f->symtab[f->symcount++];
  memset(s, 0, sizeof(*s));

  s->rawname = name;//strdup(name);
  s->name = s->rawname;

  if (gimli_demangle(s->rawname, buf, sizeof(buf))) {
    s->name = strdup(buf);
  }

  s->addr = addr;
  s->size = size;

  if (debug && 0) {
    printf("add symbol: %s`%s = " PTRFMT " (%d) %s\n",
      f->objname, s->name, s->addr, s->size,
      s->rawname != s->name ? s->rawname : "");
  }

  return s;
}

static int sort_syms_by_addr_asc(const void *A, const void *B)
{
  struct gimli_symbol *a = (struct gimli_symbol*)A;
  struct gimli_symbol *b = (struct gimli_symbol*)B;

  if (a->addr < b->addr) {
    return -1;
  }
  if (a->addr > b->addr) {
    return 1;
  }
#ifndef __MACH__
  /* no size information available on darwin */
  if (a->size && b->size) {
    return a->size - b->size;
  }
#endif
  /* use their relative ordering as a last resort */
  return a - b;
}

static void bake_symtab(gimli_mapped_object_t f)
{
  int i, j;
  struct gimli_symbol *s;

  if (!f->symchanged) return;
  f->symchanged = 0;

  if (debug) {
    printf("baking %" PRId64 " symbols for %s base=" PTRFMT "\n",
        f->symcount, f->objname, f->base_addr);
  }

  if (f->symhash) {
    gimli_hash_delete_all(f->symhash, 0);
  } else {
    f->symhash = gimli_hash_new_size(NULL, 0, f->symcount);
  }

  /* sort for bsearch */
  qsort(f->symtab, f->symcount, sizeof(struct gimli_symbol),
    sort_syms_by_addr_asc);
//printf("sorting %d symbols in %s\n", f->symcount, f->objname);

  for (i = 0; i < f->symcount; i++) {
    s = &f->symtab[i];

#ifdef __MACH__
    /* the nlist symbols on this system have no size information;
     * synthesize it now by looking at the next highest item. */
    s->size = 8; /* start with something lame */
    for (j = i + 1; j < f->symcount; j++) {
      if (f->symtab[j].addr > s->addr) {
        s->size = f->symtab[j].addr - s->addr;
        break;
      }
    }
#endif

    /* this may fail due to duplicate names */
    gimli_hash_insert(f->symhash, s->rawname, s);
  }
}

/* lower is better.
 * We weight underscores at the start heavier than
 * those later on.
 */
static int calc_readability(const char *name)
{
  int start = 1;
  int value = 0;
  while (*name) {
    if (*name == '_') {
      if (start) {
        value += 2;
      } else {
        value++;
      }
    } else {
      start = 0;
    }
    name++;
  }
  return value;
}

static int search_compare_symaddr(const void *addrp, const void *S)
{
  gimli_addr_t addr = *(gimli_addr_t*)addrp;
  struct gimli_symbol *s = (struct gimli_symbol*)S;

  if (addr < s->addr) {
    return -1;
  }
  if (addr < s->addr + s->size) {
    return 0;
  }
  return 1;
}

struct gimli_symbol *find_symbol_for_addr(gimli_mapped_object_t f,
  gimli_addr_t addr)
{
  struct gimli_symbol *csym, *best, *last;
  int i, n, bu, cu;

  bake_symtab(f);
  if (!f->symcount) return NULL;

  csym = bsearch(&addr, f->symtab, f->symcount,
      sizeof(struct gimli_symbol), search_compare_symaddr);

  if (!csym) {
    return NULL;
  }

  /* we have a candidate symbol. However, it may be an arbitrary
   * symbol in a run of multiple candidates; we need to find the
   * best candidate */
  i = csym - f->symtab;

  /* look backwards until we find the first symbol in the range */
  n = i;
  while (n > 0) {
    if (f->symtab[n-1].addr < addr) {
      csym = &f->symtab[n];
      break;
    }
    n--;
  }

  /* look forwards for the last in the range */
  last = csym;
  n = i;
  while (n < f->symcount - 1) {
    if (f->symtab[n+1].addr > addr) {
      last = &f->symtab[n];
      break;
    }
    n++;
  }

  /* best available */
  best = csym;
  bu = calc_readability(best->name);
  csym++;

  while (csym <= last) {
    cu = calc_readability(csym->name);
    if (cu < bu) {
      /* this one is better */
      best = csym;
      bu = cu;
    }
    csym++;
  }

  return best;
}


static struct gimli_symbol *sym_lookup(gimli_mapped_object_t file,
    const char *name)
{
  struct gimli_symbol *sym = NULL;

  bake_symtab(file);
  if (!file->symcount) return NULL;

  if (gimli_hash_find(file->symhash, name, (void**)&sym)) {
    return sym;
  }
  return NULL;
}

struct find_sym {
  const char *name;
  gimli_mapped_object_t file;
  struct gimli_symbol *sym;
};

static gimli_iter_status_t search_for_sym(const char *k, int klen,
    void *item, void *arg)
{
  gimli_mapped_object_t file = item;
  struct find_sym *find = arg;

  find->sym = sym_lookup(file, find->name);
  if (find->sym) return GIMLI_ITER_STOP;

  return GIMLI_ITER_CONT;
}

static gimli_iter_status_t search_for_basename(const char *k, int klen,
    void *item, void *arg)
{
  gimli_mapped_object_t file = item;
  struct find_sym *find = arg;
  char buf[1024];

  strcpy(buf, file->objname);
  if (!strcmp(basename(buf), find->name)) {
    find->file = file;
    return GIMLI_ITER_STOP;
  }

  return GIMLI_ITER_CONT;
}

/* This function always returns a buffer that needs to
 * be released via free(3).  We use the native feature
 * of the system libc if we know it is present, otherwise
 * we need to malloc a buffer for ourselves.  This
 * is made more fun because some systems have a dynamic
 * buffer size obtained via sysconf().
 */
char *gimli_realpath(const char *filename)
{
#if defined(__GLIBC__) || defined(__APPLE__)
  return realpath(filename, NULL);
#else
  char *buf = NULL;
  char *retbuf;
  int path_max = 0;

#ifdef _SC_PATH_MAX
  path_max = sysconf(path, _SC_PATH_MAX);
#endif
  if (path_max <= 0) {
    path_max = PATH_MAX;
  }
  buf = malloc(path_max);
  if (!buf) {
    return NULL;
  }

  retbuf = realpath(filename, buf);

  if (retbuf != buf) {
    free(buf);
    return NULL;
  }

  return retbuf;
#endif
}

static gimli_iter_status_t search_for_symlink(const char *k, int klen,
    void *item, void *arg)
{
  gimli_mapped_object_t file = item;
  struct find_sym *find = arg;
  char dir[1024];
  char buf[1024];
  char *real;
  int len;
  gimli_iter_status_t status = GIMLI_ITER_CONT;

  strcpy(dir, file->objname);
  snprintf(buf, sizeof(buf)-1, "%s/%s", dirname(dir), find->name);
  real = gimli_realpath(buf);
  if (!real) {
    return GIMLI_ITER_CONT;
  }

  if (!strcmp(real, file->objname)) {
    status = GIMLI_ITER_STOP;
    find->file = file;
  }

  free(real);

  return status;
}

struct gimli_symbol *gimli_sym_lookup(gimli_proc_t proc, const char *obj, const char *name)
{
  gimli_mapped_object_t f;
  struct gimli_symbol *sym = NULL;
  struct find_sym find;

  find.name = name;

  /* if obj is NULL, we're looking for it anywhere we can find it */
  if (obj == NULL) {
    gimli_hash_iter(proc->files, search_for_sym, &find);
    if (debug) {
      printf("sym_lookup: %s => " PTRFMT "\n", name, find.sym ? find.sym->addr : 0);
    }
    return find.sym;
  }

  f = gimli_find_object(proc, obj);
  if (!f) {
    /* we may have just been given the basename of the object, in which
     * case, we need to run through the list and match on basenames */
    find.file = NULL;
    find.name = obj;
    gimli_hash_iter(proc->files, search_for_basename, &find);
    if (!find.file) {
      /* so maybe we were given the basename it refers to a symlink
       * that we need to resolve... */
      gimli_hash_iter(proc->files, search_for_symlink, &find);
    }

    if (!find.file) {
      return NULL;
    }
    /* save the mapping */
    gimli_hash_insert(proc->files, obj, find.file);
    gimli_mapped_object_addref(find.file);
    f = find.file;
  }

  sym = sym_lookup(f, name);
  if (debug) {
    printf("sym_lookup: %s`%s => " PTRFMT "\n", obj, name, sym ? sym->addr : 0);
  }

  return sym;
}


/* vim:ts=2:sw=2:et:
 */
