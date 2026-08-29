startupdir = $(pkglibexecdir)/mpi/startup
startup_PROGRAMS = osu_init osu_hello

AM_CFLAGS = -I${top_srcdir}/util

if EMBEDDED_BUILD
    AM_LDFLAGS =
    LDADD =
    AM_CPPFLAGS = -I$(top_builddir)/../src/include \
		  -I${top_srcdir}/../src/include
if BUILD_PROFILING_LIB
    LDADD += $(top_builddir)/../lib/lib@PMPILIBNAME@.la
endif
    LDADD += $(top_builddir)/../lib/lib@MPILIBNAME@.la
endif
