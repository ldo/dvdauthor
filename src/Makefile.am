
bin_PROGRAMS = dvdauthor dvdunauthor spumux spuunmux mpeg2desc
nodist_bin_SCRIPTS = dvddirdel

AM_CPPFLAGS   = -DSYSCONFDIR="\"$(sysconfdir)\"" @XML_CPPFLAGS@ @MAGICK_CPPFLAGS@ @FREETYPE_CPPFLAGS@ @FRIBIDI_CPPFLAGS@
AM_CFLAGS = -Wall

dvdauthor_SOURCES = dvdauthor.c dvdauthor.h da-internal.h \
	dvdcompile.c dvdvm.h dvdvml.c dvdvmy.c dvdvmy.h \
	dvdifo.c dvdvob.c dvdpgc.c \
	dvdcli.c readxml.c readxml.h \
	conffile.c conffile.h compat.c compat.h rgb.h
dvdauthor_LDADD = $(LIBICONV) $(XML_LIBS)

dvdunauthor_SOURCES = dvdunauthor.c compat.c compat.h
dvdunauthor_LDADD = $(XML_LIBS) -ldvdread

spumux_SOURCES = subgen.c subgen.h rgb.h \
	subgen-parse-xml.c readxml.c readxml.h \
	subgen-encode.c subgen-image.c \
	subconfig.h subglobals.h \
	textsub.c textsub.h subrender.c subrender.h subreader.c subreader.h subfont.c subfont.h
spumux_LDADD = $(XML_LIBS) $(MAGICK_LIBS) $(FREETYPE_LIBS) $(FRIBIDI_LIBS) $(LIBICONV)

spuunmux_SOURCES = spuunmux.c rgb.h
spuunmux_LDADD = -lpng -lz -lm

mpeg2desc_SOURCES = mpeg2desc.c

edit = sed \
	-e 's,@sysconfdir\@,$(sysconfdir),g' \
	-e 's,@PACKAGE_NAME\@,@PACKAGE_NAME@,g' \
	-e 's,@PACKAGE_VERSION\@,@PACKAGE_VERSION@,g' \
	-e 's,@PACKAGE_BUGREPORT\@,@PACKAGE_BUGREPORT@,g'

# by having it depend on config.h, it will get rebuilt if ./configure is run
dvddirdel: dvddirdel.in config.h
	rm -f dvddirdel dvddirdel.tmp
	$(edit) dvddirdel.in > dvddirdel.tmp
	chmod +x dvddirdel.tmp
	mv dvddirdel.tmp dvddirdel


EXTRA_DIST = dvdvml.l dvdvmy.y dvddirdel.in
MAINTAINERCLEANFILES = dvdvml.c dvdvmy.c dvdvmy.h

FLEX = flex
BISON = bison

dvdvml.c: dvdvml.l
	$(FLEX) -s -B -Cem -o$@ -Pdvdvm $<

dvdvmy.c dvdvmy.h: dvdvmy.y
	$(BISON) -o dvdvmy.c -d -p dvdvm $<

dvdvml.o: dvdvmy.h
