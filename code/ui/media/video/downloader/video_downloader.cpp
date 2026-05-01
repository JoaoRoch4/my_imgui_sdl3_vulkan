#include "video_downloader.hpp"

#include <mpv/client.h>

#include <algorithm>
#include <cstdio>
#include <print>
#include <utility>

// ============================================================================
// Helpers
// ============================================================================

uint64_t VideoDownloader::fnv1a(const std::string &s)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::filesystem::path VideoDownloader::cache_path_for(const std::string &url) const
{
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a(url)));
    return m_cache_dir / (std::string(hex) + ".mp4");
}

// ============================================================================
// Constructor / destructor
// ============================================================================

VideoDownloader::VideoDownloader()
    : m_cache_dir{}
    , m_worker{}
    , m_mutex{}
    , m_cv{}
    , m_queue{}
    , m_completed{}
    , m_inflight{}
    , m_current_mpv{nullptr}
{
}

VideoDownloader::~VideoDownloader()
{
    shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

void VideoDownloader::set_cache_dir(const std::filesystem::path &dir)
{
    m_cache_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::println("[VideoDownloader] cache dir: {}", dir.string());
    if (!m_worker.joinable())
        m_worker = std::jthread{&VideoDownloader::worker, this};
}

void VideoDownloader::shutdown()
{
    if (!m_worker.joinable())
        return;

    m_worker.request_stop();
    m_cv.notify_all();

    // Interrupt any mpv instance currently blocking in mpv_wait_event
    mpv_handle *mpv = m_current_mpv.load(std::memory_order_acquire);
    if (mpv)
        mpv_command_string(mpv, "quit");

    m_worker.join();
}

// ============================================================================
// Public API
// ============================================================================

std::optional<std::filesystem::path> VideoDownloader::get_or_enqueue(const std::string &url)
{
    if (m_cache_dir.empty())
        return std::nullopt;

    const auto path = cache_path_for(url);

    // Return cached file immediately if it exists and is non-empty
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
        std::println("[VideoDownloader] cache hit: {}", path.string());
        return path;
    }

    // Enqueue if not already in-flight
    std::lock_guard lock{m_mutex};
    const bool already =
        std::any_of(m_inflight.begin(), m_inflight.end(),
                    [&url](const std::string &u) { return u == url; });
    if (!already) {
        std::println("[VideoDownloader] enqueue: {}", url);
        m_inflight.push_back(url);
        m_queue.push_back(Job{url, path});
        m_cv.notify_one();
    }
    return std::nullopt;
}

std::vector<VideoDownloader::Result> VideoDownloader::take_completed()
{
    std::lock_guard lock{m_mutex};
    return std::exchange(m_completed, {});
}

void VideoDownloader::clear_cache()
{
    if (m_cache_dir.empty())
        return;

    // Stop current transfer so its target file is cleaned up by download().
    mpv_handle *mpv = m_current_mpv.load(std::memory_order_acquire);
    if (mpv)
        mpv_command_string(mpv, "quit");

    {
        // Drop queued/inflight bookkeeping so old jobs do not reappear.
        std::lock_guard lock{m_mutex};
        m_queue.clear();
        m_inflight.clear();
        m_completed.clear();
    }

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             m_cache_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec))
            files.push_back(entry.path());
    }
    for (const auto &path : files)
        std::filesystem::remove(path, ec);
}

// ============================================================================
// Download implementation (runs on worker thread)
// ============================================================================

bool VideoDownloader::download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               std::atomic<mpv_handle *> &current_out)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return false;

    current_out.store(mpv, std::memory_order_release);

    // Silent operation — no terminal output, no video/audio rendering
    mpv_set_option_string(mpv, "terminal",     "no");
    mpv_set_option_string(mpv, "really-quiet", "yes");
    mpv_set_option_string(mpv, "vid",          "no");
    mpv_set_option_string(mpv, "aid",          "no");

    // yt-dlp integration for streaming sites
    mpv_set_option_string(mpv, "ytdl",        "yes");
    // Prefer a single-file format (no separate video+audio merge) so that
    // stream-dump produces one complete, playable file.
    mpv_set_option_string(mpv, "ytdl-format",
                          "best[ext=mp4][height<=1080]"
                          "/best[height<=1080]"
                          "/best");

    // Write raw stream bytes to the target path
    mpv_set_option_string(mpv, "stream-dump", target.string().c_str());
    std::println("[VideoDownloader] download start: {} -> {}", url, target.string());

    if (mpv_initialize(mpv) < 0) {
        current_out.store(nullptr, std::memory_order_release);
        mpv_terminate_destroy(mpv);
        return false;
    }

    const char *cmd[] = {"loadfile", url.c_str(), nullptr};
    if (mpv_command(mpv, cmd) < 0) {
        current_out.store(nullptr, std::memory_order_release);
        mpv_terminate_destroy(mpv);
        return false;
    }

    bool success = false;
    for (;;) {
        if (st.stop_requested())
            mpv_command_string(mpv, "quit");

        mpv_event *ev = mpv_wait_event(mpv, 0.5);
        if (!ev)
            break;

        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            break;

        if (ev->event_id == MPV_EVENT_END_FILE) {
            const auto *edata = static_cast<const mpv_event_end_file *>(ev->data);
            success = (edata->reason == MPV_END_FILE_REASON_EOF) &&
                      !st.stop_requested();
            break;
        }
    }

    current_out.store(nullptr, std::memory_order_release);
    mpv_terminate_destroy(mpv);

    if (!success) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        std::println("[VideoDownloader] download FAILED: {}", url);
    } else {
        std::println("[VideoDownloader] download OK: {}", target.string());
    }

    return success;
}

// ============================================================================
// Worker thread
// ============================================================================

void VideoDownloader::worker(std::stop_token st)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [this, &st] {
                return st.stop_requested() || !m_queue.empty();
            });

            if (st.stop_requested())
                break;

            job = std::move(m_queue.front());
            m_queue.erase(m_queue.begin());
        }

        std::println("[VideoDownloader] worker: picked up job: {}", job.url);
        const bool ok = download(job.url, job.target, st, m_current_mpv);

        Result result{job.url, ok ? job.target : std::filesystem::path{}, ok};

        {
            std::lock_guard lock{m_mutex};
            m_inflight.erase(
                std::remove(m_inflight.begin(), m_inflight.end(), job.url),
                m_inflight.end());
            std::println("[VideoDownloader] worker: completed (ok={}) {}", ok, job.url);
            m_completed.push_back(std::move(result));
        }
    }
}
