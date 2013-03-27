MAINTAINERCLEANFILES = Makefile.in

instdir = $(includedir)

noinst_HEADERS = cpglock-internal.h cpglock_util.h
inst_HEADERS = cpglock.h

AM_CPPFLAGS = -I$(top_builddir)/lib

AM_CPPFLAGS += -Wextra -Wall -Wformat-nonliteral -Wformat=2 -Wshadow -Wno-unused-parameter -Winit-self -Wuninitialized -Wstrict-overflow=5 -Wcast-align -Wlogical-op -Wstrict-prototypes -Wmissing-format-attribute -Wredundant-decls -Wnested-externs

lib_LTLIBRARIES = libcpglock.la

libcpglock_la_LDFLAGS = -version-info 0:2:0

libcpglock_la_SOURCES = libcpglock.c cpglock_util.c

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libcpglock.pc
