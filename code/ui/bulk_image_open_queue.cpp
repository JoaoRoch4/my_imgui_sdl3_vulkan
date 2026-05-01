#include "bulk_image_open_queue.hpp"

#include <filesystem>
#include <utility>

BulkImageOpenQueue::BulkImageOpenQueue()
    : m_worker{}
    , m_mutex{}
    , m_ready_paths{}
{
}

void BulkImageOpenQueue::enqueue_batch(std::vector<std::string> paths)
{
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready_paths.clear();
    }

    m_worker = std::jthread([this, paths = std::move(paths)](const std::stop_token& stoken) {
        std::deque<std::string> validated;
        for (const auto &path : paths) {
            if (stoken.stop_requested())
                return;

            if (std::filesystem::exists(path))
                validated.push_back(path);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &path : validated)
            m_ready_paths.push_back(path);
    });
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
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready_paths.clear();
}
