#include "bulk_image_open_queue.hpp"
#include "core/thread/thread_overwatch.hpp"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BulkImageOpenQueue::BulkImageOpenQueue()
    : m_worker{}
    , m_mutex{}
    , m_ready_paths{}
    , m_active_paths{}
    , m_worker_watch_id{0}
{}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BulkImageOpenQueue::enqueue_batch(const std::vector<std::string> &paths)
{
    stop_worker_thread(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready_paths.clear();
        m_active_paths = paths;
    }
    start_worker_thread(paths);
}

bool BulkImageOpenQueue::try_pop_ready(std::string *out_path)
{
    if (!out_path)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready_paths.empty())
        return false;

    *out_path = m_ready_paths.front();
    m_ready_paths.pop_front();
    return true;
}

void BulkImageOpenQueue::shutdown()
{
    stop_worker_thread(true);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready_paths.clear();
    m_active_paths.clear();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BulkImageOpenQueue::start_worker_thread(const std::vector<std::string> &paths)
{
    if (m_worker.joinable())
        return;

    // Register watch BEFORE launching the thread so the thread captures a
    // valid, non-zero ID on its very first heartbeat call.
    // Policy: KillOnly — if the worker hangs, kill it and remove the watch.
    // No automatic restart loop.
    const uint64_t watch_id = ThreadOverwatch::instance().watch(
        "BulkImageOpenQueue::worker",
        std::chrono::milliseconds(5000),
        [this]() { stop_worker_thread(false); }, // kill: stop thread, leave watch for cleanup
        nullptr,                                  // no restart
        ThreadOverwatch::RecoveryPolicy::KillOnly
    );

    m_worker_watch_id.store(watch_id, std::memory_order_release);

    // Capture watch_id and paths by value — the thread owns its own copies.
    m_worker = std::jthread([this, watch_id, paths](const std::stop_token &stoken) {

        ThreadOverwatch::instance().heartbeat(watch_id);

        std::deque<std::string> validated;
        for (const auto &path : paths) {
            if (stoken.stop_requested())
                break;

            if (std::filesystem::exists(path))
                validated.push_back(path);

            // Heartbeat every iteration — filesystem::exists can block on
            // network mounts and slow drives.
            ThreadOverwatch::instance().heartbeat(watch_id);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!stoken.stop_requested()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto &p : validated)
                m_ready_paths.push_back(p);
            m_active_paths.clear();
        }

        // Only clear the watch ID if it still belongs to this thread.
        // (KillOnly policy removes the watch entry from overwatch automatically,
        // so we just zero the atomic here for cleanliness.)
        const uint64_t current = m_worker_watch_id.load(std::memory_order_acquire);
        if (current == watch_id)
            m_worker_watch_id.store(0, std::memory_order_release);
    });
}

void BulkImageOpenQueue::stop_worker_thread(bool unregister_watch)
{
    // Unregister BEFORE joining so overwatch doesn't fire on a thread
    // we are intentionally stopping.
    if (unregister_watch) {
        const uint64_t watch_id = m_worker_watch_id.exchange(0, std::memory_order_acq_rel);
        ThreadOverwatch::instance().unwatch(watch_id);
    }

    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}