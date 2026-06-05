#pragma once
#include "pch.hpp"
#include "thread_overwatch.hpp"

// Lifecycle/health state of a managed thread, surfaced in the reflection panel.
enum class ThreadState { Starting, Running, Stopping, Stopped, Restarting, Failed };

// Live, read-only snapshot record for one ManagedThread.
struct ThreadInfo {
    uint64_t                              id            = 0;
    std::string                           name;
    pid_t                                 tid           = 0;
    ThreadState                           state         = ThreadState::Starting;
    ThreadOverwatch::RecoveryPolicy       policy        = ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
    uint64_t                              iterations    = 0;
    uint64_t                              restart_count = 0;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point last_heartbeat{};
    uint64_t                              overwatch_watch_id = 0;
    std::string                           failed_reason; // set when state == Failed
    std::vector<std::pair<std::string, std::string>> status; // custom key/value
};

// Process-wide registry of every ManagedThread, for read-only introspection.
//
// Kept separate from ThreadOverwatch (which owns liveness and recovery): this
// registry never acts, it only records. The two are linked by the Overwatch
// WatchId stored in each ThreadInfo. ManagedThread registers on construction and
// unregisters on destruction, so the registry is never stale.
class ThreadRegistry {
public:
    static ThreadRegistry &instance();

    ThreadRegistry(const ThreadRegistry &)            = delete;
    ThreadRegistry &operator=(const ThreadRegistry &) = delete;

    uint64_t register_thread(std::string name, ThreadOverwatch::RecoveryPolicy policy);
    void     unregister_thread(uint64_t id);

    void set_tid(uint64_t id, pid_t tid);
    void note_iteration(uint64_t id); // ++iterations, stamp heartbeat
    void note_heartbeat(uint64_t id); // stamp heartbeat only (mid-iteration keep-alive)
    void note_restart(uint64_t id);   // ++restart_count, state = Restarting
    void set_state(uint64_t id, ThreadState state);
    void set_failed(uint64_t id, std::string reason);
    void set_status(uint64_t id, std::string key, std::string value);
    void set_watch_id(uint64_t id, uint64_t watch_id);

    std::vector<ThreadInfo> snapshot() const; // value copy under shared lock

private:
    ThreadRegistry() = default;

    mutable std::shared_mutex                m_mutex;
    std::unordered_map<uint64_t, ThreadInfo> m_threads;
    std::atomic<uint64_t>                    m_next_id{1};
};
