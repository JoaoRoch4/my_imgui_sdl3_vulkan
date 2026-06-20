#include "pch.hpp"

#include "video_downloader.hpp"
#include "core/log/debug_log.hpp"
#include "managed_thread.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint64_t VideoDownloader::fnv1a(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::filesystem::path VideoDownloader::cache_path_for(const std::string &url) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a(url)));
    return m_cache_dir / (std::string(hex) + ".mp4");
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VideoDownloader::VideoDownloader()
    : m_cache_dir{}
    , m_mutex{}
    , m_cv{}
    , m_queue{}
    , m_completed{}
    , m_inflight{}
    , m_current_pid{-1}
    , m_worker{} {}

VideoDownloader::~VideoDownloader() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void VideoDownloader::set_cache_dir(const std::filesystem::path &dir) {
    m_cache_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    APP_DEBUG_LOG("[VideoDownloader] cache dir: {}", dir.string());
    start_worker_thread();
}

void VideoDownloader::shutdown() {
    stop_worker_thread();
}

void VideoDownloader::start_worker_thread() {
    if (m_worker)
        return;

    // KillOnly: a hung download is killed via request_stop (the download loop polls
    // the stop_token and quits mpv), and the worker stays unwatched until restarted.
    ManagedThread::Config cfg;
    cfg.name    = "VideoDownloader";
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    m_worker    = std::make_unique<ManagedThread>(
        cfg, [this](const std::stop_token &st, ManagedThread &self) { worker_iteration(st, self); });
}

void VideoDownloader::stop_worker_thread() {
    if (!m_worker)
        return;

    m_worker->request_stop();
    m_cv.notify_all();

    // Interrupt any in-flight yt-dlp so the join returns promptly instead of
    // waiting out a long download (the download loop also polls the stop_token).
    if (const pid_t pid = m_current_pid.load(std::memory_order_acquire); pid > 0)
        ::kill(pid, SIGTERM);

    m_worker.reset(); // ManagedThread destructor joins
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path>
VideoDownloader::get_or_enqueue(const std::string &url) {
    if (m_cache_dir.empty())
        return std::nullopt;

    const auto path = cache_path_for(url);

    // Return cached file immediately if it exists and is non-empty.
    std::error_code ec;
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::file_size(path, ec) > 0) {
        APP_DEBUG_LOG("[VideoDownloader] cache hit: {}", path.string());
        return std::filesystem::absolute(path, ec);
    }

    // Enqueue if not already in-flight.
    std::lock_guard lock{m_mutex};
    const bool already =
        std::any_of(m_inflight.begin(), m_inflight.end(),
                    [&url](const std::string &u) { return u == url; });
    if (!already) {
        APP_DEBUG_LOG("[VideoDownloader] enqueue: {}", url);
        m_inflight.push_back(url);
        m_queue.push_back(Job{url, path});
        m_cv.notify_one();
    }
    return std::nullopt;
}

std::vector<VideoDownloader::Result> VideoDownloader::take_completed() {
    std::lock_guard lock{m_mutex};
    return std::exchange(m_completed, {});
}

uint64_t VideoDownloader::bytes_inflight(const std::string &url) const {
    std::lock_guard lock{m_mutex};
    const bool in_flight =
        std::any_of(m_inflight.begin(), m_inflight.end(),
                    [&url](const std::string &u) { return u == url; });
    if (!in_flight)
        return 0;

    std::error_code ec;
    const auto sz = std::filesystem::file_size(cache_path_for(url), ec);
    return ec ? 0 : sz;
}

void VideoDownloader::clear_cache() {
    if (m_cache_dir.empty())
        return;

    // Interrupt any active download.
    if (const pid_t pid = m_current_pid.load(std::memory_order_acquire); pid > 0)
        ::kill(pid, SIGTERM);

    {
        std::lock_guard lock{m_mutex};
        m_queue.clear();
        m_inflight.clear();
        m_completed.clear();
    }

    std::error_code ec;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             m_cache_dir,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec))
            std::filesystem::remove(entry.path(), ec);
    }
}

// ---------------------------------------------------------------------------
// Download (runs on worker thread)
// ---------------------------------------------------------------------------

std::string VideoDownloader::ytdl_format() {
    // Quality/codec policy. yt-dlp merges the chosen video+audio into one mp4.
    //   - prefer an mp4-container video ≤1080p plus m4a audio (h264/aac-friendly,
    //     the most broadly decodable result);
    //   - then any ≤1080p mp4 single file; then any ≤1080p; then absolute best.
    // Tighten the first clause to `[vcodec^=avc1]` to force H.264 over AV1/VP9 if
    // the player struggles with newer codecs.
    return "bestvideo[ext=mp4][height<=1080]+bestaudio[ext=m4a]"
           "/best[ext=mp4][height<=1080]"
           "/best[height<=1080]"
           "/best";
}

bool VideoDownloader::download_succeeded(int exit_code,
                                         const std::filesystem::path &target) {
    // Success policy: yt-dlp must have exited cleanly AND left a non-trivial file.
    // The exit code alone is not enough — the previous stream-dump path "succeeded"
    // while writing 0 bytes, so we require the file to actually exist with content.
    if (exit_code != 0)
        return false;

    std::error_code ec;
    const auto size = std::filesystem::file_size(target, ec);
    return !ec && size > 0;
}

bool VideoDownloader::download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               ManagedThread &self) {
    // Run yt-dlp directly. It downloads AND muxes separate DASH video/audio tracks
    // into one playable file — something mpv's --stream-dump cannot do: ytdl_hook
    // hands mpv an edl:// pseudo-stream (no raw bytes), so stream-dump silently
    // wrote 0-byte files. See systematic-debugging trace 2026-06-20.
    std::error_code ec;
    std::filesystem::remove(target, ec); // clear any stale 0-byte remnant

    // Pipe captures yt-dlp's stdout+stderr so failures are logged (not swallowed)
    // and so the parent has something to poll while keeping the watchdog alive.
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        APP_DEBUG_LOG("[VideoDownloader] pipe() failed: {}", std::strerror(errno));
        return false;
    }

    const std::string fmt = ytdl_format();
    const std::string out = target.string();
    APP_DEBUG_LOG("[VideoDownloader] download start: {} -> {}", url, out);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        APP_DEBUG_LOG("[VideoDownloader] fork() failed: {}", std::strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr into the pipe, then exec yt-dlp.
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        const char *argv[] = {"yt-dlp", "--no-warnings", "--no-playlist",
                              "--merge-output-format", "mp4",
                              "-f", fmt.c_str(),
                              "-o", out.c_str(),
                              "--", url.c_str(), nullptr};
        ::execvp("yt-dlp", const_cast<char *const *>(argv));
        _exit(127); // exec failed — yt-dlp not on PATH
    }

    // Parent.
    ::close(pipefd[1]);
    m_current_pid.store(pid, std::memory_order_release);

    std::string tail;       // keep the last slice of output for failure diagnostics
    bool        termed = false;
    for (;;) {
        if (st.stop_requested() && !termed) {
            ::kill(pid, SIGTERM);
            termed = true;
        }

        pollfd pfd{pipefd[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 300); // 300 ms cadence: services stop + heartbeat
        self.heartbeat();

        if (pr > 0 && (pfd.revents & POLLIN) != 0) {
            std::array<char, 4096> buf{};
            const ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                tail.append(buf.data(), static_cast<std::size_t>(n));
                if (tail.size() > 8192)
                    tail.erase(0, tail.size() - 8192);
            }
        }

        int         status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped != pid)
            continue; // still running

        m_current_pid.store(-1, std::memory_order_release);
        ::close(pipefd[0]);

        const int  code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        const bool ok   = download_succeeded(code, target) && !st.stop_requested();
        if (!ok) {
            std::filesystem::remove(target, ec);
            APP_DEBUG_LOG("[VideoDownloader] download FAILED (exit={}): {}\n{}", code,
                          url, tail);
        } else {
            APP_DEBUG_LOG("[VideoDownloader] download OK: {}", out);
        }
        return ok;
    }
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

void VideoDownloader::worker_iteration(const std::stop_token &st, ManagedThread &self) {
    Job job;
    {
        std::unique_lock lock{m_mutex};
        m_cv.wait_for(lock, std::chrono::milliseconds(500), [this, &st] {
            return st.stop_requested() || !m_queue.empty();
        });

        // Heartbeat after waking — wait_for can hold for up to 500 ms.
        self.heartbeat();

        if (st.stop_requested() || m_queue.empty())
            return;

        job = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
    }

    APP_DEBUG_LOG("[VideoDownloader] worker: picked up job: {}", job.url);
    const bool ok = download(job.url, job.target, st, self);

    std::lock_guard lock{m_mutex};
    m_inflight.erase(std::remove(m_inflight.begin(), m_inflight.end(), job.url), m_inflight.end());
    m_completed.push_back({job.url, ok ? job.target : std::filesystem::path{}, ok});
    APP_DEBUG_LOG("[VideoDownloader] worker: completed (ok={}) {}", ok, job.url);
}