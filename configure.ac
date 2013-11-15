# vim:ts=2:sw=2:et:
AC_INIT(gimli,1.1,,gimli)
AM_INIT_AUTOMAKE([foreign])
AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_AS
AC_PROG_LIBTOOL
AC_C_INLINE
AC_C_BIGENDIAN

CFLAGS=`echo $CFLAGS | sed -e 's/-O2//;'`
CFLAGS="-D_REENTRANT $CFLAGS"
if test "$GCC" == "yes" ; then
  CFLAGS="$CFLAGS -Wmissing-prototypes -Wformat"
fi
MODULE_SUFFIX=".so"
SHLIB_SUFFIX=".so"
DWARF_DEBUG_FLAGS="-gdwarf-2 -g3"

AC_ARG_WITH(codesign,[
  --with-codesign=certname     Mac OS/X: name of a code signing certificate
                               to use to authorize the glider as a debugger],[
  DARWIN_CODESIGN="codesign -s \"$withval\" -f --continue glider wdb"
],[
  DARWIN_CODESIGN="echo"
])
DARWIN_DSYMUTIL="echo"

case $host in
  *darwin*)
    LDFLAGS="-lpthread"
    CORONER_LDFLAGS="-framework Security -framework CoreFoundation -sectcreate __TEXT __info_plist glider.plist"
    MODULE_SUFFIX=".bundle"
    SHLIB_SUFFIX=".dylib"
    RLDPATH="-R"
    DARWIN_DSYMUTIL="dsymutil -o .libs/wedgie.dSYM .libs/wedgie ; dsymutil -o .libs/libgimli.0.dylib.dSYM .libs/libgimli.0.dylib"
    ;;
  *linux*)
    CORONER_LDFLAGS="-lthread_db -lpthread"
    RLDPATH="-Wl,-rpath="
    ;;
  *freebsd*)
    CORONER_LDFLAGS="-lthread_db -lpthread"
    RLDPATH="-Wl,-rpath="
    ;;
  *solaris*)
    RLDPATH="-R"
    case $CC in
      *gcc*)
        ;;
      *)
        DWARF_DEBUG_FLAGS="-xdebugformat=dwarf"
        ;;
    esac
    LDFLAGS="-lrt"
    CORONER_LDFLAGS="-lc_db -lrtld_db"
  ;;
esac
AC_DEFINE_UNQUOTED([MODULE_SUFFIX], "$MODULE_SUFFIX", [platform module suffix])

CFLAGS="$CFLAGS $DWARF_DEBUG_FLAGS -DGIMLI_GLIDER_PATH=\"\\\"\$(bindir)/glider\\\"\""
LDFLAGS="$LDFLAGS $DWARF_DEBUG_FLAGS"
AC_SUBST(CORONER_LDFLAGS)
AC_SUBST(DARWIN_DSYMUTIL)
AC_SUBST(DARWIN_CODESIGN)

AC_CHECK_LIB(dl, dlopen)
AC_CHECK_LIB(m, floor)
AC_CHECK_LIB(crypt, crypt)
AC_CHECK_FUNCS(clock_gettime)

AC_ARG_WITH(libunwind,
[  --with-libunwind    Use libunwind instead of the built-in dwarf unwinder],
[
  dnl Can't use CHECK_LIB because the header files do symbol munging
  AC_MSG_CHECKING([for libunwind])
  libs="$LIBS"
  LIBS="$LIBS -lunwind-ptrace -lunwind-generic -lunwind"
  AC_TRY_LINK([
  #include <libunwind.h>
  #include <libunwind-ptrace.h>
  ],[
  unw_accessors_t accessors;
  accessors.find_proc_info = _UPT_find_proc_info;
  unw_create_addr_space(0,0);
  ],[
    AC_DEFINE(HAVE_LIBUNWIND, 1, [Have libunwind])
    AC_MSG_RESULT([yes])
    CORONER_LDFLAGS="$CORONER_LDFLAGS -lunwind-ptrace -lunwind-generic"
  ],[
    AC_MSG_RESULT([no])
  ])
  LIBS="$libs"
])
AC_CHECK_SIZEOF(void*)

AC_ARG_ENABLE(wdb,
[  --enable-wdb     Enable lua based debugger shell],
[
  AC_DEFINE(LUA_USE_POSIX, 1, [Use POSIX functions])
  AC_DEFINE(LUA_USE_DLOPEN, 1, [Use dlopen for loading modules])
  AC_DEFINE_UNQUOTED(LUA_ROOT, "$prefix/", [Installation prefix])
  AC_DEFINE(LUA_VERMAJORMINOR, "5.1", [First two version number octets])
  AC_SUBST(PACKAGE_VERMAJORMINOR, 5.1)

  AC_ARG_WITH(readline,
  [  --with-readline   link wdb against libreadline],
  [
    LDFLAGS_SAVE="$LDFLAGS"
    LDFLAGS="-L$withval"
    AC_CHECK_LIB(readline, readline, [
      AC_SUBST(RL_LDFLAGS, "-lreadline")
      AC_SUBST(RL_CFLAGS, "-DLUA_USE_READLINE")
    ])
    LDFLAGS="$LDFLAGS_SAVE"
  ])

  AC_ARG_WITH(libedit,
  [  --with-libedit    link wdb against libedit],
  [
    LDFLAGS_SAVE="$LDFLAGS"
    LDFLAGS="-L$withval"
    AC_CHECK_LIB(edit, readline, [
      AC_SUBST(RL_LDFLAGS, "-ledit")
      AC_SUBST(RL_CFLAGS, "-DLUA_USE_READLINE")
    ])
    LDFLAGS="$LDFLAGS_SAVE"
  ])
  HAVE_WDB=1
],[
  HAVE_WDB=0
])
AM_CONDITIONAL([WDB], [test x$HAVE_WDB = x1])

AC_CONFIG_HEADERS(gimli_config.h)
AC_CONFIG_FILES(Makefile)
AC_OUTPUT

