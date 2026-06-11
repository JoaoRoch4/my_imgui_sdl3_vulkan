#include "pch.hpp" // NOLINT

#include "file_browser_thumbnail_context_thread.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include <stb_image_write.h>

#include "managed_thread.hpp"
#include "thread_overwatch.hpp"

FileBrowserThumbnailThread::FileBrowserThumbnailThread()  = default;
FileBrowserThumbnailThread::~FileBrowserThumbnailThread() { shutdown(); }

void FileBrowserThumbnailThread::start(DoneFn on_done) {
    if (m_worker)
        return;
    m_on_done = std::move(on_done);
    m_stopping.store(false, std::memory_order_relaxed);

    ManagedThread::Config cfg;
    cfg.name    = "FbThumbVideo";
    cfg.timeout = std::chrono::milliseconds(30'000); // mpv render can take several s
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    cfg.watch   = true;

    m_worker = std::make_unique<ManagedThread>(
        cfg, [this](const std::stop_token &st, ManagedThread &self) { worker_iteration(st, self); });
}

void FileBrowserThumbnailThread::shutdown() {
    if (!m_worker)
        return;
    m_stopping.store(true, std::memory_order_relaxed);
    m_cv.notify_all();
    m_worker->request_stop();
    m_worker.reset(); // joins
    {
        std::lock_guard lk(m_mutex);
        m_queue.clear();
    }
    m_on_done = nullptr;
}

void FileBrowserThumbnailThread::submit(std::string key, std::filesystem::path file,
                                        std::filesystem::path out_png) {
    {
        std::lock_guard lk(m_mutex);
        m_queue.push_back({std::move(key), std::move(file), std::move(out_png)});
    }
    m_cv.notify_one();
}

void FileBrowserThumbnailThread::clear_pending() {
    std::lock_guard lk(m_mutex);
    m_queue.clear();
}

void FileBrowserThumbnailThread::worker_iteration(const std::stop_token &st, ManagedThread &self) {
    Job job;
    {
        std::unique_lock lk(m_mutex);
        m_cv.wait_for(lk, st, std::chrono::milliseconds(15'000), [&] {
            return m_stopping.load(std::memory_order_relaxed) || !m_queue.empty() ||
                   st.stop_requested();
        });
        if (m_stopping.load(std::memory_order_relaxed) || st.stop_requested() || m_queue.empty())
            return;
        job = std::move(m_queue.front());
        m_queue.pop_front();
    }

    self.heartbeat();
    std::vector<std::uint8_t> rgba;
    const bool                ok = render_video(job.file, job.out_png, rgba, st, self);
    if (m_on_done)
        m_on_done(job.key, std::move(rgba), ok);
}

bool FileBrowserThumbnailThread::render_video(const std::filesystem::path &file,
                                              const std::filesystem::path &out_png,
                                              std::vector<std::uint8_t>   &out_rgba,
                                              const std::stop_token &st, ManagedThread &self) {
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return false;

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "mute", "yes");
    mpv_set_option_string(mpv, "hwdec", "no");
    mpv_set_option_string(mpv, "loop-file", "no");
    mpv_set_option_string(mpv, "cache", "no");
    mpv_set_option_string(mpv, "ytdl", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "msg-level", "all=no");

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        return false;
    }

    std::atomic<bool> frame_ready{false};

    mpv_render_param init_params[] = {
        {MPV_RENDER_PARAM_API_TYPE,
         static_cast<void *>(const_cast<char *>(MPV_RENDER_API_TYPE_SW))},
        {MPV_RENDER_PARAM_INVALID, nullptr}};

    mpv_render_context *render_ctx = nullptr;
    if (mpv_render_context_create(&render_ctx, mpv, init_params) < 0) {
        mpv_terminate_destroy(mpv);
        return false;
    }

    mpv_render_context_set_update_callback(
        render_ctx, [](void *ctx) { static_cast<std::atomic<bool> *>(ctx)->store(true); },
        &frame_ready);

    const std::string path_str = file.string();
    const char       *cmd[]    = {"loadfile", path_str.c_str(), nullptr};
    mpv_command(mpv, cmd);

    // Phase 1: wait for VIDEO_RECONFIG (file opened).
    bool got_reconfig = false;
    auto deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        self.heartbeat();
        mpv_event *ev = mpv_wait_event(mpv, 0.05);
        if (!ev)
            break;
        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) { got_reconfig = true; break; }
        if (ev->event_id == MPV_EVENT_END_FILE) break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
    }

    if (!got_reconfig || st.stop_requested()) {
        mpv_render_context_free(render_ctx);
        mpv_terminate_destroy(mpv);
        return false;
    }

    // Seek to ~35 % of duration (if the file is long enough).
    double duration = 0.0;
    if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 && duration > 5.0) {
        const double      seek_pos    = duration * 0.35;
        const std::string pos_str     = std::to_string(seek_pos);
        const char       *seek_cmd[]  = {"seek", pos_str.c_str(), "absolute", nullptr};
        mpv_command(mpv, seek_cmd);
    }

    mpv_set_property_string(mpv, "pause", "no");

    // Phase 2: wait for at least one rendered frame.
    bool rendered = false;
    out_rgba.assign(static_cast<size_t>(k_thumb_w) * k_thumb_h * 4, 0);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        self.heartbeat();
        {
            mpv_event *ev = mpv_wait_event(mpv, 0.0);
            if (ev && ev->event_id == MPV_EVENT_END_FILE)
                break;
        }
        if (frame_ready.exchange(false)) {
            int    size[2] = {k_thumb_w, k_thumb_h};
            size_t stride  = static_cast<size_t>(k_thumb_w) * 4;
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_SW_SIZE, size},
                {MPV_RENDER_PARAM_SW_FORMAT, static_cast<void *>(const_cast<char *>("rgba"))},
                {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                {MPV_RENDER_PARAM_SW_POINTER, out_rgba.data()},
                {MPV_RENDER_PARAM_INVALID, nullptr}};
            if (mpv_render_context_render(render_ctx, rp) >= 0) {
                rendered = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mpv_render_context_free(render_ctx);
    mpv_terminate_destroy(mpv);

    if (rendered && !st.stop_requested()) {
        std::error_code ec;
        std::filesystem::create_directories(out_png.parent_path(), ec);
        stbi_write_png(out_png.string().c_str(), k_thumb_w, k_thumb_h, 4, out_rgba.data(),
                       k_thumb_w * 4);
        return true;
    }
    out_rgba.clear();
    return false;
}
