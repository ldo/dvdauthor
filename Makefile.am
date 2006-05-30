SUBDIRS = doc src

noinst_DATA = dvdauthor.spec

EXTRA_DIST = dvdauthor.spec dvdauthor.spec.in

edit = sed \
	-e 's,@sysconfdir\@,$(sysconfdir),g' \
	-e 's,@PACKAGE_NAME\@,@PACKAGE_NAME@,g' \
	-e 's,@PACKAGE_VERSION\@,@PACKAGE_VERSION@,g' \
	-e 's,@PACKAGE_BUGREPORT\@,@PACKAGE_BUGREPORT@,g'

dvdauthor.spec: dvdauthor.spec.in src/config.h
	rm -f dvdauthor.spec dvdauthor.spec.tmp
	$(edit) dvdauthor.spec.in > dvdauthor.spec.tmp
	mv dvdauthor.spec.tmp dvdauthor.spec

