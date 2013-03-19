noinst_PROGRAMS = lockstress locktest many many2 thread thread2 try

AM_CPPFLAGS = -I$(top_builddir)/lib

AM_CPPFLAGS += -Wextra -Wall -Wformat-nonliteral -Wformat=2 -Wshadow -Wno-unused-parameter -Winit-self -Wuninitialized -Wstrict-overflow=5 -Wcast-align -Wlogical-op -Wstrict-prototypes -Wmissing-format-attribute -Wredundant-decls -Wnested-externs

AM_LDFLAGS = -L$(top_builddir)/lib


AM_CFLAGS = $(LOCAL_INCLUDE_CFLAGS) $(WARN_CFLAGS)
LDADD = $(top_builddir)/lib/libcpglock.la

lockstress_SOURCES = lockstress.c
locktest_SOURCES = locktest.c
many_SOURCES = many.c
many2_SOURCES = many2.c
thread_SOURCES = thread.c
thread2_SOURCES = thread2.c
try_SOURCES = try.c
