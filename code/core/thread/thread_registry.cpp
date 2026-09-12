#include "pch.hpp"
#include "thread_registry.hpp"

ThreadRegistry &ThreadRegistry::instance()
{
    // Immortal on purpose. A registry with automatic storage is destroyed in
    // reverse order of construction, and this one is constructed EARLY (the
    // first ManagedThread) while the objects owning those threads live in
    // MemoryManagement, destroyed LATE — so any thread still running at static
    // exit would call set_state()/unregister_thread() on a freed hash table.
    // Owners are expected to tear their threads down in App::destroy(); this
    // just makes the failure mode a harmless no-op instead of memory
    // corruption. Never deleted, but the static pointer keeps it reachable, so
    // LeakSanitizer does not report it.
    static ThreadRegistry *const g_instance = new ThreadRegistry();
    return *g_instance;
}

uint64_t ThreadRegistry::register_thread(std::string name, ThreadOverwatch::RecoveryPolicy policy)
{
    const uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);

    ThreadInfo info{};
    info.id             = id;
    info.name           = std::move(name);
    info.policy         = policy;
    info.state          = ThreadState::Starting;
    info.started_at     = std::chrono::steady_clock::now();
    info.last_heartbeat = info.started_at;

    std::unique_lock lock(m_mutex);
    m_threads.emplace(id, std::move(info));
    return id;
}

void ThreadRegistry::unregister_thread(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    m_threads.erase(id);
}

void ThreadRegistry::set_tid(uint64_t id, pid_t tid)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end())
        it->second.tid = tid;
}

void ThreadRegistry::note_iteration(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end()) {
        ++it->second.iterations;
        it->second.last_heartbeat = std::chrono::steady_clock::now();
    }
}

void ThreadRegistry::note_heartbeat(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end())
        it->second.last_heartbeat = std::chrono::steady_clock::now();
}

void ThreadRegistry::note_restart(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end()) {
        ++it->second.restart_count;
        it->second.state = ThreadState::Restarting;
    }
}

void ThreadRegistry::set_state(uint64_t id, ThreadState state)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end())
        it->second.state = state;
}

void ThreadRegistry::set_failed(uint64_t id, std::string reason)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end()) {
        it->second.state         = ThreadState::Failed;
        it->second.failed_reason = std::move(reason);
    }
}

void ThreadRegistry::set_status(uint64_t id, std::string key, std::string value)
{
    std::unique_lock lock(m_mutex);
    const auto it = m_threads.find(id);
    if (it == m_threads.end())
        return;

    auto &status = it->second.status;
    const auto kv = std::ranges::find_if(status, [&](const auto &p) { return p.first == key; });
    if (kv != status.end())
        kv->second = std::move(value);
    else
        status.emplace_back(std::move(key), std::move(value));
}

void ThreadRegistry::set_watch_id(uint64_t id, uint64_t watch_id)
{
    std::unique_lock lock(m_mutex);
    if (const auto it = m_threads.find(id); it != m_threads.end())
        it->second.overwatch_watch_id = watch_id;
}

std::vector<ThreadInfo> ThreadRegistry::snapshot() const
{
    std::shared_lock lock(m_mutex);
    std::vector<ThreadInfo> out;
    out.reserve(m_threads.size());
    for (const auto &[id, info] : m_threads)
        out.push_back(info);
    return out;
}
