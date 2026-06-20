// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// writer.hpp — background tape-write thread for tapir.
//
// All tape I/O (data files + manifest) is serialised on a single std::jthread
// so the FUSE dispatch thread never blocks on tape I/O. The public interface
// is intentionally minimal: callers enqueue a file write or request a sync;
// internal queue types and the thread loop are implementation details.

#pragma once

#include "raii.hpp"

#include <archive.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <ctime>

namespace tapir
{

// Forward declarations — callers include the full headers; writer.hpp stays lightweight.
class  Tape;
class  Index;
struct Node;
struct Staged;

class WriterThread
{
public:
    WriterThread(Tape &tape, int block_factor, std::mutex &state_mtx, Index &index);
    ~WriterThread() = default; // ~jthread() handles request_stop() + join()

    WriterThread(const WriterThread &) = delete;
    WriterThread &operator=(const WriterThread &) = delete;

    // Enqueue one file for async write to tape. `data` is a shared_ptr to the
    // temp-file Staged struct, kept alive for t_read while the write is in flight.
    void enqueue_file(Node *node, std::shared_ptr<Staged> data,
                      std::string path, time_t mtime);

    // Flush the index manifest to tape. Temporarily releases `lk` (which guards
    // the Index) while waiting for the writer thread, then reacquires before
    // returning. No-op if the index is not dirty.
    void sync(std::unique_lock<std::mutex> &lk);

private:
    Tape   &tape_;
    int     block_factor_;
    std::mutex &state_mtx_;
    Index  &index_;

    std::mutex               queue_mtx_;
    std::condition_variable_any queue_cv_;
    std::deque<std::function<void()>> queue_;
    std::jthread             thread_;

    // All files written between two sync() calls accumulate into one tape file.
    // The archive is opened on the first enqueue_file after a sync and kept open
    // until sync closes it. Field order matters for destruction: ar is destroyed
    // before fd so archive_write_free (end-of-archive blocks) runs before the fd
    // close that causes the st driver to write the filemark.
    struct OpenWrite {
        Fd              fd;                  // must outlive ar (filemark written on fd close)
        ArchiveWritePtr ar;                  // freed first → writes end-of-archive blocks
        int             tape_file   = -1;
    };
    std::optional<OpenWrite> open_write_;

    enum class WaitResult { HasWork, Stopping };

    void push(std::function<void()> task);
    void run(std::stop_token st);
    WaitResult wait_for_work(std::unique_lock<std::mutex> &lk, std::stop_token st);
};

} // namespace tapir
