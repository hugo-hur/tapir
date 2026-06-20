// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_block_offset.cpp — DEVICE-FREE reproduction of the per-member block-offset
// read scenario (the bug behind test_tape_seek's gamma failure).
//
// A tar written with B-byte block padding is a byte-for-byte stand-in for the
// tape's block stream. The only tape-specific read operation, `MTFSR` to a block
// boundary, maps exactly to `lseek(fd, block*B)` on a regular file. So we can
// reproduce the exact failure with no hardware:
//
//   - A member whose tar header is NOT block-aligned has, in front of it within
//     its physical block, the tail of the PREVIOUS member's data.
//   - Starting libarchive at the block boundary (what Tape::seek_to does today)
//     feeds it that prior data -> it never finds a valid header -> extract fails.
//   - Starting at the header's byte offset (block start + within-block offset,
//     the recommended fix) extracts the member correctly.
//
// This test always runs (no TAPIR_TEST_DEVICE needed). It exercises the real
// tar I/O primitives (tar_write_file / tar_extract_member) and pins down the
// invariant the read-side fix must satisfy.

#include "test_tape_common.hpp"   // check(), gen(), make_src(), Fd, finish()

#include <archive_entry.h>

// Open a libarchive read on an already-positioned fd (caller has lseek'd it).
static struct archive *open_read_fd(int fd, int bsize)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_fd(a, fd, static_cast<size_t>(bsize)) != ARCHIVE_OK) {
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

int main()
{
    const int B = 4096; // simulated physical block size (8 × 512-byte tar records)

    // "big" is sized so the next member's header lands partway into a block, not
    // on a B boundary. "target" is the member we will try to read back.
    const std::vector<std::byte> big = gen(5000, 1);
    const std::vector<std::byte> tgt = gen(300, 2);
    std::printf("=== test_block_offset (no device, block size %d) ===\n", B);

    // 1) write a 2-member tar to a temp file, padded to B-byte blocks (as on tape)
    char tmpl[] = "/tmp/tapir-blk-XXXXXX";
    int wfd = mkstemp(tmpl);
    std::string path = tmpl;
    check(wfd >= 0, "create temp tar file");
    {
        Fd bigsrc = make_src(big), tgtsrc = make_src(tgt);
        struct archive *w = archive_write_new();
        archive_write_set_format_pax_restricted(w);
        archive_write_set_bytes_per_block(w, B);
        archive_write_set_bytes_in_last_block(w, B);
        bool wopen = archive_write_open_fd(w, wfd) == ARCHIVE_OK;
        std::vector<OutFile> outs = {
            {"big",    bigsrc.get(), big.size(), 0, 0},
            {"target", tgtsrc.get(), tgt.size(), 0, 0},
        };
        bool wrote = wopen;
        for (const auto &of : outs) {
            int64_t blk, boff;
            wrote = wrote && tar_write_file(w, of, B, blk, boff);
        }
        archive_write_close(w);
        archive_write_free(w);
        check(wrote, "wrote 2-member block-padded tar");
    }
    ::close(wfd);

    // 2) find target's header byte position P (what a scan/reindex would record)
    int64_t P = -1;
    {
        int rfd = ::open(path.c_str(), O_RDONLY);
        struct archive *a = open_read_fd(rfd, B);
        if (a) {
            struct archive_entry *e;
            while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
                if (std::string(archive_entry_pathname(e)) == "target") { P = archive_read_header_position(a); break; }
                archive_read_data_skip(a);
            }
            archive_read_free(a);
        }
        if (rfd >= 0) ::close(rfd);
    }
    check(P > 0, "located target's header byte position");

    const int64_t block_start = (P / B) * B;          // where MTFSR/lseek-to-block lands
    const int64_t within_block = P - block_start;     // the offset the fix must store
    std::printf("  target header @byte %lld -> block %lld (byte %lld), within-block offset %lld\n",
                (long long)P, (long long)(P / B), (long long)block_start, (long long)within_block);
    check(within_block != 0, "target header is NOT block-aligned (scenario is meaningful)");

    // 3) BUG path: start at the block boundary (Tape::seek_to today) -> must fail,
    //    because the block begins inside the previous member's data.
    {
        int rfd = ::open(path.c_str(), O_RDONLY);
        ::lseek(rfd, block_start, SEEK_SET);
        struct archive *a = open_read_fd(rfd, B);
        Fd out; uint64_t sz = 0;
        bool ok = a && tar_extract_member(a, "target", out, sz);
        if (a) archive_read_free(a);
        if (rfd >= 0) ::close(rfd);
        check(!ok, "extract from BLOCK BOUNDARY fails (header preceded by prior member's bytes)");
    }

    // 4) FIX path: position at the BLOCK BOUNDARY (exactly what the tape does after
    //    MTFSR — the raw block with the prior member's bytes padding the front),
    //    then use the offset-correcting helper. It reads the whole block and feeds
    //    libarchive from the header -> must succeed with the original content.
    {
        int rfd = ::open(path.c_str(), O_RDONLY);
        ::lseek(rfd, block_start, SEEK_SET);   // NOT the header — the block start
        ArchiveReadPtr a = tar_open_at_block_offset(rfd, B, within_block);
        Fd out; uint64_t sz = 0;
        bool ok = a && tar_extract_member(a.get(), "target", out, sz);
        std::vector<std::byte> got;
        if (ok && out.valid() && sz == tgt.size()) {
            got.resize(sz);
            ok = (::pread(out.get(), got.data(), sz, 0) == static_cast<ssize_t>(sz));
        } else ok = false;
        a.reset();                             // free archive (+callback state) before fd
        if (rfd >= 0) ::close(rfd);
        check(ok && got == tgt, "offset-correcting helper extracts from block boundary + offset");
    }

    ::unlink(path.c_str());
    return finish();
}
