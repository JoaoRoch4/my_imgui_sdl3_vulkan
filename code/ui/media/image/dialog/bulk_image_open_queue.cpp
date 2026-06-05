#include "pch.hpp"

#include "bulk_image_open_queue.hpp"
#include "managed_thread.hpp"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BulkImageOpenQueue::BulkImageOpenQueue()
    : m_worker{}
    , m_mutex{}
    , m_ready_paths{}
    , m_active_paths{}
{}

// Out-of-line so unique_ptr<ManagedThread> sees the complete type (the destructor
// joins the worker via the ManagedThread destructor).
BulkImageOpenQueue::~BulkImageOpenQueue() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BulkImageOpenQueue::enqueue_batch(const std::vector<std::string> &paths)
{
    stop_worker_thread();
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
    stop_worker_thread();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready_paths.clear();
    m_active_paths.clear();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BulkImageOpenQueue::start_worker_thread(const std::vector<std::string> &paths)
{
    if (m_worker)
        return;

    // One-shot worker: ManagedThread names it ("BulkImageOpen") and watches it
    // (KillOnly). The body validates the whole batch in a single iteration —
    // heartbeating per path because filesystem::exists() can block on network
    // mounts — then stops the loop itself.
    ManagedThread::Config cfg;
    cfg.name    = "BulkImageOpen";
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    m_worker    = std::make_unique<ManagedThread>(
        cfg, [this, paths](const std::stop_token &stoken, ManagedThread &self) {
            std::deque<std::string> validated;
            for (const auto &path : paths) {
                if (stoken.stop_requested())
                    break;

                if (std::filesystem::exists(path))
                    validated.push_back(path);

                self.heartbeat();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (!stoken.stop_requested()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto &p : validated)
                    m_ready_paths.push_back(p);
                m_active_paths.clear();
            }

            self.request_stop(); // one-shot — end the loop after one pass
        });
}

void BulkImageOpenQueue::stop_worker_thread()
{
    if (!m_worker)
        return;

    m_worker->request_stop();
    m_worker.reset(); // ManagedThread destructor joins
}