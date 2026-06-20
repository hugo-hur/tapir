# Shared static library (like LTFS's libltfs) used by all the executables.
noinst_LIBRARIES = libtapir.a
libtapir_a_SOURCES = index.cpp tar_io.cpp tape.cpp security.cpp cli.cpp \
                     index.hpp tar_io.hpp tape.hpp raii.hpp security.hpp cli.hpp
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
check_PROGRAMS   = test_security test_index test_block_offset test_seek_plan test_tape_seq test_tape_seek
TESTS            = test_security test_index test_block_offset test_seek_plan test_tape_seq test_tape_seek \
                   test_tape_mount.sh

# The shell mount test drives the binaries by path; tell it where they are so it
# works under VPATH builds too. `make check` already builds them via check-am's
# all-am prerequisite, so no extra dependency is needed here.
AM_TESTS_ENVIRONMENT = TAPIR_BIN_DIR='$(abs_builddir)'; export TAPIR_BIN_DIR;
EXTRA_DIST           = test_tape_mount.sh

test_security_SOURCES  = test_security.cpp
test_security_CPPFLAGS = $(CRYPTO_CFLAGS)
test_security_LDADD    = libtapir.a $(CRYPTO_LIBS)

test_index_SOURCES  = test_index.cpp
test_index_CPPFLAGS = $(JSON_CFLAGS)
test_index_LDADD    = libtapir.a $(CRYPTO_LIBS)

# Device-free reproduction of the per-member block-offset read scenario. Always
# runs (no hardware): simulates the tape block stream with a block-padded tar file.
test_block_offset_SOURCES  = test_block_offset.cpp test_tape_common.hpp
test_block_offset_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
test_block_offset_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)

# Pure unit test of the tape seek-planner (the import-positioning bug). No hardware.
test_seek_plan_SOURCES  = test_seek_plan.cpp
test_seek_plan_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(CRYPTO_CFLAGS)
test_seek_plan_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)

# Optional hardware tests (share test_tape_common.hpp). Skip (exit 77) unless
# TAPIR_TEST_DEVICE is set, e.g.:  TAPIR_TEST_DEVICE=/dev/nst0 make check
#   test_tape_seq  — sequential read-back (on-tape data integrity)
#   test_tape_seek — per-member random-access read (read_member / seek path)
test_tape_seq_SOURCES   = test_tape_seq.cpp test_tape_common.hpp
test_tape_seq_CPPFLAGS  = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
test_tape_seq_LDADD     = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)

test_tape_seek_SOURCES  = test_tape_seek.cpp test_tape_common.hpp
test_tape_seek_CPPFLAGS = $(LIBARCHIVE_CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS)
test_tape_seek_LDADD    = libtapir.a $(LIBARCHIVE_LIBS) $(CRYPTO_LIBS)
