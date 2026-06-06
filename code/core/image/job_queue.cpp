#include "job_queue.hpp"

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

namespace img {

void JobQueue::push(std::function<void()> fn, Priority p) {
    {
        std::lock_guard lk(m_mutex);
        switch (p) {
        case Priority::High: m_high.push_back({std::move(fn), p}); break;
        case Priority::Low:  m_low.push_back({std::move(fn), p}); break;
        default:             m_normal.push_back({std::move(fn), p}); break;
        }
    }
    m_cv.notify_one();
}

bool JobQueue::pop(Task &out) {
    std::lock_guard lk(m_mutex);
    if (!m_high.empty())   { out = std::move(m_high.front());   m_high.pop_front();   return true; }
    if (!m_normal.empty()) { out = std::move(m_normal.front()); m_normal.pop_front(); return true; }
    if (!m_low.empty())    { out = std::move(m_low.front());    m_low.pop_front();    return true; }
    return false;
}

bool JobQueue::run_one() {
    Task t;
    if (!pop(t))
        return false;
    if (t.fn)
        t.fn();
    return true;
}

bool JobQueue::wait_and_run_one(std::chrono::milliseconds timeout, const std::stop_token &st) {
    {
        std::unique_lock lk(m_mutex);
        m_cv.wait_for(lk, st, timeout, [&] {
            return m_stopping.load(std::memory_order_relaxed) || !m_high.empty() ||
                   !m_normal.empty() || !m_low.empty();
        });
    }
    return run_one();
}

void JobQueue::parallel_for(int begin, int end, int min_chunk,
                            const std::function<void(int, int)> &body, unsigned helper_hint) {
    if (begin >= end || !body)
        return;
    if (min_chunk < 1)
        min_chunk = 1;

    const long long total              = static_cast<long long>(end) - begin;
    const long long max_chunks_by_size = (total + min_chunk - 1) / min_chunk;
    const long long want               = static_cast<long long>(helper_hint) + 1; // +1 = caller
    int             chunks             = static_cast<int>(std::min(max_chunks_by_size, want));
    if (chunks < 1)
        chunks = 1;

    const auto chunk_range = [begin, total, chunks](int ci) -> std::pair<int, int> {
        const long long base = total / chunks;
        const long long rem  = total % chunks;
        const long long lo   = begin + ci * base + std::min<long long>(ci, rem);
        const long long hi   = lo + base + (ci < rem ? 1 : 0);
        return {static_cast<int>(lo), static_cast<int>(hi)};
    };

    // Chunks are claimed via an atomic index, so any thread (a pushed helper task OR
    // the calling thread) can run any not-yet-claimed chunk. This makes the loop
    // robust: even if the pending helper tasks are dropped (clear_pending), the
    // calling thread alone will still run every chunk — it cannot hang.
    auto       next = std::make_shared<std::atomic<int>>(0);
    auto       done = std::make_shared<std::atomic<int>>(0);
    const auto work = [chunks, next, done, &body, chunk_range] {
        for (;;) {
            const int ci = next->fetch_add(1, std::memory_order_relaxed);
            if (ci >= chunks)
                break;
            const auto [lo, hi] = chunk_range(ci);
            if (lo < hi)
                body(lo, hi);
            done->fetch_add(1, std::memory_order_acq_rel);
        }
    };

    for (int i = 1; i < chunks; ++i)
        push([work] { work(); }, Priority::High);

    work(); // calling thread participates

    while (done->load(std::memory_order_acquire) < chunks) {
        if (!run_one())
            std::this_thread::yield();
    }
}

void JobQueue::request_stop() {
    m_stopping.store(true, std::memory_order_relaxed);
    m_cv.notify_all();
}

void JobQueue::reset_stop() { m_stopping.store(false, std::memory_order_relaxed); }

void JobQueue::clear_pending() {
    std::lock_guard lk(m_mutex);
    m_high.clear();
    m_normal.clear();
    m_low.clear();
}

std::size_t JobQueue::pending() const {
    std::lock_guard lk(m_mutex);
    return m_high.size() + m_normal.size() + m_low.size();
}

} // namespace img
