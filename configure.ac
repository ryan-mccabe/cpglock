AC_INIT(cpglock, 0.2, rmccabe@redhat.com)
AC_CONFIG_HEADER([config.h])
AC_CANONICAL_SYSTEM
AM_INIT_AUTOMAKE(cpglock, 0.2)

LT_PREREQ([2.2.6])
LT_INIT

AC_CONFIG_MACRO_DIR([m4])

AC_CANONICAL_HOST
AC_PROG_LIBTOOL
AC_LANG([C])

# Checks for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET

# Checks for libraries.
AC_CHECK_LIB([cpg], [cpg_finalize])
AC_CHECK_LIB([pthread], [pthread_create])
AC_CHECK_LIB([qb], [qb_loop_create])
AC_CHECK_LIB([rt], [clock_gettime])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_HEADER_TIME

AC_CHECK_HEADERS([arpa/inet.h fcntl.h malloc.h netdb.h netinet/in.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h syslog.h termios.h unistd.h corosync/cpg.h qb/qbloop.h qb/qbmap.h qb/qblog.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_PID_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_HEADER_TIME

# Checks for library functions.
AC_FUNC_CLOSEDIR_VOID
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_SELECT_ARGTYPES
AC_TYPE_SIGNAL
AC_FUNC_STAT

AC_CHECK_FUNCS([gettimeofday memmove memset select socket strcasecmp strchr strdup strerror])

AC_CONFIG_FILES([Makefile
	lib/Makefile
	lib/libcpglock.pc
	bin/Makefile
	test/Makefile
	doc/Makefile])

AC_OUTPUT
