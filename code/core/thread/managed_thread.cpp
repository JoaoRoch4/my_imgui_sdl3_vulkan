#include "pch.hpp"
#include "managed_thread.hpp"
#include "thread_registry.hpp"
#include "core/log/debug_log.hpp"

namespace {
constexpr std::size_t k_max_thread_name = 15; // Linux pthread name limit (15 + NUL)
} // namespace

std::string ManagedThread::truncate_name(std::string_view name)
{
    if (name.size() <= k_max_thread_name)
        return std::string(name);

    APP_DEBUG_LOG("[ManagedThread] name '{}' exceeds {} chars; truncating to '{}'",
                  name, k_max_thread_name, name.substr(0, k_max_thread_name));
    return std::string(name.substr(0, k_max_thread_name));
}

ManagedThread::ManagedThread(Config cfg, IterationFn body)
    : m_cfg(std::move(cfg))
    , m_name(truncate_name(m_cfg.name))
    , m_body(std::move(body))
{
    m_registry_id = ThreadRegistry::instance().register_thread(m_name, m_cfg.policy);

    start_thread_only();

    if (m_cfg.watch) {
        m_watch_id = ThreadOverwatch::instance().watch(
            m_name, m_cfg.timeout,
            /*kill*/ [this] { request_stop(); },
            /*restart*/ [this] { respawn(); },
            m_cfg.policy);
        ThreadRegistry::instance().set_watch_id(m_registry_id, m_watch_id);
    }
}

ManagedThread::~ManagedThread()
{
    {
        std::lock_guard lk(m_respawn_mutex);
        m_shutting_down = true; // block any in-flight respawn from re-spawning
    }

    // Unwatch BEFORE joining so the monitor thread cannot fire a restart mid-teardown.
    if (m_cfg.watch)
        ThreadOverwatch::instance().unwatch(m_watch_id);

    request_stop();
    if (m_thread.joinable())
        m_thread.join();

    ThreadRegistry::instance().unregister_thread(m_registry_id);
}

void ManagedThread::request_stop()
{
    m_thread.request_stop();
}

bool ManagedThread::joinable() const
{
    return m_thread.joinable();
}

void ManagedThread::set_status(std::string key, std::string value)
{
    ThreadRegistry::instance().set_status(m_registry_id, std::move(key), std::move(value));
}

void ManagedThread::heartbeat()
{
    if (m_cfg.watch)
        ThreadOverwatch::instance().heartbeat(m_watch_id);
    ThreadRegistry::instance().note_heartbeat(m_registry_id);
}

void ManagedThread::start_thread_only()
{
    m_thread = std::jthread{[this](std::stop_token st) { run(st); }};
}

void ManagedThread::run(const std::stop_token& st)
{
    pthread_setname_np(pthread_self(), m_name.c_str());
    ThreadRegistry::instance().set_tid(m_registry_id, ::gettid());
    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Running);

    while (!st.stop_requested()) {
        if (m_cfg.watch)
            ThreadOverwatch::instance().heartbeat(m_watch_id);
        ThreadRegistry::instance().note_iteration(m_registry_id);

        try {
            m_body(st, *this);
        } catch (const std::exception &e) {
            // One bad iteration must not kill the worker — log and keep looping.
            APP_DEBUG_LOG("[ManagedThread] {} body threw: {}", m_name, e.what());
        } catch (...) {
            APP_DEBUG_LOG("[ManagedThread] {} body threw unknown exception", m_name);
        }
    }

    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Stopped);
}

void ManagedThread::respawn() // runs on the Overwatch monitor thread
{
    std::lock_guard lk(m_respawn_mutex);
    if (m_shutting_down)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_restart >= m_cfg.restart_reset_window)
        m_consecutive_restarts = 0; // streak broken — the thread had recovered
    ++m_consecutive_restarts;
    m_last_restart = now;

    if (m_consecutive_restarts > m_cfg.max_consecutive_restarts)
        escalate(); // [[noreturn]] — aborts the process

    ThreadRegistry::instance().note_restart(m_registry_id); // ++restart_count, state = Restarting

    m_thread.request_stop();
    if (m_thread.joinable())
        m_thread.join(); // drains a cooperative thread; a truly wedged one leaks

    start_thread_only(); // watch + registry entry persist across the respawn
}

void ManagedThread::escalate()
{
    const std::string reason = std::format("restart storm: {} consecutive restarts within {} ms",
                                            m_consecutive_restarts, m_cfg.restart_reset_window.count());

    // std::println (not APP_DEBUG_LOG) so the fatal path is visible in Release too.
    std::println(stderr, "[ManagedThread] FATAL {}: {}", m_name, reason);

    if (m_cfg.watch)
        ThreadOverwatch::instance().unwatch(m_watch_id);
    ThreadRegistry::instance().set_failed(m_registry_id, reason);

    std::println(stderr, "[ManagedThread] stacktrace:\n{}", std::to_string(std::stacktrace::current()));
    std::fflush(stderr);
    std::abort();
}

std::unique_ptr<ManagedThread> spawn_demo_thread(std::chrono::seconds lifetime, std::string name)
{
    ManagedThread::Config cfg;
    cfg.name   = std::move(name);
    cfg.policy = ThreadOverwatch::RecoveryPolicy::KillOnly;
    cfg.watch  = true;

    const auto start = std::chrono::steady_clock::now();
    return std::make_unique<ManagedThread>(cfg,
        [start, lifetime](const std::stop_token & /*st*/, ManagedThread &self) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            self.set_status("elapsed_s",
                            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()));
            if (lifetime > std::chrono::seconds{0} && elapsed >= lifetime) {
                self.request_stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
}
