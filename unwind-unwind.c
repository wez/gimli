/* Copyright (c) 2013-present Facebook. All Rights Reserved
 * For licensing information, see:
 * https://bitbucket.org/wez/gimli/src/tip/LICENSE
 */

/* This uses libunwind to unwind stack frames.
 * It does this by driving the libunwind-ptrace implementation.  Since we want
 * to manage the actual ptrace() calls (we've already stopped the process, so
 * don't want unwind stopping/starting on top of our management of the target),
 * we wrap around libunwind rather than just calling the ptrace implementation
 * directly.
 */

#include "impl.h"

#ifdef HAVE_LIBUNWIND

#define FETCH_CUR struct gimli_unwind_cursor *cur = arg

static int find_proc_info(unw_addr_space_t as, unw_word_t ip,
  unw_proc_info_t *pip, int need_unwind_info, void *arg)
{
  FETCH_CUR;

  return _UPT_find_proc_info(as, ip, pip, need_unwind_info,
      cur->proc->unw_upt);
}

static void put_unwind_info(unw_addr_space_t as, unw_proc_info_t *pip,
    void *arg)
{
  FETCH_CUR;

  _UPT_put_unwind_info(as, pip, cur->proc->unw_upt);
}

static int get_dyn_info_list_addr(unw_addr_space_t as, unw_word_t *dilap,
    void *arg)
{
  FETCH_CUR;

  return _UPT_get_dyn_info_list_addr(as, dilap, cur->proc->unw_upt);
}

static int access_mem(unw_addr_space_t as, unw_word_t addr,
    unw_word_t *valp, int write, void *arg)
{
  if (write) {
    fprintf(stderr, "libunwind wants to write to memory at " PTRFMT "\n",
        addr);
    abort();
  }

  // We use `the_proc` directly here, because the unwind-ptrace implementation
  // calls into access_mem but sets arg to unw_upt and not our unwind
  // cursor pointer
  if (gimli_read_mem(the_proc, addr, valp, sizeof(*valp)) == sizeof(*valp)) {
    return 0;
  }
  return -UNW_EINVAL;
}

/* Access register number REG at address ADDR.  */
static int access_reg(unw_addr_space_t as, unw_regnum_t regnum,
    unw_word_t *valp, int write, void *arg)
{
  FETCH_CUR;

  void **regaddr = gimli_reg_addr(cur, regnum);
  if (write) {
    fprintf(stderr, "libunwind wants to store " PTRFMT " to reg %d\n",
        (PTRFMT_T)*valp, regnum);
    abort();
  }
  *valp = (intptr_t)*regaddr;
  return 0;
}

static int access_fpreg(unw_addr_space_t as, unw_regnum_t regnum,
    unw_fpreg_t *valp, int write , void *arg)
{
  FETCH_CUR;
  fprintf(stderr, "access_fpreg: regno=%d\n", regnum);
  return -UNW_EUNSPEC;
}

static int resume(unw_addr_space_t as, unw_cursor_t *cp, void *arg)
{
  FETCH_CUR;
  fprintf(stderr, "unw_resume called, but shouldn't have been\n");
  abort();
}

static unw_accessors_t accessors = {
  find_proc_info,
  put_unwind_info,
  get_dyn_info_list_addr,
  access_mem,
  access_reg,
  access_fpreg,
  resume,
  NULL, // get_proc_name: not used here
};

int gimli_unw_proc_init(gimli_proc_t proc)
{
  // These are both leaked.  We're short lived so this doesn't matter.
  proc->unw_addrspace = unw_create_addr_space(&accessors, 0);
  proc->unw_upt = _UPT_create(proc->pid);

  return 1;
}

int gimli_unw_unwind_init(struct gimli_unwind_cursor *cur)
{
  int err = unw_init_remote(&cur->unw_cursor, cur->proc->unw_addrspace, cur);

  if (err != 0) {
    fprintf(stderr, "unw_init_remote: %d %s\n",
        err, unw_strerror(err));
    return 0;
  }

  return 1;
}

int gimli_unw_unwind_next(struct gimli_unwind_cursor *cur)
{
  if (unw_step(&cur->unw_cursor)) {
    int i = 0;

    unw_get_reg(&cur->unw_cursor, UNW_REG_IP, &cur->st.pc);
    unw_get_reg(&cur->unw_cursor, UNW_REG_SP, &cur->st.fp);
    cur->st.sp = cur->st.fp;

    // Ask unwind to fill out information on all of the registers,
    // then update our cursor with them.  Failure to do this results
    // in being unable to read the correct location of variables
    // when we print out the full stack trace
    while (1) {
      // gimli_reg_addr returns NULL for invalid register offsets;
      // leverage that to avoid replicating machdep logic in here
      unw_word_t *valp = (unw_word_t*)gimli_reg_addr(cur, i);
      if (!valp) {
        break;
      }

      unw_get_reg(&cur->unw_cursor, i, valp);
      i++;
    }

    return 1;
  }
  return 0;
}

#endif /* HAVE_LIBUNWIND */

/* vim:ts=2:sw=2:et:
 */

