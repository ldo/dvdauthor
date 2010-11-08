man1_MANS = dvdauthor.1 dvddirdel.1 dvdunauthor.1 mpeg2desc.1 spumux.1 spuunmux.1
dist_pkgdata_DATA = common.xsd dvdauthor.xsd spumux.xsd


EXTRA_DIST = $(man1_MANS)

.PHONY: manpages

manpages:
	rm -f $(man1_MANS)
	docbook2man root.sgml

$(man1_MANS):
	$(MAKE) manpages

.PHONY: html

html:
	rm -f *.html
	docbook2html -d html.dsl root.sgml

index.html:
	$(MAKE) html

MAINTAINERCLEANFILES = $(man1_MANS)
