man1_MANS = dvdauthor.1 spumux.1
dist_pkgdata_DATA = dvdauthor.xsd spumux.xsd


EXTRA_DIST = $(man1_MANS)

$(man1_MANS):
	rm -f $(man1_MANS)
	docbook2man root.sgml

.PHONY: html

html:
	rm -f *.html
	docbook2html -d html.dsl root.sgml

MAINTAINERCLEANFILES = $(man1_MANS)
