#include "pch.hpp" // NOLINT

#include "file_browser_file_operations.hpp"

#include <chrono>
#include <utility>

#include "managed_thread.hpp"
#include "thread_overwatch.hpp"

FileBrowserFileOperations::FileBrowserFileOperations()  = default;
FileBrowserFileOperations::~FileBrowserFileOperations() { shutdown(); }

void FileBrowserFileOperations::start() {
    if (m_running)
        return;
    m_queue.reset_stop();
    m_abort.store(false, std::memory_order_relaxed);

    ManagedThread::Config mc;
    mc.name    = "FileBrowserOps";
    mc.timeout = std::chrono::milliseconds(120'000); // generous; heartbeats per entry
    mc.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    mc.watch   = true;

    m_worker = std::make_unique<ManagedThread>(
        mc, [this](const std::stop_token &st, ManagedThread &self) {
            m_active_worker = &self; // single serial worker; touched only here
            if (m_queue.wait_and_run_one(std::chrono::milliseconds(5'000), st))
                self.heartbeat();
            m_active_worker = nullptr;
        });
    m_running = true;
}

void FileBrowserFileOperations::shutdown() {
    if (!m_running)
        return;
    m_running = false;
    m_abort.store(true, std::memory_order_relaxed); // cancel any in-flight op
    m_queue.clear_pending();
    m_queue.request_stop();
    if (m_worker)
        m_worker->request_stop();
    m_worker.reset(); // joins
}

fileops::Result FileBrowserFileOperations::create_directory(const std::filesystem::path &dir) {
    return fileops::create_directory(dir);
}

fileops::Result FileBrowserFileOperations::rename(const std::filesystem::path &from,
                                                  const std::filesystem::path &to) {
    return fileops::rename(from, to);
}

FileBrowserFileOperations::JobId
FileBrowserFileOperations::enqueue(Op op, std::filesystem::path a, std::filesystem::path b) {
    const JobId id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lk(m_mutex);
        m_status[id] = JobStatus{}; // Running, 0/0
    }
    m_queue.submit(
        [this, id, op, a = std::move(a), b = std::move(b)] { run_job(id, op, a, b); },
        img::Priority::Normal);
    return id;
}

void FileBrowserFileOperations::run_job(JobId id, Op op, const std::filesystem::path &a,
                                        const std::filesystem::path &b) {
    fileops::Progress p;

    const fileops::ProgressFn on_progress = [this, id](const fileops::Progress &pr) {
        if (m_active_worker)
            m_active_worker->heartbeat(); // keep the watchdog alive during long ops
        std::lock_guard lk(m_mutex);
        if (auto it = m_status.find(id); it != m_status.end()) {
            it->second.done    = pr.done;
            it->second.total   = pr.total;
            it->second.current = pr.current;
        }
    };
    const fileops::AbortFn abort = [this] { return m_abort.load(std::memory_order_relaxed); };

    fileops::Result r;
    switch (op) {
    case Op::Remove: r = fileops::remove_recursive(a, p, abort, on_progress); break;
    case Op::Copy:   r = fileops::copy_tree(a, b, p, abort, on_progress); break;
    case Op::Move:   r = fileops::move_path(a, b, p, abort, on_progress); break;
    }

    std::lock_guard lk(m_mutex);
    if (auto it = m_status.find(id); it != m_status.end()) {
        it->second.state = r.ok ? JobStatus::State::Done : JobStatus::State::Failed;
        it->second.error = r.error;
        if (r.ok)
            it->second.done = it->second.total;
    }
}

FileBrowserFileOperations::JobId FileBrowserFileOperations::submit_remove(std::filesystem::path target) {
    return enqueue(Op::Remove, std::move(target), {});
}

FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_copy(std::filesystem::path from, std::filesystem::path to) {
    return enqueue(Op::Copy, std::move(from), std::move(to));
}

FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_move(std::filesystem::path from, std::filesystem::path to) {
    return enqueue(Op::Move, std::move(from), std::move(to));
}

bool FileBrowserFileOperations::poll(JobId id, JobStatus &out) const {
    std::lock_guard lk(m_mutex);
    const auto      it = m_status.find(id);
    if (it == m_status.end())
        return false;
    out = it->second;
    return true;
}
