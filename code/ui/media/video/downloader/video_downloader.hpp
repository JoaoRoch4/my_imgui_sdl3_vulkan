#pragma once
#include "pch.hpp"

class ManagedThread;

/// Downloads online video/audio URLs to a local disk cache. Each job runs a
/// three-stage fallback cascade, stopping at the first success:
///   1. the system yt-dlp binary (handles YouTube, Vimeo, Twitch, … and muxes
///      separate DASH video+audio tracks into one playable file);
///   2. scripts/video_download.py, running the vendored (newer) yt-dlp;
///   3. a plain curl direct fetch, for authenticated direct-media / CDN-redirect
///      links that yt-dlp's generic extractor rejects.
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

    /// Live state of a queued or in-flight download, as reported by the running
    /// child process (yt-dlp / curl progress output).
    struct Progress {
        bool     active  = false; ///< URL is queued or currently downloading.
        double   percent = -1.0;  ///< 0..100; -1 while the total size is unknown.
        uint64_t bytes   = 0;     ///< Bytes on disk so far.
        uint64_t total   = 0;     ///< Total bytes; 0 while unknown.
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

    /// Percentage/size of a queued or in-flight download.
    /// Returns an inactive Progress for URLs that are not in the queue.
    [[nodiscard]] Progress progress(const std::string &url) const;

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

    /// Absolute path to the python fallback downloader (scripts/video_download.py
    /// at the repo root, derived from the cache dir). Empty if not found there.
    [[nodiscard]] std::filesystem::path python_fallback_script() const;

    /// Download url to target via a three-stage cascade, stopping at the first
    /// success: (1) the system yt-dlp binary, (2) scripts/video_download.py
    /// running the vendored (newer) yt-dlp, (3) a plain curl direct fetch.
    /// Deletes the partial file on failure of each attempt.
    [[nodiscard]] bool download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               ManagedThread &self);

    /// Run a child process (argv NULL-terminated), capturing its stdout+stderr
    /// into `tail` for diagnostics while polling `st` (SIGTERM on stop) and
    /// heart-beating the watchdog. Returns the exit code, or -1 if the process
    /// could not be spawned/reaped.
    [[nodiscard]] int run_child(const char *const *argv,
                                const std::stop_token &st,
                                ManagedThread &self,
                                std::string &tail);

    /// Feed one line of child output to the progress parsers, updating
    /// m_progress_percent / m_progress_total for the job being downloaded.
    /// Understands yt-dlp's `[download] 45.3% of 123.45MiB` lines (hence
    /// --newline on the command line) and curl's column progress meter.
    void parse_progress_line(std::string_view line);

    /// Parse a yt-dlp/curl size token ("74.9M", "~123.45MiB") into bytes.
    /// Returns 0 when the token is not a size (e.g. "Unknown", "--:--").
    [[nodiscard]] static uint64_t parse_size_token(std::string_view token);

    /// Reset the progress counters and point them at `url` (empty = idle).
    void reset_progress(const std::string &url);

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
    std::string                     m_progress_url;     ///< URL the two counters below describe.
    double                          m_progress_percent; ///< 0..100, -1 while unknown.
    uint64_t                        m_progress_total;   ///< Total bytes, 0 while unknown.
    std::atomic<int>                m_current_pid; ///< PID of the running yt-dlp, -1 if none.
    std::unique_ptr<ManagedThread>  m_worker;
};