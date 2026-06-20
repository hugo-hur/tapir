ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src

EXTRA_DIST = .gitmodules

clean-local:
	rm -f $(PACKAGE)-*.tar.gz $(PACKAGE)-*.tar.bz2 $(PACKAGE)-*.tar.xz

dist-hook:
	@test -f $(srcdir)/deps/libarchive/configure.ac || \
	    { echo "ERROR: deps/libarchive submodule is not initialised; run: git submodule update --init deps/libarchive"; exit 1; }
	mkdir -p $(distdir)/deps/libarchive
	git -C $(srcdir)/deps/libarchive archive --format=tar HEAD | \
	    tar x --directory $(distdir)/deps/libarchive
	cd $(distdir)/deps/libarchive && sh build/autogen.sh 2>/dev/null
