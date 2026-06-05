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
    , m_current_mpv{nullptr}
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

    // Interrupt any mpv instance currently blocking in mpv_wait_event so the join
    // returns promptly instead of waiting out the 0.5 s event poll.
    if (mpv_handle *mpv = m_current_mpv.load(std::memory_order_acquire))
        mpv_command_string(mpv, "quit");

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
    mpv_handle *mpv = m_current_mpv.load(std::memory_order_acquire);
    if (mpv)
        mpv_command_string(mpv, "quit");

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

bool VideoDownloader::download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               ManagedThread &self) {
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return false;

    m_current_mpv.store(mpv, std::memory_order_release);

    // Silent operation — no terminal output, no video/audio rendering.
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "really-quiet", "yes");
    mpv_set_option_string(mpv, "vid", "no");
    mpv_set_option_string(mpv, "aid", "no");

    // yt-dlp integration for streaming sites.
    mpv_set_option_string(mpv, "ytdl", "yes");
    // Prefer a single-file format so stream-dump produces one complete file.
    mpv_set_option_string(mpv, "ytdl-format",
                          "best[ext=mp4][height<=1080]"
                          "/best[height<=1080]"
                          "/best");

    // Write raw stream bytes to the target path.
    mpv_set_option_string(mpv, "stream-dump", target.string().c_str());
    APP_DEBUG_LOG("[VideoDownloader] download start: {} -> {}", url, target.string());

    if (mpv_initialize(mpv) < 0) {
        m_current_mpv.store(nullptr, std::memory_order_release);
        mpv_terminate_destroy(mpv);
        return false;
    }

    const char *cmd[] = {"loadfile", url.c_str(), nullptr};
    if (mpv_command(mpv, cmd) < 0) {
        m_current_mpv.store(nullptr, std::memory_order_release);
        mpv_terminate_destroy(mpv);
        return false;
    }

    bool success = false;
    for (;;) {
        if (st.stop_requested())
            mpv_command_string(mpv, "quit");

        mpv_event *ev = mpv_wait_event(mpv, 0.5);
        self.heartbeat();

        if (!ev || ev->event_id == MPV_EVENT_SHUTDOWN)
            break;

        if (ev->event_id == MPV_EVENT_END_FILE) {
            const auto *edata = static_cast<const mpv_event_end_file *>(ev->data);
            success = (edata->reason == MPV_END_FILE_REASON_EOF) && !st.stop_requested();
            break;
        }
    }

    m_current_mpv.store(nullptr, std::memory_order_release);
    mpv_terminate_destroy(mpv);

    if (!success) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        APP_DEBUG_LOG("[VideoDownloader] download FAILED: {}", url);
    } else {
        APP_DEBUG_LOG("[VideoDownloader] download OK: {}", target.string());
    }

    return success;
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