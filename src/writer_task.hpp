// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// writer_task.hpp — background-writer work items for WriterThread.
//
// WriterTask is the queue element type and the SOLE friend of WriterThread: its
// protected broker accessors hand WriterThread's private state down to subclasses,
// so a new kind of task is just a new WriterTask subclass in writer_task.cpp — no
// extra friend declaration and no direct field access. The concrete task types are
// implementation details (defined in writer_task.cpp); callers build them through
// the factory functions below.

#pragma once

#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace tapir
{

// Forward declarations — writer_task.cpp includes the full headers.
class  Tape;
class  Index;
struct Node;
struct Staged;
class  WriterThread;

// Base class for a background-writer work item. Concrete tasks subclass this and
// implement run(). WriterThread grants friendship to THIS class only; the protected
// accessors carry that access down to subclasses, so subclasses never need to be
// friends themselves and never touch WriterThread's fields directly.
class WriterTask
{
public:
    virtual ~WriterTask() = default;      // virtual: destroyed via unique_ptr<WriterTask>
    virtual void run(WriterThread &w) = 0;

protected:
    // Broker accessors (defined in writer_task.cpp, where WriterThread is complete).
    static Tape       &tape(WriterThread &w);
    static Index      &index(WriterThread &w);
    static std::mutex &state_mtx(WriterThread &w);
    static int         block_factor(WriterThread &w);
    // The currently-open data tape file (empty between a sync and the next write).
    // Returned as the raw optional for read/reset; construction of the private
    // OpenWrite goes through ensure_open() (the one step that needs friendship).
    static auto       &open_write(WriterThread &w);   // std::optional<WriterThread::OpenWrite>&
    static bool        ensure_open(WriterThread &w);   // open a data tape file if none is in progress
};

// Task factories — the concrete WriterTask subclasses stay internal to
// writer_task.cpp; callers (WriterThread) only see the base and these.
std::unique_ptr<WriterTask> make_write_member_task(Node *node, std::shared_ptr<Staged> data,
                                                   std::string path, time_t mtime);
// Creates a sync task; out_future receives the future to wait on for completion.
std::unique_ptr<WriterTask> make_sync_task(std::future<void> &out_future);

} // namespace tapir
