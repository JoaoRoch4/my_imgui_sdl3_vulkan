#pragma once

#include "pch.hpp"

class BulkImageOpenQueue {
public:
    BulkImageOpenQueue();
    ~BulkImageOpenQueue() = default;

    BulkImageOpenQueue(const BulkImageOpenQueue &) = delete;
    BulkImageOpenQueue &operator=(const BulkImageOpenQueue &) = delete;

    void enqueue_batch(const std::vector<std::string>& paths);
    bool try_pop_ready(std::string *out_path);
    void shutdown();

private:
    void start_worker_thread(const std::vector<std::string> &paths);
    void stop_worker_thread(bool unregister_watch);

    std::jthread m_worker;
    std::mutex m_mutex;
    std::deque<std::string> m_ready_paths;
    std::vector<std::string> m_active_paths;
    std::atomic<uint64_t> m_worker_watch_id;
};
