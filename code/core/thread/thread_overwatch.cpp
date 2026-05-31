#include "pch.hpp"
#include "thread_overwatch.hpp"
#include "core/log/debug_log.hpp"

namespace {

constexpr bool k_thread_overwatch_debug = true;

template <typename... Args>
void overwatch_debug(std::format_string<Args...> fmt, Args &&...args)
{
    if constexpr (k_thread_overwatch_debug)
        APP_DEBUG_LOG(fmt, std::forward<Args>(args)...);
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ThreadOverwatch &ThreadOverwatch::instance()
{
    static ThreadOverwatch g_instance;
    return g_instance;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ThreadOverwatch::ThreadOverwatch()
    : m_monitor{}
    , m_mutex{}
    , m_watches{}
    , m_next_id{1}
{
    overwatch_debug("[ThreadOverwatch] monitor thread starting");
    m_monitor = std::jthread{[this](const std::stop_token &st) { monitor_loop(st); }};
}

ThreadOverwatch::~ThreadOverwatch()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ThreadOverwatch::WatchId ThreadOverwatch::watch(std::string               name,
                                                std::chrono::milliseconds timeout,
                                                std::function<void()>     kill_request,
                                                std::function<void()>     restart_request,
                                                RecoveryPolicy            policy)
{
    const WatchId id = m_next_id.fetch_add(1, std::memory_order_relaxed);

    WatchEntry entry{
        .name                 = std::move(name),
        .timeout              = timeout,
        .last_heartbeat       = std::chrono::steady_clock::now(),
        .kill_request         = std::move(kill_request),
        .restart_request      = std::move(restart_request),
        .policy               = policy,
        .recovery_in_progress = false,
    };

    std::lock_guard<std::mutex> lock(m_mutex);
    m_watches.emplace(id, std::move(entry));

    const auto it = m_watches.find(id);
    if (it != m_watches.end()) {
        overwatch_debug("[ThreadOverwatch] watch added: {} (id={}, timeout={}ms, policy={})",
                        it->second.name,
                        id,
                        static_cast<long long>(it->second.timeout.count()),
                        it->second.policy == RecoveryPolicy::KillOnly ? "KillOnly" : "RestartOnTimeout");
    }

    return id;
}

void ThreadOverwatch::unwatch(WatchId id)
{
    if (id == 0)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_watches.find(id);
    if (it == m_watches.end()) {
        overwatch_debug("[ThreadOverwatch] unwatch ignored: missing id={}", id);
        return;
    }

    overwatch_debug("[ThreadOverwatch] watch removed: {} (id={})", it->second.name, id);
    m_watches.erase(it);
}

void ThreadOverwatch::heartbeat(WatchId id)
{
    if (id == 0)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_watches.find(id);
    if (it == m_watches.end())
        return;

    it->second.last_heartbeat = std::chrono::steady_clock::now();
}

void ThreadOverwatch::shutdown()
{
    overwatch_debug("[ThreadOverwatch] shutdown requested");

    if (m_monitor.joinable()) {
        m_monitor.request_stop();
        m_monitor.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    overwatch_debug("[ThreadOverwatch] clearing {} watch(es)", m_watches.size());
    m_watches.clear();
}

// ---------------------------------------------------------------------------
// Monitor loop
// ---------------------------------------------------------------------------

void ThreadOverwatch::monitor_loop(const std::stop_token &stoken)
{
    using clock = std::chrono::steady_clock;

    overwatch_debug("[ThreadOverwatch] monitor loop started");
    auto last_summary = clock::now();

    for (;;) {
        if (stoken.stop_requested())
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Periodic summary
        const auto now = clock::now();
        if (now - last_summary >= std::chrono::seconds(10)) {
            std::size_t watch_count     = 0;
            std::size_t recovering_count = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                watch_count = m_watches.size();
                for (const auto &[id, entry] : m_watches)
                    if (entry.recovery_in_progress)
                        ++recovering_count;
            }
            overwatch_debug("[ThreadOverwatch] summary: watches={} recovering={}",
                            watch_count, recovering_count);
            last_summary = now;
        }

        // Collect timed-out watches
        struct RecoveryAction {
            WatchId               id;
            std::string           name;
            RecoveryPolicy        policy;
            std::function<void()> kill_request;
            std::function<void()> restart_request;
        };

        std::vector<RecoveryAction> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto loop_now = clock::now();

            for (auto &[id, entry] : m_watches) {
                if (entry.recovery_in_progress)
                    continue;
                if (loop_now - entry.last_heartbeat <= entry.timeout)
                    continue;

                entry.recovery_in_progress = true;
                pending.push_back({
                    .id              = id,
                    .name            = entry.name,
                    .policy          = entry.policy,
                    .kill_request    = entry.kill_request,
                    .restart_request = entry.restart_request,
                });
            }
        }

        // Execute recovery actions outside the lock
        for (auto &action : pending) {
            APP_DEBUG_LOG("[ThreadOverwatch] heartbeat timeout: {} (id={}) -> {}",
                          action.name, action.id,
                          action.policy == RecoveryPolicy::KillOnly ? "kill only" : "kill + restart");

            if (action.kill_request) {
                overwatch_debug("[ThreadOverwatch] kill request: {} (id={})", action.name, action.id);
                action.kill_request();
            }

            if (action.policy == RecoveryPolicy::KillOnly) {
                // Remove the watch and stop — no restart, no re-arm
                overwatch_debug("[ThreadOverwatch] kill-only: removing watch (id={})", action.id);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_watches.erase(action.id);
                continue;
            }

            // RestartOnTimeout
            if (action.restart_request) {
                overwatch_debug("[ThreadOverwatch] restart request: {} (id={})", action.name, action.id);
                action.restart_request();
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_watches.find(action.id);
                if (it != m_watches.end()) {
                    it->second.recovery_in_progress = false;
                    it->second.last_heartbeat       = clock::now();
                    overwatch_debug("[ThreadOverwatch] recovery completed: {} (id={})",
                                    it->second.name, action.id);
                } else {
                    overwatch_debug("[ThreadOverwatch] recovery finished but watch gone (id={})", action.id);
                }
            }
        }
    }

    overwatch_debug("[ThreadOverwatch] monitor loop stopped");
}