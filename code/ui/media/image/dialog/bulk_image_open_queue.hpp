#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BulkImageOpenQueue {
public:
    BulkImageOpenQueue();
    ~BulkImageOpenQueue() = default;

    BulkImageOpenQueue(const BulkImageOpenQueue &) = delete;
    BulkImageOpenQueue &operator=(const BulkImageOpenQueue &) = delete;

    void enqueue_batch(std::vector<std::string> paths);
    bool try_pop_ready(std::string *out_path);
    void shutdown();

private:
    std::jthread m_worker;
    std::mutex m_mutex;
    std::deque<std::string> m_ready_paths;
};
