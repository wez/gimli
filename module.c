/*
 * Copyright (c) 2007-2012 Message Systems, Inc. All rights reserved
 * Copyright (c) 2013-present Facebook, Inc.
 * For licensing information, see:
 * https://bitbucket.org/wez/gimli/src/tip/LICENSE
 */
#include "impl.h"

struct module_func_item {
  STAILQ_ENTRY(module_func_item) items;
  void (*func)();
  void *arg;
};

struct module_func_list {
  STAILQ_HEAD(funclist, module_func_item) list;
};

static STAILQ_HEAD(modulelist, module_item)
  modules = STAILQ_HEAD_INITIALIZER(modules);

static gimli_hash_t hooks = NULL;
static gimli_hash_t loaded_modules = NULL;

gimli_iter_status_t gimli_visit_modules(gimli_module_visit_f func, void *arg)
{
  struct module_item *mod;
  gimli_iter_status_t status = GIMLI_ITER_CONT;

  STAILQ_FOREACH(mod, &modules, modules) {
    status = func(mod, arg);
    if (status != GIMLI_ITER_CONT) {
      break;
    }
  }
  return status;
}

/* {{{ API v2 compat shims */

static void v2_tracer_shim(gimli_proc_t proc, void *arg)
{
  struct module_item *mod = arg;

  mod->ptr.v2->perform_trace(&ana_api, mod->exename);
}

static gimli_iter_status_t v2_printer_shim(gimli_proc_t proc,
    gimli_stack_frame_t frame,
    const char *varname, gimli_type_t t, gimli_addr_t addr,
    int depth, void *arg)
{
  struct module_item *mod = arg;
  const char *typename;
  uint64_t size;

  if (t) {
    size = gimli_type_size(t);
    typename = gimli_type_declname(t);
  } else {
    size = 0;
    typename = "<optimized out>";
  }

  if (mod->ptr.v2->before_print_frame_var(&ana_api,
        mod->exename,
        frame ? frame->cur.tid : 0,
        frame ? frame->cur.frameno : 0,
        frame ? (void*)(intptr_t)frame->cur.st.pc : 0,
        frame,
        typename,
        varname,
        (void*)(intptr_t)addr,
        size) == GIMLI_ANA_SUPPRESS) {
    return GIMLI_ITER_STOP;
  }

  return GIMLI_ITER_CONT;
}

/* }}} */

static int load_module(const char *exename, const char *filename)
{
  void *h;
  struct module_item *mod;
  gimli_module_init_func func;
  int (*modinit)(int);
  int found = 0;

  if (!loaded_modules) {
    loaded_modules = gimli_hash_new(NULL);
  }

  if (gimli_hash_find(loaded_modules, filename, &h)) {
    return 1;
  }
  gimli_hash_insert(loaded_modules, filename, (void*)filename);

  h = dlopen(filename, RTLD_NOW|RTLD_GLOBAL);
  if (!h) {
    printf("Unable to load library: %s: %s\n", filename, dlerror());
    return 0;
  }
  printf("Loaded tracer module %s for %s\n", filename, exename);

  modinit = (int (*)(int))dlsym(h, "gimli_module_init");
  if (modinit) {
    found++;
    modinit(GIMLI_ANA_API_VERSION);
  }

  func = (gimli_module_init_func)dlsym(h, "gimli_ana_init");
  if (func) {
    found++;

    mod = calloc(1, sizeof(*mod));
    mod->ptr.v2 = (*func)(&ana_api);

    if (!mod->ptr.v2) {
      free(mod);
    } else {
      mod->name = strdup(filename);
      mod->exename = strdup(exename);
      mod->api_version = mod->ptr.v2->api_version == 1 ? 1 : 2;
      STAILQ_INSERT_TAIL(&modules, mod, modules);

      if (mod->ptr.v2->perform_trace) {
        gimli_module_register_tracer(v2_tracer_shim, mod);
      }
      if (mod->ptr.v2->before_print_frame_var) {
        gimli_module_register_var_printer(v2_printer_shim, mod);
      }
    }
  }

  return found;
}

static int load_module_for_file_named(gimli_mapped_object_t file,
    const char *name, int explicit_ask)
{
  char buf[1024];
  char buf2[1024];
  void *h;
  int res = 0;
  char *dot;
  int namelen;

  if (debug) {
    if (explicit_ask) {
      printf("[ %s requests tracing via %s ]\n", file->objname, name);
    } else {
      printf("[ %s inferring tracer module, maybe %s ]\n", file->objname, name);
    }
  }

  strcpy(buf, file->objname);

  namelen = strlen(name);
  dot = strchr(name, '.');
  if (dot) {
    namelen = dot - name;
  }

  snprintf(buf2, sizeof(buf2)-1, "%s/%.*s%s", dirname(buf), namelen, name, MODULE_SUFFIX);

  if (debug) printf("[ %s: resolved module tracer name to %s ]\n", file->objname, buf2);

  if (access(buf2, F_OK) == 0) {
    res = load_module(file->objname, buf2);
    if (!res) {
      printf("Failed to load modules from %s\n", buf2);
    }
  } else if (explicit_ask) {
    printf("NOTE: module %s declared that its tracing "
        "should be performed by %s, but that module was not found (%s)\n",
        file->objname, buf2, strerror(errno));
  }
  return res;
}

static int load_modules_from_trace_section(gimli_object_file_t file,
    gimli_mapped_object_t via)
{
  struct gimli_section_data *s;
  gimli_hash_t names;
  char *name;
  char *end;
  int res = 1;

  if (!file) {
    // Can happen when target was deleted
    return 0;
  }

  s = gimli_get_section_by_name(file, GIMLI_TRACE_SECTION_NAME);
  if (!s) {
    return 1;
  }

  // This section holds a sequence of strings that we need
  // to read in, unique and load

  names = gimli_hash_new(NULL);
  name = (char*)s->data;
  end = name + s->size;
  while (name < end) {
    if (gimli_hash_insert(names, name, name)) {
      if (!load_module_for_file_named(via, name, 1)) {
        res = 0;
      }
    }
    name += strlen(name) + 1;
    // Deal with padding/alignment
    while (name < end && *name == 0) {
      name++;
    }
  }

  gimli_hash_destroy(names);
  return res;
}

static int load_module_for_file(gimli_mapped_object_t file)
{
  struct gimli_symbol *sym;
  char *name = NULL;
  char buf[1024];
  char buf2[1024];
  void *h;
  int res = 1;

  if (file->aux_elf) {
    if (!load_modules_from_trace_section(file->aux_elf, file)) {
      res = 0;
    }
  }
  if (!load_modules_from_trace_section(file->elf, file)) {
    res = 0;
  }

  sym = gimli_sym_lookup(the_proc, file->objname, "gimli_tracer_module_name");
  if (sym) {
    name = gimli_read_string(the_proc, sym->addr);
    if (!load_module_for_file_named(file, name, 1)) {
      res = 0;
    }
    free(name);
  }

  strcpy(buf, file->objname);
  snprintf(buf2, sizeof(buf2)-1, "gimli_%s", basename(buf));
  name = strdup(buf2);
  if (debug) printf("[ %s: computed %s for tracing ]\n", file->objname, name);

  if (!load_module_for_file_named(file, name, 0)) {
    res = 0;
  }

  free(name);

  return res;
}

/* perform discovery of tracer module */
static gimli_iter_status_t load_modules_for_file(const char *k, int klen,
    void *item, void *arg)
{
  gimli_mapped_object_t file = item;

  load_module_for_file(file);

  return GIMLI_ITER_CONT;
}


void gimli_load_modules(gimli_proc_t proc)
{
  gimli_hash_iter(the_proc->files, load_modules_for_file, NULL);
}

static void destroy_hooks(void *hptr)
{
  struct module_func_list *hook = hptr;
  struct module_func_item *item;

  while (STAILQ_FIRST(&hook->list)) {
    item = STAILQ_FIRST(&hook->list);
    STAILQ_REMOVE_HEAD(&hook->list, items);
    free(item);
  }
  free(hook);
}

gimli_iter_status_t gimli_hook_visit(const char *name,
    gimli_hook_visit_f func, void *arg)
{
  gimli_iter_status_t status = GIMLI_ITER_CONT;
  struct module_func_list *hook = NULL;
  struct module_func_item *item;

  if (!hooks) {
    return GIMLI_ITER_CONT;
  }

  if (!gimli_hash_find(hooks, name, (void**)&hook)) {
    return GIMLI_ITER_CONT;
  }

  STAILQ_FOREACH(item, &hook->list, items) {
    status = func(item->func, item->arg, arg);
    if (status != GIMLI_ITER_CONT) {
      break;
    }
  }

  return status;
}

int gimli_hook_register(const char *name, void (*func)(), void *arg)
{
  struct module_func_item *item;
  struct module_func_list *hook = NULL;

  if (!hooks || !gimli_hash_find(hooks, name, (void**)&hook)) {
    hook = calloc(1, sizeof(*hook));
    if (!hook) {
      return 0;
    }

    STAILQ_INIT(&hook->list);

    if (!hooks) {
      hooks = gimli_hash_new(destroy_hooks);
      if (!hooks) {
        free(hook);
        return 0;
      }
    }
    gimli_hash_insert(hooks, name, hook);
  }

  item = calloc(1, sizeof(*item));
  if (!item) {
    return 0;
  }

  item->func = func;
  item->arg = arg;

  STAILQ_INSERT_TAIL(&hook->list, item, items);

  return 1;
}

/* vim:ts=2:sw=2:et:
 */

