// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "writer.hpp"
#include "index.hpp"

#include <future>

namespace tapir
{

    WriterThread::WriterThread(Tape &tape, int block_factor,
                               std::mutex &state_mtx, Index &index)
        : tape_(tape), block_factor_(block_factor),
          state_mtx_(state_mtx), index_(index),
          thread_([this](std::stop_token st)
                  { worker_loop(st); })
    {
    }

    void WriterThread::push(std::unique_ptr<WriterTask> task)
    {
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            queue_.push_back(std::move(task));
        }
        queue_cv_.notify_one();
    }

    // Sleeps until a task arrives in the queue, then returns HasWork. Returns
    // Stopping only when the thread has been asked to stop AND the queue is empty —
    // meaning every pending write has already completed.
    //
    // Uses condition_variable_any::wait(lock, stop_token, pred) — a C++20 overload
    // that internally registers a stop callback on the stop_token so that
    // request_stop() (called by ~jthread) wakes this wait without any extra
    // notify_one() call from the destructor side.
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
        const bool has_work = queue_cv_.wait(lk, st, [this]
                                             { return !queue_.empty(); });
        return has_work ? WaitResult::HasWork : WaitResult::Stopping;
    }

    void WriterThread::worker_loop(std::stop_token st)
    {
        std::unique_lock<std::mutex> lk(queue_mtx_);
        while (wait_for_work(lk, st) == WaitResult::HasWork)
        {
            auto task = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            task->run(*this);
            lk.lock();
        }
    }

    void WriterThread::enqueue_file(Node *node, std::shared_ptr<Staged> data,
                                    std::string path, time_t mtime)
    {
        push(make_write_member_task(node, std::move(data), std::move(path), mtime));
    }

    void WriterThread::sync(std::unique_lock<std::mutex> &lk)
    {
        if (!index_.dirty())
            return;

        std::future<void> fut;
        push(make_sync_task(fut));

        lk.unlock(); // release state_mtx_ so the writer thread can acquire it and run the sync task
        fut.wait();  // wait until the writer thread finishes the sync task
        lk.lock();   // reacquire the lock the caller handed us
    }

} // namespace tapir
