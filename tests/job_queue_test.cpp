#include <doctest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include "job_queue.hpp"

using namespace img;

namespace {

// Drives a JobQueue from `n` plain std::jthreads (stand-ins for the app's
// ManagedThread workers), looping on wait_and_run_one until stopped.
struct DriverPool {
    JobQueue                 &q;
    std::vector<std::jthread> drivers;

    DriverPool(JobQueue &queue, unsigned n) : q(queue) {
        for (unsigned i = 0; i < n; ++i)
            drivers.emplace_back([this](std::stop_token st) {
                while (!st.stop_requested())
                    q.wait_and_run_one(std::chrono::milliseconds(20), st);
            });
    }
    ~DriverPool() {
        for (auto &d : drivers) d.request_stop();
        q.request_stop(); // wake any blocked waiters so they observe the stop
    }
};

} // namespace

TEST_CASE("parallel_for covers the whole range exactly once (caller-only)") {
    JobQueue         q;
    std::vector<int> hits(1000, 0);
    q.parallel_for(0, 1000, 1,
                   [&](int lo, int hi) { for (int i = lo; i < hi; ++i) hits[i]++; },
                   /*helper_hint=*/0);
    for (const int h : hits) CHECK(h == 1);
}

TEST_CASE("parallel_for sums a large range across driver threads") {
    JobQueue   q;
    DriverPool pool(q, 4);

    std::atomic<long long> sum{0};
    constexpr int          N = 100000;
    q.parallel_for(0, N, 256,
                   [&](int lo, int hi) {
                       long long s = 0;
                       for (int i = lo; i < hi; ++i) s += i;
                       sum += s;
                   },
                   /*helper_hint=*/4);

    const long long expected = static_cast<long long>(N - 1) * N / 2;
    CHECK(sum.load() == expected);
}

TEST_CASE("nested parallel_for does not deadlock") {
    JobQueue   q;
    DriverPool pool(q, 2); // fewer workers than nested fan-out

    std::atomic<int> total{0};
    q.parallel_for(0, 8, 1,
                   [&](int lo, int hi) {
                       for (int o = lo; o < hi; ++o)
                           q.parallel_for(0, 50, 1,
                                          [&](int a, int b) { total += (b - a); },
                                          /*helper_hint=*/2);
                   },
                   /*helper_hint=*/2);

    CHECK(total.load() == 8 * 50);
}

TEST_CASE("submit runs work and returns the result") {
    JobQueue   q;
    DriverPool pool(q, 2);
    auto       f = q.submit([] { return 21 * 2; });
    CHECK(f.get() == 42);
}

TEST_CASE("submit captures exceptions into the future") {
    JobQueue   q;
    DriverPool pool(q, 1);
    auto       f = q.submit([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(f.get(), std::runtime_error);
}

TEST_CASE("clear_pending drops queued work") {
    JobQueue q; // no drivers: nothing runs, so work stays queued
    auto     f1 = q.submit([] { return 1; });
    auto     f2 = q.submit([] { return 2; });
    CHECK(q.pending() == 2);
    q.clear_pending();
    CHECK(q.pending() == 0);
}

TEST_CASE("wait_and_run_one returns false when stopped with an empty queue") {
    JobQueue        q;
    q.request_stop();
    std::stop_source ss;
    CHECK_FALSE(q.wait_and_run_one(std::chrono::milliseconds(5), ss.get_token()));
}
