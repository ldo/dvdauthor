AC_INIT(DVDAuthor,0.7.2,dvdauthor-users@lists.sourceforge.net)

AC_CONFIG_HEADERS(src/config.h)
AC_CONFIG_AUX_DIR(autotools)

AM_INIT_AUTOMAKE
LT_INIT

AC_PROG_INSTALL

AC_SYS_LARGEFILE

AC_HEADER_STDBOOL

AC_LANG([C])
AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([], [void nested(void) {}])],
    [echo C compiler allows nested routines; AC_DEFINE([HAVE_NESTED_ROUTINES], [1], [define to 1 if C compiler allows nested routines])],
    [echo WARNING: C compiler does not allow nested routines--some functionality will be disabled]
)

have_dvdread=false
AC_CHECK_LIB(dvdread, DVDOpenFile, have_dvdread=true)

AC_CHECK_LIB(gnugetopt, getopt_long)

dnl AC_CHECK_HEADERS initializes CPP, so must appear outside of any conditionals
AC_CHECK_HEADERS( \
    getopt.h \
    io.h \
)

AC_CHECK_FUNCS( \
    strndup \
    getopt_long \
    setmode \
)

PKG_CHECK_MODULES(LIBPNG, [libpng])
AC_SUBST(LIBPNG_CFLAGS)
AC_SUBST(LIBPNG_LIBS)

config_static=''
if test "$enable_shared" = 'no'; then
    config_static='--static'
fi

usemagick=0

AC_CHECK_PROGS(MAGICKCONFIG, [MagickCore-config])
if test -n "$MAGICKCONFIG"; then
    ac_save_CPPFLAGS="$CPPFLAGS"
    ac_save_LIBS="$LIBS"
    MAGICK_CPPFLAGS="`$MAGICKCONFIG --cppflags`"
    MAGICK_LIBS="`$MAGICKCONFIG --ldflags` `$MAGICKCONFIG --libs`"
    CPPFLAGS="$CPPFLAGS $MAGICK_CPPFLAGS"
    LIBS="$MAGICK_LIBS $LIBS"
    AC_CHECK_FUNC(ExportImagePixels, usemagick=1, AC_MSG_NOTICE([ImageMagick does not support the function
         ExportImagePixels.  Please upgrade to ImageMagick 5.5.7 or newer]))
    CPPFLAGS="$ac_save_CPPFLAGS"
    LIBS="$ac_save_LIBS"
    if test "$usemagick" = 1; then
        AC_DEFINE(HAVE_MAGICK, 1, [Whether the ImageMagick libraries are available])
    fi
fi

if test "$usemagick" != 1; then
    PKG_CHECK_MODULES([MAGICK], [GraphicsMagick], usemagick=1; [AC_DEFINE(HAVE_GMAGICK, 1, [whether the GraphicsMagick libraries are available])], [:])
fi

if test "$usemagick" != 1; then
    MAGICK_CPPFLAGS="$LIBPNG_CFLAGS"
    MAGICK_LIBS="$LIBPNG_LIBS"
fi

AC_SUBST(MAGICK_CPPFLAGS)
AC_SUBST(MAGICK_LIBS)

PKG_CHECK_MODULES([FONTCONFIG], [fontconfig], [AC_DEFINE(HAVE_FONTCONFIG, 1, [whether Fontconfig is available])], [:])
AC_SUBST(FONTCONFIG_CFLAGS)
AC_SUBST(FONTCONFIG_LIBS)

PKG_CHECK_MODULES([FRIBIDI], [fribidi], [AC_DEFINE(HAVE_FRIBIDI, 1, [whether FriBidi is available])], [:])
AC_SUBST(FRIBIDI_CFLAGS)
AC_SUBST(FRIBIDI_LIBS)

AC_CHECK_PROGS(FREETYPECONFIG, [freetype-config])
if test -n "$FREETYPECONFIG"; then
    FREETYPE_CPPFLAGS="`$FREETYPECONFIG --cflags`"
    FREETYPE_LIBS="`$FREETYPECONFIG --libs $config_static`"
    AC_DEFINE(HAVE_FREETYPE, 1, [Whether FreeType is available])

    ac_save_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS $FREETYPE_CPPFLAGS"
    AC_CHECK_HEADERS(ft2build.h)
    CPPFLAGS="$ac_save_CPPFLAGS"
    AC_SUBST(FREETYPE_CPPFLAGS)
    AC_SUBST(FREETYPE_LIBS)
else
    AC_MSG_ERROR([freetype not found])
fi


AC_ARG_ENABLE([default-video-format],
AS_HELP_STRING([--enable-default-video-format=format], [specify a default video format, either NTSC or PAL, to be used if no configuration or input setting is given]),
[
if test "$enable_default_video_format" == "PAL" -o "$enable_default_video_format" == "pal"; then
    AC_DEFINE(DEFAULT_VIDEO_FORMAT, 2, [Default video format, 1 => NTSC, 2 => PAL])
elif test "$enable_default_video_format" == "NTSC" -o "$enable_default_video_format" == "ntsc"; then
    AC_DEFINE(DEFAULT_VIDEO_FORMAT, 1, [Default video format, 1 => NTSC, 2 => PAL])
else
    AC_MSG_ERROR([specify either --enable-default-video-format=pal or --enable-default-video-format=ntsc])
fi
])

AC_ARG_ENABLE([localize-filenames],
AS_HELP_STRING([--enable-localize-filenames], [specifies that filenames are to be interpreted in the locale encoding. If omitted, they are always interpreted as UTF-8]),
[
AC_DEFINE(LOCALIZE_FILENAMES, 1, [Whether to localize filenames])
])

AC_ARG_ENABLE([dvdunauthor],
AS_HELP_STRING([--disable-dvdunauthor], [don't build dvdunauthor (which requires libdvdread). If not specified and libdvdread cannot be found, an error occurs])
)
AS_IF([test "x$enable_dvdunauthor" != xno],
[AS_IF([test "x$have_dvdread" = xfalse],
[AC_MSG_ERROR([missing libdvdread])]
)
],
[have_dvdread=false]
)
AM_CONDITIONAL(HAVE_DVDREAD, $have_dvdread)

dnl AM_LANGINFO_CODESET

AM_ICONV

ifdef([AM_PATH_XML2],
    [AM_PATH_XML2(2.6.0, , AC_MSG_ERROR([You must have libxml2 >= 2.6.0 installed]))],
    [errprint([You must have libxml2 (>= 2.6.0) installed
])m4_exit(1)AC_MSG_ERROR([You must have libxml2 (>= 2.6.0) installed])])

AC_CHECK_DECLS(O_BINARY, , , [ #include <fcntl.h> ] )

AC_OUTPUT(Makefile doc/Makefile src/Makefile)
