MAINTAINERCLEANFILES = Makefile.in

noinst_HEADERS = daemon_init.h platform.h sock.h

bin_PROGRAMS = cpglockdump
sbin_PROGRAMS = cpglockd

AM_CPPFLAGS = -I$(top_builddir)/lib -I$(top_builddir)/bin

AM_CPPFLAGS += -Wextra -Wall -Wformat-nonliteral -Wformat=2 -Wshadow -Wno-unused-parameter -Winit-self -Wuninitialized -Wstrict-overflow=5 -Wcast-align -Wlogical-op -Wstrict-prototypes -Wmissing-format-attribute -Wredundant-decls -Wnested-externs

cpglockd_SOURCES = cpglockd.c daemon_init.c sock.c
cpglockdump_SOURCES = cpglockdump.c

cpglockd_LDADD = -lqb -lcpg $(top_builddir)/lib/libcpglock.la
cpglockdump_LDADD = -lqb -lcpg $(top_builddir)/lib/libcpglock.la
