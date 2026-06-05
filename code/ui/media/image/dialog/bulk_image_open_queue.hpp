#pragma once

#include "pch.hpp"

class ManagedThread;

class BulkImageOpenQueue {
public:
    BulkImageOpenQueue();
    ~BulkImageOpenQueue();

    BulkImageOpenQueue(const BulkImageOpenQueue &) = delete;
    BulkImageOpenQueue &operator=(const BulkImageOpenQueue &) = delete;

    void enqueue_batch(const std::vector<std::string>& paths);
    bool try_pop_ready(std::string *out_path);
    void shutdown();

private:
    void start_worker_thread(const std::vector<std::string> &paths);
    void stop_worker_thread();

    std::unique_ptr<ManagedThread> m_worker;
    std::mutex m_mutex;
    std::deque<std::string> m_ready_paths;
    std::vector<std::string> m_active_paths;
};
