/*
 * Copyright (c) 2007-2012 Message Systems, Inc. All rights reserved
 * For licensing information, see:
 * https://bitbucket.org/wez/gimli/src/tip/LICENSE
 */
#include "impl.h"

/* Implements GIMLI_ANA_API_VERSION <= 2 */

static const struct gimli_proc_stat *v2_get_proc_stat(void)
{
  return &the_proc->proc_stat;
}

static int v2_get_source_info(void *addr, char *buf,
  int buflen, int *lineno)
{
  uint64_t l;
  int ret = gimli_determine_source_line_number(the_proc,
      (gimli_addr_t)(intptr_t)addr, buf, buflen, &l);
  if (ret) {
    *lineno = (int)l;
  }
  return ret;
}

static char *v2_get_string_symbol(const char *obj, const char *name)
{
  return gimli_get_string_symbol(the_proc, obj, name);
}

static int v2_copy_from_symbol(const char *obj, const char *name,
  int deref, void *buf, uint32_t size)
{
  return gimli_copy_from_symbol(obj, name, deref, buf, size);
}

static struct gimli_symbol *v1_sym_lookup(
    const char *obj, const char *name)
{
  return gimli_sym_lookup(the_proc, obj, name);
}

static const char *v1_pc_sym_name(void *addr,
    char *buf, int buflen)
{
  return gimli_pc_sym_name(the_proc, (gimli_addr_t)(intptr_t)addr, buf, buflen);
}

static int v1_read_mem(void *src, void *dest, int len)
{
  return gimli_read_mem(the_proc, (gimli_addr_t)(intptr_t)src, dest, len);
}

static char *v1_read_string(void *src)
{
  return gimli_read_string(the_proc, (gimli_addr_t)(intptr_t)src);
}

static int v2_get_parameter(void *context, const char *varname,
  const char **datatypep, void **addrp, uint64_t *sizep)
{
  gimli_stack_frame_t frame = context;
  gimli_addr_t addr;
  gimli_type_t t;

  if (!gimli_stack_frame_resolve_var(frame, GIMLI_WANT_PARAMS,
        varname, &t, &addr)) {
    return 0;
  }

  *addrp = (void*)(intptr_t)addr;
  *sizep = gimli_type_size(t);
  *datatypep = gimli_type_declname(t);

  return 1;
}


struct gimli_ana_api ana_api = {
  GIMLI_ANA_API_VERSION,
  v1_sym_lookup,
  v1_pc_sym_name,
  v1_read_mem,
  v1_read_string,
  v2_get_source_info,
  v2_get_parameter,
  v2_get_string_symbol,
  v2_copy_from_symbol,
  v2_get_proc_stat,
};

/* vim:ts=2:sw=2:et:
 */
