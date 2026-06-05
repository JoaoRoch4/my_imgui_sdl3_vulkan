#pragma once
#include "pch.hpp"
#include "thread_overwatch.hpp"

// RAII wrapper around std::jthread. Owns the loop, names the OS thread, registers
// with ThreadOverwatch (watchdog) and ThreadRegistry (introspection), and enforces
// a recovery policy with a restart-storm guard.
//
// Contract: the body runs once per iteration and MUST return within `timeout` even
// when idle. Condvar workers wait with `cv.wait_for(lk, timeout / 2, pred)`, never
// an unbounded wait — the watch lives for the whole thread lifetime, so a body that
// blocks longer than `timeout` is indistinguishable from a hang.
class ManagedThread {
public:
    // Repeated body. `self` lets the body publish status (set_status) or stop the
    // thread (one-shot tasks call self.request_stop() when their work is done).
    using IterationFn = std::function<void(const std::stop_token &, ManagedThread &)>;

    struct Config {
        std::string                     name;          // truncated to 15 chars (Linux limit)
        std::chrono::milliseconds       timeout{5000};
        ThreadOverwatch::RecoveryPolicy policy = ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
        bool                            watch  = true; // false → no Overwatch registration

        // Restart-storm guard (RestartOnTimeout only): if the thread respawns more
        // than `max_consecutive_restarts` times without staying healthy for
        // `restart_reset_window`, the process aborts.
        uint32_t                  max_consecutive_restarts = 5;
        std::chrono::milliseconds restart_reset_window{60'000};
    };

    ManagedThread(Config cfg, IterationFn body);
    ~ManagedThread();

    ManagedThread(const ManagedThread &)            = delete;
    ManagedThread &operator=(const ManagedThread &) = delete;

    void               request_stop();
    [[nodiscard]] bool joinable() const;
    void set_status(std::string key, std::string value);

private:
    void start_thread_only();      // spawn a fresh jthread running run()
    void run(std::stop_token st);  // the owned loop
    void respawn();                // Overwatch restart callback (monitor thread)
    [[noreturn]] void escalate();  // restart storm → log + abort

    static std::string truncate_name(std::string_view name);

    Config       m_cfg;
    std::string  m_name; // truncated, <= 15 chars
    IterationFn  m_body;
    uint64_t     m_registry_id = 0;
    uint64_t     m_watch_id    = 0;
    std::jthread m_thread;

    std::mutex                            m_respawn_mutex;
    bool                                  m_shutting_down        = false;
    uint32_t                              m_consecutive_restarts = 0;
    std::chrono::steady_clock::time_point m_last_restart{};
};
