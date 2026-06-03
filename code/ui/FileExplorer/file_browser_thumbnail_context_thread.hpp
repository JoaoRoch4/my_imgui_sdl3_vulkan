#pragma once

#include "pch.hpp"

#include <condition_variable>
#include <deque>
#include <stop_token>

/// Persistent off-thread thumbnail decode pool for the file browser.
///
/// Image jobs (stb/webp) and video jobs (libmpv SW render) run in separate
/// worker pools so a slow video decode never blocks image thumbnails. Each
/// finished job invokes the DoneFn with the decoded RGBA buffer (k_thumb_w x
/// k_thumb_h x 4) so the context can upload it without a disk round-trip; the
/// worker also writes the PNG for cross-session persistence. Each in-flight job
/// holds a per-job ThreadOverwatch watch (KillOnly); idle workers hold none.
///
/// No Vulkan, no ImGui — generator threads must never touch the GPU.
class FileBrowserThumbnailThread {
public:
    /// Invoked on a worker thread when a job finishes. `rgba` is empty + ok=false
    /// on failure. The callback must take its own locks; it runs off the render thread.
    using DoneFn = std::function<void(const std::string& key,
                                      std::vector<std::uint8_t> rgba, bool ok)>;

    FileBrowserThumbnailThread();
    ~FileBrowserThumbnailThread();

    FileBrowserThumbnailThread(const FileBrowserThumbnailThread&)            = delete;
    FileBrowserThumbnailThread& operator=(const FileBrowserThumbnailThread&) = delete;

    void start(DoneFn on_done);                              // spawn the pools
    void submit(std::string key, std::filesystem::path file,
                std::filesystem::path out_png, bool is_image);
    void clear_pending();                                    // drop queued (not in-flight) jobs
    void shutdown();                                         // stop + join all workers

    [[nodiscard]] static bool is_image_ext(const std::filesystem::path& p);

    /// On-disk + in-memory thumbnail dimensions (pixels).
    static constexpr int k_thumb_w = 320;
    static constexpr int k_thumb_h = 180;

    static constexpr int k_image_workers = 4; // matches retired FileThumbnailCache
    static constexpr int k_video_workers = 2;

private:
    struct Job {
        std::string           key;
        std::filesystem::path file;
        std::filesystem::path out_png;
    };

    void worker_loop(std::stop_token st, bool image_pool);
    /// Decode `file` to k_thumb_w x k_thumb_h RGBA, write `out_png`, return pixels.
    /// Empty vector on failure. Honours st + the per-job abort flag; heartbeats
    /// `watch_id` so a long (but progressing) video decode never trips the watchdog.
    std::vector<std::uint8_t> generate(const std::filesystem::path& file,
                                       const std::filesystem::path& out_png,
                                       const std::stop_token& st,
                                       std::uint64_t watch_id,
                                       const std::atomic<bool>& abort);

    std::mutex                  m_mutex;
    std::condition_variable_any m_cv;
    std::deque<Job>             m_image_queue;
    std::deque<Job>             m_video_queue;
    DoneFn                      m_on_done;
    std::vector<std::jthread>   m_workers;
};
