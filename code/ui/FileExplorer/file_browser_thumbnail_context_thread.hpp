#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

class ManagedThread;

// Dedicated single worker that renders ONE representative frame of a video file via
// libmpv (software render) into a k_thumb_w x k_thumb_h RGBA buffer, for use as a
// file-browser thumbnail. It is a ManagedThread (RecoveryPolicy::KillOnly) so it is
// registered with ThreadOverwatch and heartbeats during the (potentially multi-second)
// mpv wait loops.
//
// IMAGE thumbnails do NOT go here — FileBrowserThumbnailContext submits those to the
// shared ImageJobSystem pool. This worker only handles the mpv video path, which can't
// run on that pool. Results are delivered via the on_done callback, invoked on the
// worker thread; the render-thread context drains them on its frame boundary.
class FileBrowserThumbnailThread {
public:
    // (key, rgba = k_thumb_w*k_thumb_h*4 bytes on success / empty on failure, ok).
    // Invoked on the worker thread — the callback must be cheap and thread-safe.
    using DoneFn = std::function<void(const std::string &key,
                                      std::vector<std::uint8_t> rgba, bool ok)>;

    FileBrowserThumbnailThread();
    ~FileBrowserThumbnailThread();
    FileBrowserThumbnailThread(const FileBrowserThumbnailThread &)            = delete;
    FileBrowserThumbnailThread &operator=(const FileBrowserThumbnailThread &) = delete;

    void start(DoneFn on_done); // spawn the worker
    void shutdown();            // stop + join

    // Queue a video file for thumbnail generation. out_png is written for persistence.
    void submit(std::string key, std::filesystem::path file, std::filesystem::path out_png);
    void clear_pending(); // drop queued (not in-flight) jobs

    static constexpr int k_thumb_w = 320;
    static constexpr int k_thumb_h = 180;

    // Number of parallel mpv render workers. Each pulls independently from the shared
    // queue with its own mpv instance, so a folder of videos generates several thumbnails
    // at once instead of serially. Kept modest because each worker is an NVDEC session
    // (consumer GPUs cap concurrent decode sessions) and a live mpv+render context.
    static constexpr int k_worker_count = 4;

private:
    struct Job {
        std::string           key;
        std::filesystem::path file;
        std::filesystem::path out_png;
    };

    void worker_iteration(const std::stop_token &st, ManagedThread &self);
    // Render one frame into out_rgba (k_thumb_w*k_thumb_h*4) and write out_png.
    bool render_video(const std::filesystem::path &file, const std::filesystem::path &out_png,
                      std::vector<std::uint8_t> &out_rgba, const std::stop_token &st,
                      ManagedThread &self);

    std::mutex                     m_mutex;
    std::condition_variable_any    m_cv;
    std::deque<Job>                m_queue;
    std::atomic<bool>              m_stopping{false};
    DoneFn                         m_on_done;
    std::vector<std::unique_ptr<ManagedThread>> m_workers;
};
