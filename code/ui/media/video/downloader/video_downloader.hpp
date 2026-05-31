#pragma once
#include "pch.hpp"

struct mpv_handle;

/// Downloads online video/audio URLs to a local disk cache using the libmpv
/// stream layer (yt-dlp is used internally for YouTube, Vimeo, Twitch, etc.).
///
/// One background jthread drains a FIFO queue of pending jobs.
/// Cache filenames are stable FNV-1a hashes of the URL — a given URL is never
/// downloaded more than once.
///
/// Usage:
///   1. Call set_cache_dir() once at startup to set the storage directory and
///      start the worker thread.
///   2. Call get_or_enqueue() when the user opens a video URL.
///      - Returns the cached path immediately if a complete file already exists.
///      - Otherwise enqueues a background download and returns std::nullopt.
///   3. Call take_completed() each frame to collect finished downloads.
///   4. Call shutdown() before destroying the object (also called by destructor).
class VideoDownloader {
public:
    struct Result {
        std::string           url;
        std::filesystem::path cached_path; ///< Valid only when ok == true.
        bool                  ok;
    };

    VideoDownloader();
    ~VideoDownloader();

    VideoDownloader(const VideoDownloader &)            = delete;
    VideoDownloader &operator=(const VideoDownloader &) = delete;

    /// Set cache directory and start the worker thread.
    void set_cache_dir(const std::filesystem::path &dir);

    /// Return cached path immediately if it exists, otherwise enqueue
    /// a background download (idempotent) and return nullopt.
    [[nodiscard]] std::optional<std::filesystem::path>
    get_or_enqueue(const std::string &url);

    /// Drain and return all downloads completed since the last call.
    std::vector<Result> take_completed();

    /// Bytes written so far for a currently in-flight URL; 0 if not queued.
    [[nodiscard]] uint64_t bytes_inflight(const std::string &url) const;

    /// Cancel active/queued downloads and remove cached files from disk.
    void clear_cache();

    /// Cancel any in-progress download and stop the worker thread.
    void shutdown();

private:
    struct Job {
        std::string           url;
        std::filesystem::path target;
    };

    void worker(std::stop_token st);

    // Worker entry point — runs on m_worker jthread
    void worker_loop(std::stop_token st, uint64_t watch_id);

    void start_worker_thread();
    void stop_worker_thread(bool unregister_watch);

    [[nodiscard]] std::filesystem::path cache_path_for(const std::string &url) const;

    /// Download url to target via mpv stream-dump. Deletes partial file on failure.
    [[nodiscard]] bool download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               uint64_t watch_id);

    static uint64_t fnv1a(const std::string &s);

    std::filesystem::path     m_cache_dir;
    std::jthread              m_worker;
    mutable std::mutex        m_mutex;
    std::condition_variable   m_cv;
    std::vector<Job>          m_queue;
    std::vector<Result>       m_completed;
    std::vector<std::string>  m_inflight; ///< URLs queued or currently downloading.
    std::atomic<mpv_handle *> m_current_mpv;
    std::atomic<uint64_t>     m_worker_watch_id;
};