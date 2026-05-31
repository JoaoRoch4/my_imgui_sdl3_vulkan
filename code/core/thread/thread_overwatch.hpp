#pragma once
#include "pch.hpp"

class ThreadOverwatch {
public:
    using WatchId = uint64_t;

    enum class RecoveryPolicy {
        RestartOnTimeout, // kill + restart (default)
        KillOnly,         // kill on timeout, unwatch automatically, no restart
    };

    static ThreadOverwatch &instance();

    ThreadOverwatch(const ThreadOverwatch &)            = delete;
    ThreadOverwatch &operator=(const ThreadOverwatch &) = delete;

    WatchId watch(std::string               name,
                  std::chrono::milliseconds timeout,
                  std::function<void()>     kill_request,
                  std::function<void()>     restart_request,
                  RecoveryPolicy            policy = RecoveryPolicy::RestartOnTimeout);

    void unwatch(WatchId id);
    void heartbeat(WatchId id);
    void shutdown();

private:
    struct WatchEntry {
        std::string                           name;
        std::chrono::milliseconds             timeout;
        std::chrono::steady_clock::time_point last_heartbeat;
        std::function<void()>                 kill_request;
        std::function<void()>                 restart_request;
        RecoveryPolicy                        policy;
        bool                                  recovery_in_progress;
    };

    ThreadOverwatch();
    ~ThreadOverwatch();
    void monitor_loop(const std::stop_token &stoken);

    std::jthread                             m_monitor;
    std::mutex                               m_mutex;
    std::unordered_map<WatchId, WatchEntry>  m_watches;
    std::atomic<WatchId>                     m_next_id;
};