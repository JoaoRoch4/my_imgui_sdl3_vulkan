#pragma once

#include "pch.hpp"

#include <condition_variable>
#include <deque>
#include <stop_token>

/// Filesystem mutation service for the file browser.
///
/// Fast metadata ops (create_directory, rename) run synchronously. Heavy ops
/// (recursive remove, copy, move) are queued on a single serial worker thread so
/// they never block the render thread; progress is polled via poll(). The worker
/// registers a per-job ThreadOverwatch watch (KillOnly) and heartbeats per entry,
/// mirroring FileBrowserScanner.
class FileBrowserFileOperations {
public:
    FileBrowserFileOperations();
    ~FileBrowserFileOperations();

    FileBrowserFileOperations(const FileBrowserFileOperations&)            = delete;
    FileBrowserFileOperations& operator=(const FileBrowserFileOperations&) = delete;

    struct Result {
        bool        ok = true;
        std::string error; // empty on success
    };

    // Fast metadata ops — synchronous, instant feedback.
    Result create_directory(const std::filesystem::path& dir);
    Result rename(const std::filesystem::path& from, const std::filesystem::path& to);

    using JobId = std::uint64_t;

    // Heavy ops — queued on the worker, never block the UI. Returns the job id.
    JobId submit_remove(const std::filesystem::path& target);                      // recursive
    JobId submit_copy(const std::filesystem::path& from, const std::filesystem::path& to);
    JobId submit_move(const std::filesystem::path& from, const std::filesystem::path& to);

    struct Progress {
        enum class State { Running, Done, Failed };
        State                 state = State::Running;
        std::uint64_t         done  = 0;
        std::uint64_t         total = 0;
        std::filesystem::path current;
        std::string           error; // populated on Failed
    };

    // Non-blocking (render thread). false if `id` is unknown. A Done/Failed job is
    // reported once then erased, so poll it until it returns a terminal state.
    bool poll(JobId id, Progress& out);

    void shutdown(); // stop + join the worker

private:
    enum class Op { Remove, Copy, Move };
    struct Job {
        JobId                 id;
        Op                    op;
        std::filesystem::path from;
        std::filesystem::path to;
    };

    JobId enqueue(Op op, std::filesystem::path from, std::filesystem::path to);
    void  worker_loop(std::stop_token st);
    void  run_job(const Job& job, std::uint64_t watch_id);
    std::uint64_t count_entries(const std::filesystem::path& root) const;

    std::mutex                          m_mutex;
    std::condition_variable_any         m_cv;
    std::deque<Job>                     m_queue;
    std::unordered_map<JobId, Progress> m_progress;
    std::atomic<JobId>                  m_next_id{1};
    std::atomic<bool>                   m_kill_requested{false};
    std::jthread                        m_worker;
};
