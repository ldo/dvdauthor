AC_INIT(DVDAuthor,0.6.11,dvdauthor-users@lists.sourceforge.net)

AM_CONFIG_HEADER(src/config.h)
AC_CONFIG_AUX_DIR(autotools)

AM_INIT_AUTOMAKE

AC_PROG_INSTALL

AC_SYS_LARGEFILE

AC_CHECK_LIB(dvdread, DVDOpenFile, , AC_MSG_ERROR([You must have libdvdread(-devel) installed]))

AC_CHECK_LIB(gnugetopt, getopt_long)

dnl AC_CHECK_HEADERS initializes CPP, so must appear outside of any conditionals
AC_CHECK_HEADERS( \
	getopt.h \
	io.h \
)

AC_CHECK_FUNCS( \
	strsep \
	getopt_long \
	setmode \
)

AC_CHECK_PROGS(MAGICKCONFIG, [Magick-config GraphicsMagick-config])

usemagick=0
if test -n "$MAGICKCONFIG"; then
   ac_save_CPPFLAGS="$CPPFLAGS"
   ac_save_LIBS="$LIBS"
   MAGICK_CPPFLAGS="`$MAGICKCONFIG --cppflags`"
   MAGICK_LIBS="`$MAGICKCONFIG --ldflags` `$MAGICKCONFIG --libs`"
   CPPFLAGS="$CPPFLAGS $MAGICK_CPPFLAGS"
   LIBS="$MAGICK_LIBS $LIBS"
   AC_CHECK_FUNC(ExportImagePixels, usemagick=1, AC_MSG_NOTICE([ImageMagick/GraphicsMagick does not support the function
        ExportImagePixels.  Please upgrade to ImageMagick 5.5.7 or newer (or
        the corresponding GraphicsMagick version)]))
   CPPFLAGS="$ac_save_CPPFLAGS"
   LIBS="$ac_save_LIBS"
fi

if test "$usemagick" != 1; then
   AC_CHECK_LIB(z, zlibVersion, test, AC_MSG_ERROR([You must have libz(-devel) installed]))

   AC_CHECK_LIB(png, png_sig_cmp, test, AC_MSG_ERROR([You must have libpng(-devel) installed]), -lz -lm)
   MAGICK_CPPFLAGS=
   MAGICK_LIBS="-lpng -lz -lm"
else
   AC_DEFINE(HAVE_MAGICK, 1, [Whether the ImageMagick or GraphicsMagick libraries are available])
fi

AC_SUBST(MAGICK_CPPFLAGS)
AC_SUBST(MAGICK_LIBS)

AC_CHECK_PROGS(FRIBIDICONFIG, [fribidi-config])
if test -n "$FRIBIDICONFIG"; then
   FRIBIDI_CPPFLAGS="`$FRIBIDICONFIG --cflags`"
   FRIBIDI_LIBS="`$FRIBIDICONFIG --libs`"
   AC_DEFINE(HAVE_FRIBIDI, 1, [Whether FriBiDi is available])
fi

AC_SUBST(FRIBIDI_CPPFLAGS)
AC_SUBST(FRIBIDI_LIBS)

AC_CHECK_PROGS(FREETYPECONFIG, [freetype-config])
if test -n "$FREETYPECONFIG"; then
   FREETYPE_CPPFLAGS="`$FREETYPECONFIG --cflags`"
   FREETYPE_LIBS="`$FREETYPECONFIG --libs`"
   AC_DEFINE(HAVE_FREETYPE, 1, [Whether FreeType is available])

   ac_save_CPPFLAGS="$CPPFLAGS"
   CPPFLAGS="$CPPFLAGS $FREETYPE_CPPFLAGS"
   AC_CHECK_HEADERS(ft2build.h)
   CPPFLAGS="$ac_save_CPPFLAGS"
fi

AC_SUBST(FREETYPE_CPPFLAGS)
AC_SUBST(FREETYPE_LIBS)

AM_LANGINFO_CODESET

AM_ICONV

AM_PATH_XML2(2.6.0, , AC_MSG_ERROR([You must have libxml2 >= 2.6.0 installed]))

AC_CHECK_DECLS(O_BINARY, , , [ #include <fcntl.h> ] )

AC_OUTPUT(Makefile doc/Makefile src/Makefile)
