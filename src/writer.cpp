// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "writer.hpp"
#include "index.hpp"
#include "tape.hpp"
#include "tar_io.hpp"

#include <archive.h>

#include <cinttypes>
#include <cstdio>
#include <future>

namespace tapir
{

WriterThread::WriterThread(Tape &tape, int block_factor,
                           std::mutex &state_mtx, Index &index)
    : tape_(tape), block_factor_(block_factor),
      state_mtx_(state_mtx), index_(index),
      thread_([this](std::stop_token st) { run(st); })
{}

void WriterThread::push(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.push_back(std::move(task));
    }
    queue_cv_.notify_one();
}

// Sleeps until a tape-write task arrives in the queue, then returns true.
// Returns false only when the thread has been asked to stop and the queue is
// empty — meaning every pending write has already completed.
//
// Uses condition_variable_any::wait(lock, stop_token, pred) — a C++20 overload
// that internally registers a stop callback on the stop_token so that
// request_stop() (called by ~jthread) wakes this wait without any extra
// notify_one() call from the destructor side.
//
// Return value encodes why the wait ended:
//   true  — queue is non-empty; caller should dequeue and run the next task.
//   false — stop was requested AND the queue is empty; caller should exit.
//
// Drain guarantee: if stop fires while tasks remain, the predicate
// !queue_.empty() is still true, so wait() keeps returning true and the loop
// continues until every queued task has run. This ensures all in-flight file
// writes and the final manifest flush complete before the thread joins.
WriterThread::WaitResult
WriterThread::wait_for_work(std::unique_lock<std::mutex> &lk, std::stop_token st)
{
    // The predicate answers "is there something to write?". The thread blocks
    // as long as it returns false (queue empty). When push() enqueues a task
    // and calls notify_one(), the predicate is re-checked; if it now returns
    // true the thread wakes and wait() returns HasWork to the caller. On
    // spurious wake-ups (which condition variables may produce) the predicate
    // returning false simply puts the thread back to sleep.
    const bool has_work = queue_cv_.wait(lk, st, [this] { return !queue_.empty(); });
    return has_work ? WaitResult::HasWork : WaitResult::Stopping;
}

void WriterThread::run(std::stop_token st)
{
    std::unique_lock<std::mutex> lk(queue_mtx_);
    while (wait_for_work(lk, st) == WaitResult::HasWork)
    {
        auto task = std::move(queue_.front());
        queue_.pop_front();
        lk.unlock();
        task();
        lk.lock();
    }
}

void WriterThread::enqueue_file(Node *node, std::shared_ptr<Staged> data,
                                std::string path, time_t mtime)
{
    push([node, data = std::move(data), path = std::move(path), mtime, this]()
    {
        // Open a new tape file if none is currently in progress.
        if (!open_write_) {
            ArchiveWritePtr ar;
            Fd fd;
            int tape_file = -1;
            if (!tape_.open_write_at_eod(block_factor_, ar, fd, tape_file)) {
                std::lock_guard<std::mutex> gl(state_mtx_);
                std::fprintf(stderr, "tapir: writer: FAILED to open tape for writing '%s'\n",
                             path.c_str());
                node->staged.reset();
                return;
            }
            open_write_.emplace(OpenWrite{std::move(fd), std::move(ar), tape_file});
        }

        const int64_t bsize = static_cast<int64_t>(block_factor_) * 512;
        OutFile of{path, data->fd.get(), data->size, mtime, node->mode};
        int64_t blk = -1, off = -1;
        const bool ok = tar_write_file(open_write_->ar.get(), of, bsize, blk, off);

        std::lock_guard<std::mutex> gl(state_mtx_);
        if (ok) {
            node->data_tape_file = open_write_->tape_file;
            node->block_factor   = block_factor_;
            node->block_number   = blk;
            node->block_offset   = off;
            // Keep node->staged alive — it serves read-after-write until sync() closes
            // this tape file, the point at which the member becomes re-readable from
            // tape. Mark it flushed so sync() knows it is safe to release.
            node->staged_flushed = true;
            std::fprintf(stderr,
                         "tapir: writer: wrote '%s' -> tape file %d block %" PRId64 " offset %" PRId64 "\n",
                         path.c_str(), open_write_->tape_file, blk, off);
        } else {
            std::fprintf(stderr, "tapir: writer: FAILED to write '%s'\n", path.c_str());
            node->staged.reset(); // write failed: data is not on tape, drop the staged copy
        }
    });
}

void WriterThread::sync(std::unique_lock<std::mutex> &lk)
{
    if (!index_.dirty())
        return;

    // std::promise is move-only; wrap in shared_ptr so the capturing lambda
    // satisfies std::function's CopyConstructible requirement.
    auto sp  = std::make_shared<std::promise<void>>();
    auto fut = sp->get_future();

    push([sp, this]()
    {
        // push index write job to the queue
        // Close the open tape file before writing the manifest. All prior
        // enqueue_file() tasks have already run (queue is FIFO).
        // archive_write_close writes end-of-archive blocks; resetting open_write_
        // destroys ar (archive_write_free, idempotent) then fd (filemark written).
        if (open_write_) {
            const int tf = open_write_->tape_file;
            archive_write_close(open_write_->ar.get());
            open_write_.reset(); // ar freed (no-op), then fd closed → filemark
            tape_.note_write_done(tf);
            std::fprintf(stderr, "tapir: writer: closed tape file %d\n", tf);
        }

        std::string manifest;
        uint64_t snap_version = 0;
        {
            std::lock_guard<std::mutex> gl(state_mtx_);
            // The tape file is now closed and re-readable, so drop the staged temp
            // copies of the files it holds. This also lets serialize() group them
            // under their real data_tape_file instead of treating them as new staged
            // files. Files released *during* this sync are not yet flushed and keep
            // their staged copy for the next sync.
            index_.release_flushed_staged();
            manifest = index_.serialize(0, block_factor_);
            // Snapshot the index version that produced this manifest. serialize()
            // does not advance it, so any FUSE mutation landing during the unlocked
            // tape write below will push version() past this value.
            snap_version = index_.version();
        }

        int out_mtf = 0;
        const bool ok = tape_.write_manifest_at_eod(manifest, out_mtf);
        if (ok)
        {
            std::lock_guard<std::mutex> gl(state_mtx_);
            // Only mark clean if nothing mutated the index while the manifest was
            // written WITHOUT the lock. If a create/unlink/write raced this sync, the
            // change is not in the manifest just written; clearing dirty here would
            // drop it from tape and suppress the next sync. Leave it dirty instead.
            if (index_.version() == snap_version)
                index_.mark_clean();
            else
                std::fprintf(stderr,
                             "tapir: writer: index changed during manifest write "
                             "(v%llu -> v%llu) — staying dirty for next sync\n",
                             static_cast<unsigned long long>(snap_version),
                             static_cast<unsigned long long>(index_.version()));
            std::fprintf(stderr, "tapir: writer: manifest written at tape file %d\n", out_mtf);
        }
        else
        {
            std::fprintf(stderr, "tapir: writer: FAILED to write manifest to tape\n");
        }
        sp->set_value();
    });

}
        lk.unlock(); // release state_mtx_ so the writer thread can acquire it and start running the sync job
        fut.wait();  // wait until writer thread is ready processing the sync job
        lk.lock();   // reacquire lock

} // namespace tapir
