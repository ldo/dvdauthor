// basic headers
#include <stdio.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_IO_H
#include <io.h>
#endif

// this doesn't really belong here, but it was easiest
#ifdef HAVE_MAGICK
#define BUILDSPEC_MAGICK " magick"
#else
#define BUILDSPEC_MAGICK ""
#endif

#ifdef HAVE_GETOPT_LONG
#define BUILDSPEC_GETOPT " gnugetopt"
#else
#define BUILDSPEC_GETOPT ""
#endif

#ifdef HAVE_ICONV
#define BUILDSPEC_ICONV " iconv"
#else
#define BUILDSPEC_ICONV ""
#endif

#ifdef HAVE_FREETYPE
#define BUILDSPEC_FREETYPE " freetype"
#else
#define BUILDSPEC_FREETYPE ""
#endif

#ifdef HAVE_FRIBIDI
#define BUILDSPEC_FRIBIDI " fribidi"
#else
#define BUILDSPEC_FRIBIDI ""
#endif

#define BUILDSPEC BUILDSPEC_GETOPT BUILDSPEC_MAGICK BUILDSPEC_ICONV BUILDSPEC_FREETYPE BUILDSPEC_FRIBIDI



#ifndef HAVE_STRSEP
chat *strsep(char **stringp,const char *delim);
#endif

#if !HAVE_DECL_O_BINARY
#define O_BINARY 0
#endif

#if defined(HAVE_SETMODE) && HAVE_DECL_O_BINARY
#define win32_setmode setmode
#else
#define win32_setmode(x,y)
#endif

#define PACKAGE_HEADER(x) PACKAGE_NAME "::" x ", version " PACKAGE_VERSION ".\nBuild options:" BUILDSPEC "\nSend bugs to <" PACKAGE_BUGREPORT ">\n\n"

#ifdef ICONV_CONV
#define ICONV_CAST (const char **)
#else
#define ICONV_CAST (char **)
#endif

#ifndef HAVE_FT2BUILD_H
#define FT_FREETYPE_H <freetype/freetype.h>
#define FT_GLYPH_H <freetype/ftglyph.h>
#endif
