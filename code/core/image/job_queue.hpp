#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stop_token>
#include <type_traits>

#include "image_types.hpp"

// A small, dependency-free work queue with a cooperative parallel-for. It has NO
// dependency on the project PCH, ImGui, Vulkan or ManagedThread, so it is unit-
// tested in isolation. In the application, ImageJobSystem drives it from a pool of
// ManagedThread workers (each loops on wait_and_run_one); in tests, plain
// std::threads drive it. parallel_for additionally has the CALLING thread help
// execute chunks, so nesting a parallel_for inside a running job cannot deadlock
// the pool.

namespace img {

class JobQueue {
public:
    JobQueue()  = default;
    ~JobQueue() = default;
    JobQueue(const JobQueue &)            = delete;
    JobQueue &operator=(const JobQueue &) = delete;

    // Submit arbitrary work; returns a future for the result. Exceptions thrown by
    // `fn` are captured into the future (the worker keeps running).
    template <class F>
    [[nodiscard]] auto submit(F &&fn, Priority p = Priority::Normal)
        -> std::future<std::invoke_result_t<F>>;

    // Run a single queued task if one is available. Returns false if the queue was
    // empty. Worker loops call this; cooperative waits call it to help.
    bool run_one();

    // Block up to `timeout` for a task (or stop), then run one. Returns true if a
    // task ran. Used by worker loops so an idle wait still returns each iteration.
    bool wait_and_run_one(std::chrono::milliseconds timeout, const std::stop_token &st);

    // Cooperative parallel-for over [begin, end). Splits into up to
    // (helper_hint + 1) contiguous chunks, enqueues all but one, runs one on the
    // calling thread, then helps run pending work until every chunk has finished.
    // `body(lo, hi)` is invoked once per chunk over a disjoint sub-range.
    void parallel_for(int begin, int end, int min_chunk,
                      const std::function<void(int lo, int hi)> &body,
                      unsigned helper_hint);

    void                      request_stop();   // wake all waiters
    void                      reset_stop();     // clear the stop flag (reuse)
    void                      clear_pending();  // drop queued (not running) tasks
    [[nodiscard]] std::size_t pending() const;

private:
    struct Task {
        std::function<void()> fn;
        Priority              prio = Priority::Normal;
    };

    void push(std::function<void()> fn, Priority p);
    bool pop(Task &out); // locks internally; high -> normal -> low

    mutable std::mutex          m_mutex;
    std::condition_variable_any m_cv;
    std::deque<Task>            m_high;
    std::deque<Task>            m_normal;
    std::deque<Task>            m_low;
    std::atomic<bool>           m_stopping{false};
};

template <class F>
auto JobQueue::submit(F &&fn, Priority p) -> std::future<std::invoke_result_t<F>> {
    using R    = std::invoke_result_t<F>;
    auto task  = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
    auto fut   = task->get_future();
    push([task] { (*task)(); }, p);
    return fut;
}

} // namespace img
