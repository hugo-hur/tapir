# Shared static library (like LTFS's libltfs) used by all the executables.
noinst_LIBRARIES = libtapir.a
libtapir_a_SOURCES = index.cpp tar_io.cpp tape.cpp security.cpp \
                     index.hpp tar_io.hpp tape.hpp raii.hpp security.hpp
libtapir_a_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)

bin_PROGRAMS = tapir tfsck mktapir

# tapir: the FUSE mount (needs libfuse3)
tapir_SOURCES  = tapir.cpp writer.cpp writer.hpp
tapir_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(FUSE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
tapir_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(FUSE_LIBS) $(CRYPTO_LIBS)

# tfsck: the verifier (no FUSE dependency)
tfsck_SOURCES  = tfsck.cpp
tfsck_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
tfsck_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)

# mktapir: build/convert the index (import existing tar tape files)
mktapir_SOURCES  = mktapir.cpp
mktapir_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
mktapir_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)

# Unit tests (make check)
check_PROGRAMS   = test_security
TESTS            = test_security

test_security_SOURCES  = test_security.cpp
test_security_CPPFLAGS = $(CRYPTO_CFLAGS)
test_security_LDADD    = libtapir.a $(CRYPTO_LIBS)
