#include "pch.hpp"

#include "file_browser_file_operations.hpp"

#include "core/thread/thread_overwatch.hpp"

FileBrowserFileOperations::FileBrowserFileOperations()
    : m_worker{[this](std::stop_token st) { worker_loop(std::move(st)); }} {}

FileBrowserFileOperations::~FileBrowserFileOperations() { shutdown(); }

void FileBrowserFileOperations::shutdown() {
    m_worker.request_stop();
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

// ---- synchronous fast ops --------------------------------------------------

FileBrowserFileOperations::Result
FileBrowserFileOperations::create_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {false, ec.message()};
    return {};
}

FileBrowserFileOperations::Result
FileBrowserFileOperations::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
        return {false, ec.message()};
    return {};
}

// ---- async heavy ops -------------------------------------------------------

FileBrowserFileOperations::JobId
FileBrowserFileOperations::enqueue(Op op, std::filesystem::path from, std::filesystem::path to) {
    const JobId id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        m_progress[id] = Progress{Progress::State::Running, 0, 0, from, {}};
        m_queue.push_back(Job{id, op, std::move(from), std::move(to)});
    }
    m_cv.notify_one();
    return id;
}

FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_remove(const std::filesystem::path& target) {
    return enqueue(Op::Remove, target, {});
}
FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    return enqueue(Op::Copy, from, to);
}
FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_move(const std::filesystem::path& from, const std::filesystem::path& to) {
    return enqueue(Op::Move, from, to);
}

bool FileBrowserFileOperations::poll(JobId id, Progress& out) {
    std::lock_guard lock(m_mutex);
    auto it = m_progress.find(id);
    if (it == m_progress.end())
        return false;
    out = it->second;
    if (it->second.state != Progress::State::Running)
        m_progress.erase(it); // terminal state reported once
    return true;
}

std::uint64_t FileBrowserFileOperations::count_entries(const std::filesystem::path& root) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return 1;
    std::uint64_t n = 1; // count root itself
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

void FileBrowserFileOperations::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, st, [this] { return !m_queue.empty(); });
            if (st.stop_requested())
                break;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }

        m_kill_requested.store(false, std::memory_order_release);

        const std::uint64_t watch_id = ThreadOverwatch::instance().watch(
            "FileBrowserFileOps", std::chrono::milliseconds(10000),
            [this] { m_kill_requested.store(true, std::memory_order_release); }, nullptr,
            ThreadOverwatch::RecoveryPolicy::KillOnly);

        run_job(job, watch_id);

        ThreadOverwatch::instance().unwatch(watch_id);
    }
}

void FileBrowserFileOperations::run_job(const Job& job, std::uint64_t watch_id) {
    const auto aborted = [&] {
        return m_kill_requested.load(std::memory_order_acquire);
    };
    const auto set_progress = [&](std::uint64_t done, std::uint64_t total,
                                  const std::filesystem::path& current) {
        std::lock_guard lock(m_mutex);
        if (auto it = m_progress.find(job.id); it != m_progress.end()) {
            it->second.done = done;
            it->second.total = total;
            it->second.current = current;
        }
    };
    const auto finish = [&](bool ok, std::string err) {
        std::lock_guard lock(m_mutex);
        if (auto it = m_progress.find(job.id); it != m_progress.end()) {
            it->second.state = ok ? Progress::State::Done : Progress::State::Failed;
            it->second.error = std::move(err);
        }
    };

    std::uint64_t total = 0; // counted lazily — the move fast-path skips it entirely
    std::uint64_t done = 0;
    std::error_code ec;

    switch (job.op) {
    case Op::Remove: {
        total = count_entries(job.from);
        // Recursive, entry-by-entry so we can heartbeat + honour aborts.
        if (std::filesystem::is_directory(job.from, ec)) {
            std::vector<std::filesystem::path> entries;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     job.from, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                entries.push_back(it->path());
            }
            // Remove deepest-first so directories are empty when removed.
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.string().size() > b.string().size(); });
            for (const auto& p : entries) {
                if (aborted()) { finish(false, "operation aborted"); return; }
                std::filesystem::remove(p, ec);
                ThreadOverwatch::instance().heartbeat(watch_id);
                set_progress(++done, total, p);
            }
        }
        std::filesystem::remove(job.from, ec);
        if (ec) { finish(false, ec.message()); return; }
        finish(true, {});
        return;
    }
    case Op::Copy:
    case Op::Move: {
        // Move fast-path: same-filesystem rename is atomic + instant.
        if (job.op == Op::Move) {
            std::error_code rec;
            std::filesystem::rename(job.from, job.to, rec);
            if (!rec) { finish(true, {}); return; } // same-device: instant, no tree walk
            // else fall through to copy-then-remove (cross-device).
        }
        total = count_entries(job.from); // only reached for a copy or cross-device move
        if (std::filesystem::is_directory(job.from, ec)) {
            std::filesystem::create_directories(job.to, ec);
            for (auto it = std::filesystem::recursive_directory_iterator(
                     job.from, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (aborted()) { finish(false, "operation aborted"); return; }
                const auto rel = std::filesystem::relative(it->path(), job.from, ec);
                const auto dst = job.to / rel;
                std::error_code cec;
                if (it->is_directory())
                    std::filesystem::create_directories(dst, cec);
                else
                    std::filesystem::copy_file(it->path(), dst,
                        std::filesystem::copy_options::overwrite_existing, cec);
                if (cec) { finish(false, cec.message()); return; }
                ThreadOverwatch::instance().heartbeat(watch_id);
                set_progress(++done, total, it->path());
            }
        } else {
            std::filesystem::copy_file(job.from, job.to,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { finish(false, ec.message()); return; }
            set_progress(++done, total, job.from);
        }
        if (job.op == Op::Move) {
            std::error_code rmec;
            std::filesystem::remove_all(job.from, rmec); // cross-device move cleanup
        }
        finish(true, {});
        return;
    }
    }
}
