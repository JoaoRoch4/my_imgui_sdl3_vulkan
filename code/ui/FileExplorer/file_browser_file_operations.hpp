#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "job_queue.hpp" // img::JobQueue (pure, header-safe)

class ManagedThread;

// File-operations service for the file browser, split into:
//   * fileops::  — pure std::filesystem functions (no ImGui / Vulkan / ManagedThread
//                  / PCH), unit-tested in isolation. They never throw across the API
//                  boundary (std::error_code only) and report progress + honour an
//                  abort callback so long operations can be cancelled mid-flight.
//   * FileBrowserFileOperations (added in a later step) — a thin async wrapper that
//     runs the heavy ops on a single serial worker and exposes poll-based progress.

namespace fileops {

// Result of an operation. `error` is empty on success.
struct Result {
    bool        ok = true;
    std::string error;

    [[nodiscard]] static Result success() { return {}; }
    [[nodiscard]] static Result failure(std::string e) { return {false, std::move(e)}; }
};

// Live progress for a long-running op. `total` is filled during a counting phase,
// then `done` / `current` advance as entries are processed.
struct Progress {
    std::uint64_t         done  = 0;
    std::uint64_t         total = 0;
    std::filesystem::path current;
};

// Returns true to abort the operation (checked between entries).
using AbortFn = std::function<bool()>;

// Called after each processed entry (and once when `total` is known) so a caller can
// surface live progress. Optional — defaulted to empty in the operations below.
using ProgressFn = std::function<void(const Progress &)>;

// Create a directory (and any missing parents). Success if it exists afterwards.
[[nodiscard]] Result create_directory(const std::filesystem::path &dir);

// Rename / move within a filesystem (no recursion, no copy fallback).
[[nodiscard]] Result rename(const std::filesystem::path &from, const std::filesystem::path &to);

// Recursively remove a file or directory tree. Counts entries into progress.total,
// then removes them updating progress.done / current. Honours abort().
[[nodiscard]] Result remove_recursive(const std::filesystem::path &target,
                                      Progress &progress, const AbortFn &abort,
                                      const ProgressFn &on_progress = {});

// Recursively copy a file or directory tree to `to` (cross-device safe).
[[nodiscard]] Result copy_tree(const std::filesystem::path &from,
                               const std::filesystem::path &to, Progress &progress,
                               const AbortFn &abort, const ProgressFn &on_progress = {});

// Move `from` to `to`: try a fast rename; on cross-device failure fall back to
// copy_tree + remove_recursive.
[[nodiscard]] Result move_path(const std::filesystem::path &from,
                               const std::filesystem::path &to, Progress &progress,
                               const AbortFn &abort, const ProgressFn &on_progress = {});

} // namespace fileops

// Async file-operations service owned by the file browser. Fast metadata ops run
// synchronously; heavy ops (recursive remove / copy / move) run on a single serial
// worker (one ManagedThread driving an img::JobQueue, registered with ThreadOverwatch)
// and expose poll-based progress. Serial by design: concurrent copies on one disk
// only thrash. No ImGui/Vulkan dependency.
class FileBrowserFileOperations {
public:
    FileBrowserFileOperations();
    ~FileBrowserFileOperations();
    FileBrowserFileOperations(const FileBrowserFileOperations &)            = delete;
    FileBrowserFileOperations &operator=(const FileBrowserFileOperations &) = delete;

    void start();    // spawn the serial worker
    void shutdown(); // abort in-flight, drop queued, join

    // Synchronous, instant metadata ops.
    [[nodiscard]] fileops::Result create_directory(const std::filesystem::path &dir);
    [[nodiscard]] fileops::Result rename(const std::filesystem::path &from,
                                         const std::filesystem::path &to);

    // Asynchronous heavy ops; return a JobId to poll().
    using JobId = std::uint64_t;
    JobId submit_remove(std::filesystem::path target);
    JobId submit_copy(std::filesystem::path from, std::filesystem::path to);
    JobId submit_move(std::filesystem::path from, std::filesystem::path to);

    struct JobStatus {
        enum class State { Running, Done, Failed };
        State                 state = State::Running;
        std::uint64_t         done  = 0;
        std::uint64_t         total = 0;
        std::filesystem::path current;
        std::string           error;
    };

    // Non-blocking snapshot; false if the id is unknown. Render thread.
    [[nodiscard]] bool poll(JobId id, JobStatus &out) const;

private:
    enum class Op { Remove, Copy, Move };
    JobId enqueue(Op op, std::filesystem::path a, std::filesystem::path b);
    void  run_job(JobId id, Op op, const std::filesystem::path &a,
                  const std::filesystem::path &b);

    img::JobQueue                                m_queue;
    std::unique_ptr<ManagedThread>              m_worker;
    ManagedThread                              *m_active_worker = nullptr; // worker-thread only
    mutable std::mutex                          m_mutex;
    std::unordered_map<JobId, JobStatus>        m_status;
    std::atomic<JobId>                          m_next_id{1};
    std::atomic<bool>                           m_abort{false};
    bool                                        m_running = false;
};
