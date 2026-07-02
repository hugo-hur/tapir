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

    // ── WriterTask broker ───────────────────────────────────────────────────────
    // These are the only functions that touch WriterThread's private state; every
    // task subclass reaches the writer through them (see writer.hpp). WriterThread
    // is complete here (writer.hpp is included), so the accessors compile.
    Tape       &WriterTask::tape(WriterThread &w)         { return w.tape_; }
    Index      &WriterTask::index(WriterThread &w)        { return w.index_; }
    std::mutex &WriterTask::state_mtx(WriterThread &w)    { return w.state_mtx_; }
    int         WriterTask::block_factor(WriterThread &w) { return w.block_factor_; }
    auto       &WriterTask::open_write(WriterThread &w)   { return w.open_write_; }

    // Open a fresh data tape file at EOD if none is currently in progress. The one
    // step that must construct the private OpenWrite, so it lives in the friended
    // base rather than in a subclass.
    bool WriterTask::ensure_open(WriterThread &w)
    {
        if (w.open_write_)
            return true;
        ArchiveWritePtr ar;
        Fd fd;
        int tape_file = -1;
        if (!w.tape_.open_write_at_eod(w.block_factor_, ar, fd, tape_file))
            return false;
        w.open_write_.emplace(WriterThread::OpenWrite{std::move(fd), std::move(ar), tape_file});
        return true;
    }

    // ── concrete tasks (implementation detail; internal linkage) ────────────────
    namespace
    {
        // Append one staged file as a member to the current data tape file.
        class WriteMemberTask : public WriterTask
        {
        public:
            WriteMemberTask(Node *node, std::shared_ptr<Staged> data,
                            std::string path, time_t mtime)
                : node_(node), data_(std::move(data)),
                  path_(std::move(path)), mtime_(mtime) {}
            void run(WriterThread &w) override;

        private:
            Node                   *node_;
            std::shared_ptr<Staged> data_;
            std::string             path_;
            time_t                  mtime_;
        };

        // Close the open data tape file, write the manifest, and signal completion
        // through the future handed back by get_future().
        class SyncTask : public WriterTask
        {
        public:
            std::future<void> get_future() { return done_.get_future(); }
            void run(WriterThread &w) override;

        private:
            std::promise<void> done_;
        };

        void WriteMemberTask::run(WriterThread &w)
        {
            // Open a new tape file if none is currently in progress.
            if (!ensure_open(w)) {
                std::lock_guard<std::mutex> gl(state_mtx(w));
                std::fprintf(stderr, "tapir: writer: FAILED to open tape for writing '%s'\n",
                             path_.c_str());
                node_->staged.reset();
                return;
            }
            auto &ow = open_write(w);

            const int64_t bsize = static_cast<int64_t>(block_factor(w)) * 512;
            OutFile of{path_, data_->fd.get(), data_->size, mtime_, node_->mode};
            int64_t blk = -1, off = -1;
            const bool ok = tar_write_file(ow->ar.get(), of, bsize, blk, off);

            std::lock_guard<std::mutex> gl(state_mtx(w));
            if (ok) {
                node_->data_tape_file = ow->tape_file;
                node_->block_factor   = block_factor(w);
                node_->block_number   = blk;
                node_->block_offset   = off;
                // Keep node->staged alive — it serves read-after-write until sync()
                // closes this tape file, the point at which the member becomes
                // re-readable from tape. Mark it flushed so sync() may release it.
                node_->staged_flushed = true;
                std::fprintf(stderr,
                             "tapir: writer: wrote '%s' -> tape file %d block %" PRId64 " offset %" PRId64 "\n",
                             path_.c_str(), ow->tape_file, blk, off);
            } else {
                std::fprintf(stderr, "tapir: writer: FAILED to write '%s'\n", path_.c_str());
                node_->staged.reset(); // write failed: data is not on tape, drop the staged copy
            }
        }

        void SyncTask::run(WriterThread &w)
        {
            // Close the open tape file before writing the manifest. All prior
            // WriteMemberTask runs have already completed (queue is FIFO).
            // archive_write_close writes end-of-archive blocks; resetting open_write_
            // destroys ar (archive_write_free, idempotent) then fd (filemark written).
            if (auto &ow = open_write(w)) {
                const int tf = ow->tape_file;
                archive_write_close(ow->ar.get());
                ow.reset(); // ar freed (no-op), then fd closed → filemark
                tape(w).note_write_done(tf);
                std::fprintf(stderr, "tapir: writer: closed tape file %d\n", tf);
            }

            // write_manifest_at_eod finds the tape file the manifest will occupy and
            // hands it to this callback, so serialize() can record the manifest's own
            // location in the generation directory. The callback runs on this (writer)
            // thread before the tape write, holding state_mtx only for the serialise.
            uint64_t snap_version = 0;
            int out_mtf = 0;
            const bool ok = tape(w).write_manifest_at_eod(
                [&](int manifest_pos) {
                    std::lock_guard<std::mutex> gl(state_mtx(w));
                    // The tape file is now closed and re-readable, so drop the staged
                    // temp copies of the files it holds. This also lets serialize()
                    // group them under their real data_tape_file instead of treating
                    // them as new staged files. Files released *during* this sync are
                    // not yet flushed and keep their staged copy for the next sync.
                    index(w).release_flushed_staged();
                    std::string m = index(w).serialize(0, block_factor(w), manifest_pos);
                    // Snapshot the index version that produced this manifest. serialize()
                    // does not advance it, so any FUSE mutation landing during the
                    // (unlocked) tape write below will push version() past this value.
                    snap_version = index(w).version();
                    return m;
                },
                out_mtf);
            if (ok)
            {
                std::lock_guard<std::mutex> gl(state_mtx(w));
                // Only mark clean if nothing mutated the index while the manifest was
                // written WITHOUT the lock. If a create/unlink/write raced this sync, the
                // change is not in the manifest just written; clearing dirty here would
                // drop it from tape and suppress the next sync. Leave it dirty instead.
                if (index(w).version() == snap_version)
                    index(w).mark_clean();
                else
                    std::fprintf(stderr,
                                 "tapir: writer: index changed during manifest write "
                                 "(v%llu -> v%llu) — staying dirty for next sync\n",
                                 static_cast<unsigned long long>(snap_version),
                                 static_cast<unsigned long long>(index(w).version()));
                std::fprintf(stderr, "tapir: writer: manifest written at tape file %d\n", out_mtf);
            }
            else
            {
                std::fprintf(stderr, "tapir: writer: FAILED to write manifest to tape\n");
            }
            done_.set_value();
        }
    } // anonymous namespace

    // ── WriterThread ────────────────────────────────────────────────────────────

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
        push(std::make_unique<WriteMemberTask>(node, std::move(data),
                                               std::move(path), mtime));
    }

    void WriterThread::sync(std::unique_lock<std::mutex> &lk)
    {
        if (!index_.dirty())
            return;

        auto task = std::make_unique<SyncTask>();
        std::future<void> fut = task->get_future();
        push(std::move(task));

        lk.unlock(); // release state_mtx_ so the writer thread can acquire it and run the sync task
        fut.wait();  // wait until the writer thread finishes the sync task
        lk.lock();   // reacquire the lock the caller handed us
    }

} // namespace tapir
