// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "writer.hpp"
#include "index.hpp"
#include "tape.hpp"
#include "tar_io.hpp"

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
        // Tape I/O runs here, outside any lock.
        int out_dtf = 0;
        OutFile of{path, data->fd.get(), data->size, mtime};
        const bool ok = tape_.write_data_at_eod(
            block_factor_,
            [&of](struct archive *a) { return tar_write_files(a, {of}); },
            out_dtf);

        // Brief lock to update node state. t_read serves from n->staged until
        // this point; after staged.reset() it falls through to the tape cache.
        std::lock_guard<std::mutex> gl(state_mtx_);
        if (ok)
        {
            node->data_tape_file = out_dtf;
            node->block_factor   = block_factor_;
            std::fprintf(stderr, "tapir: writer: wrote '%s' -> tape file %d\n",
                         path.c_str(), out_dtf);
        }
        else
        {
            std::fprintf(stderr,
                         "tapir: writer: FAILED to write '%s' -- data lost on tape error\n",
                         path.c_str());
        }
        node->staged.reset(); // drop temp fd; subsequent reads go to tape
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
        // Serialize while holding state_mtx_. All prior enqueue_file() tasks
        // have already run (queue is FIFO), so every node has data_tape_file
        // set and staged == nullptr.
        std::string manifest;
        {
            std::lock_guard<std::mutex> gl(state_mtx_);
            manifest = index_.serialize(0, block_factor_);
        }

        int out_mtf = 0;
        const bool ok = tape_.write_manifest_at_eod(manifest, out_mtf);
        if (ok)
        {
            std::lock_guard<std::mutex> gl(state_mtx_);
            index_.mark_clean();
            std::fprintf(stderr, "tapir: writer: manifest written at tape file %d\n", out_mtf);
        }
        else
        {
            std::fprintf(stderr, "tapir: writer: FAILED to write manifest to tape\n");
        }
        sp->set_value(); // wake WriterThread::sync() whether or not write succeeded
    });

    lk.unlock(); // release state_mtx_ so the writer thread can acquire it
    fut.wait();
    lk.lock();
}

} // namespace tapir
