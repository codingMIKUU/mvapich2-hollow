[#] start of __file__

AC_DEFUN([PAC_SUBCFG_PREREQ_]PAC_SUBCFG_AUTO_SUFFIX,[
])

AC_DEFUN([PAC_SUBCFG_BODY_]PAC_SUBCFG_AUTO_SUFFIX,[

# the pm_names variable is set by the top level configure
build_pm_mpirun=no
for pm_name in $pm_names ; do
    if test "X$pm_name" = "Xmpirun" ; then
        build_pm_mpirun=yes
    fi
done

AM_CONDITIONAL([BUILD_PM_MPIRUN],[test "x$build_pm_mpirun" = "xyes"])

AM_COND_IF([BUILD_PM_MPIRUN],[

AC_ARG_ENABLE([rsh],
              [AS_HELP_STRING([--enable-rsh],
                              [Enable use of rsh for command execution by default.])
              ],
              [],
              [enable_rsh=no])

AC_ARG_VAR([RSH_CMD], [path to rsh command])
AC_PATH_PROG([RSH_CMD], [rsh], [/usr/bin/rsh])

AC_ARG_VAR([SSH_CMD], [path to ssh command])
AC_PATH_PROG([SSH_CMD], [ssh], [/usr/bin/ssh])

AC_ARG_VAR([ENV_CMD], [path to env command])
AC_PATH_PROG([ENV_CMD], [env], [/usr/bin/env])

AC_ARG_VAR([DBG_CMD], [path to debugger command])
AC_PATH_PROG([DBG_CMD], [gdb], [/usr/bin/gdb])

AC_ARG_VAR([XTERM_CMD], [path to xterm command])
AC_PATH_PROG([XTERM_CMD], [xterm], [/usr/bin/xterm])

AC_ARG_VAR([SHELL_CMD], [path to shell command])
AC_PATH_PROG([SHELL_CMD], [bash], [/bin/bash])

AC_ARG_VAR([TOTALVIEW_CMD], [path to totalview command])
AC_PATH_PROG([TOTALVIEW_CMD], [totalview], [/usr/totalview/bin/totalview])

AC_ARG_VAR([SRUN_CMD], [path to srun command])
AC_PATH_PROG([SRUN_CMD], [srun], [/usr/bin/srun])

AC_PROG_YACC
AC_PROG_LEX

AC_SEARCH_LIBS(ceil, m,,[AC_MSG_ERROR([libm not found.])],)
AC_CHECK_FUNCS([strdup strndup get_current_dir_name])

if test -n "`echo $build_os | grep solaris`"; then
    AC_SEARCH_LIBS(herror, resolv,,[AC_MSG_ERROR([libresolv not found.])],)
    AC_SEARCH_LIBS(bind, socket,,[AC_MSG_ERROR([libsocket not found.])],)
    AC_SEARCH_LIBS(sendfile, sendfile,,[AC_MSG_ERROR([libsendfile not found.])],)
    mpirun_rsh_other_libs="-lresolv -lsocket"
    mpispawn_other_libs="-lresolv -lsocket -lnsl -lsendfile"
fi

if test "$enable_rsh" = "yes"; then
    AC_DEFINE(USE_RSH, 1, [Define to enable use of rsh for command execution by default.])
fi

AC_DEFINE(HAVE_PMI_IBARRIER, 1, [Define if pmi client supports PMI_Ibarrier])
AC_DEFINE(HAVE_PMI_WAIT, 1, [Define if pmi client supports PMI_Wait])
AC_DEFINE(HAVE_PMI2_KVS_IFENCE, 1, [Define if pmi client supports PMI2_KVS_Ifence])
AC_DEFINE(HAVE_PMI2_KVS_WAIT, 1, [Define if pmi client supports PMI2_KVS_Wait])
AC_DEFINE(HAVE_PMI2_SHMEM_IALLGATHER, 1, [Define if pmi client supports PMI2_Iallgather])
AC_DEFINE(HAVE_PMI2_SHMEM_IALLGATHER_WAIT, 1, [Define if pmi client supports PMI2_Iallgather_wait])
AC_DEFINE(HAVE_PMI2_SHMEM_IALLGATHER, 1, [Define if pmi client supports PMI2_SHMEM_Iallgather])
AC_DEFINE(HAVE_PMI2_SHMEM_IALLGATHER_WAIT, 1, [Define if pmi client supports PMI2_SHMEM_Iallgather_wait])

# MVAPICH2_VERSION is exported from the top level configure
AC_DEFINE_UNQUOTED([MVAPICH2_VERSION], ["$MVAPICH2_VERSION"], [Set to current version of mvapich2 package])

])

AM_CONDITIONAL([WANT_RDYNAMIC], [test "x$GCC" = xyes])
AM_CONDITIONAL([WANT_CKPT_RUNTIME], [test "x$enable_ckpt" = xyes])

dnl AC_MSG_NOTICE([RUNNING CONFIGURE FOR MPIRUN PROCESS MANAGERS])
# do nothing extra here for now

])dnl end _BODY

[#] end of __file__
