#pragma once
#include "pch.hpp"

class ManagedThread;

/// Downloads online video/audio URLs to a local disk cache by shelling out to
/// yt-dlp (handles YouTube, Vimeo, Twitch, etc., and muxes separate DASH
/// video+audio tracks into one playable file).
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

    // One iteration of the worker loop — runs on the ManagedThread.
    void worker_iteration(const std::stop_token &st, ManagedThread &self);

    void start_worker_thread();
    void stop_worker_thread();

    [[nodiscard]] std::filesystem::path cache_path_for(const std::string &url) const;

    /// Download url to target by running yt-dlp. Deletes partial file on failure.
    [[nodiscard]] bool download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               ManagedThread &self);

    /// yt-dlp -f format string (quality/codec policy — see .cpp).
    [[nodiscard]] static std::string ytdl_format();

    /// Did the finished yt-dlp run actually leave a usable file? (success policy)
    [[nodiscard]] static bool download_succeeded(int exit_code,
                                                 const std::filesystem::path &target);

    static uint64_t fnv1a(const std::string &s);

    std::filesystem::path           m_cache_dir;
    mutable std::mutex              m_mutex;
    std::condition_variable         m_cv;
    std::vector<Job>                m_queue;
    std::vector<Result>             m_completed;
    std::vector<std::string>        m_inflight; ///< URLs queued or currently downloading.
    std::atomic<int>                m_current_pid; ///< PID of the running yt-dlp, -1 if none.
    std::unique_ptr<ManagedThread>  m_worker;
};