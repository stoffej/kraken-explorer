#pragma once

#include <mutex>
#include <thread>
#include <vector>

#include "core/move_only_function.h"

struct App;

// Work handed from any thread to the main thread. Replaces Qt's queued and
// BlockingQueuedConnection: the main loop calls tasks_drain() once per frame.
using Task = MoveOnlyFunction<void(App&)>;

struct Tasks
{
    std::mutex mutex;
    std::vector<Task> queue;
    std::thread::id main_thread = std::this_thread::get_id(); // construct on the main thread
    void (*wake)() = nullptr; // e.g. glfwPostEmptyEvent, so a posted task never waits for the idle timeout
};

// Queues fn for the main thread and returns immediately. Thread-safe.
void tasks_post(Tasks& tasks, Task fn);

// Queues fn and blocks until the main thread has run it. Exceptions thrown by fn are
// rethrown here; std::future_error (broken_promise) if the queue is dropped unrun.
// Called on the main thread it runs fn directly instead of deadlocking.
void tasks_run_on_main_blocking(Tasks& tasks, App& app, Task fn);

// Runs every queued task on the calling (main) thread. Tasks posted while draining
// run on the next call.
void tasks_drain(Tasks& tasks, App& app);
