// basic headers
#define _GNU_SOURCE /* really just for strndup */

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
#define BUILDSPEC_MAGICK " imagemagick"
#else
#ifdef HAVE_GMAGICK
#define BUILDSPEC_MAGICK " graphicsmagick"
#else
#define BUILDSPEC_MAGICK ""
#endif
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

unsigned int strtounsigned
  (
    const char * s,
    const char * what /* description of what I'm trying to convert, for error message */
  );
  /* parses s as an unsigned decimal integer, returning its value. Aborts the
    program on error. */

#ifndef HAVE_STRNDUP
char * strndup
  (
    const char * s,
    size_t n
  );
#endif

#if !HAVE_DECL_O_BINARY
#define O_BINARY 0
#endif

#if defined(HAVE_SETMODE) && HAVE_DECL_O_BINARY
#define win32_setmode setmode
#else
#define win32_setmode(x,y)
#endif

#define PACKAGE_HEADER(x) PACKAGE_NAME "::" x ", version " PACKAGE_VERSION ".\nBuild options:" BUILDSPEC "\nSend bug reports to <" PACKAGE_BUGREPORT ">\n\n"

#ifndef HAVE_FT2BUILD_H
#define FT_FREETYPE_H <freetype/freetype.h>
#define FT_GLYPH_H <freetype/ftglyph.h>
#endif

/* values for vfile.ftype */
#define VFTYPE_FILE 0 /* an actual file I opened */
#define VFTYPE_PIPE 1 /* an actual pipe I opened to/from a child process */
#define VFTYPE_REDIR 2 /* a redirection to/from another already-opened file */
struct vfile /* for keeping track of files opened by varied_open */
  {
    FILE * h; /* do your I/O to/from this */
    int ftype, mode; /* for use by varied_close */
  } /*vfile*/;
struct vfile varied_open
  (
    const char * fname,
    int mode, /* either O_RDONLY or O_WRONLY, nothing more */
    const char * what /* description of what I'm trying to open, for error message */
  );
  /* opens the file fname, which can be an ordinary file name or take one of the
    following special forms:
        "-" -- refers to standard input (if mode is O_RDONLY) or output (if O_WRONLY)
        "&n" -- (n integer) refers to the already-opened file handle n
        "cmd|" -- spawns cmd as a subprocess and reads from its standard output
        "|cmd" -- spawns cmd as a subprocess and writes to its standard input.

    Will abort the process on any errors.
  */
void varied_close(struct vfile vf);
  /* closes a file previously opened by varied_open. */
