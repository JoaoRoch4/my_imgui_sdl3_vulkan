#include "pch.hpp"

#include "file_browser_thumbnail_context_thread.hpp"

#include "core/thread/thread_overwatch.hpp"

#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <stb_image_write.h>

namespace {

bool is_image_ext_impl(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp";
}

// Decode + resize to k_thumb_w x k_thumb_h RGBA. Returns the pixels (and writes
// the PNG for persistence). Empty on failure.
std::vector<std::uint8_t> gen_image(const std::filesystem::path& file,
                                    const std::filesystem::path& out_png) {
    constexpr int k_channels = 4;
    int src_w = 0, src_h = 0;
    std::uint8_t* pixels = nullptr;
    bool is_webp = false;
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".webp") {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return {};
        std::vector<std::uint8_t> raw(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(std::bit_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        pixels = WebPDecodeRGBA(raw.data(), raw.size(), &src_w, &src_h);
        is_webp = true;
    } else {
        int ch = 0;
        pixels = stbi_load(file.string().c_str(), &src_w, &src_h, &ch, k_channels);
    }
    if (!pixels)
        return {};

    std::vector<std::uint8_t> dst(static_cast<size_t>(FileBrowserThumbnailThread::k_thumb_w) *
                                  FileBrowserThumbnailThread::k_thumb_h * k_channels);
    stbir_resize_uint8_linear(pixels, src_w, src_h, 0, dst.data(),
                              FileBrowserThumbnailThread::k_thumb_w,
                              FileBrowserThumbnailThread::k_thumb_h, 0, STBIR_RGBA);
    if (is_webp)
        WebPFree(pixels);
    else
        stbi_image_free(pixels);

    std::error_code ec;
    std::filesystem::create_directories(out_png.parent_path(), ec);
    stbi_write_png(out_png.string().c_str(), FileBrowserThumbnailThread::k_thumb_w,
                   FileBrowserThumbnailThread::k_thumb_h, k_channels, dst.data(),
                   FileBrowserThumbnailThread::k_thumb_w * k_channels);
    return dst;
}

} // namespace

bool FileBrowserThumbnailThread::is_image_ext(const std::filesystem::path& p) {
    return is_image_ext_impl(p);
}

FileBrowserThumbnailThread::FileBrowserThumbnailThread() = default;
FileBrowserThumbnailThread::~FileBrowserThumbnailThread() { shutdown(); }

void FileBrowserThumbnailThread::start(DoneFn on_done) {
    m_on_done = std::move(on_done);
    for (int i = 0; i < k_image_workers; ++i)
        m_workers.emplace_back([this](std::stop_token st) { worker_loop(std::move(st), true); });
    for (int i = 0; i < k_video_workers; ++i)
        m_workers.emplace_back([this](std::stop_token st) { worker_loop(std::move(st), false); });
}

void FileBrowserThumbnailThread::submit(std::string key, std::filesystem::path file,
                                        std::filesystem::path out_png, bool is_image) {
    {
        std::lock_guard lock(m_mutex);
        Job job{std::move(key), std::move(file), std::move(out_png)};
        // Visible-first: push front so the latest request is serviced first.
        if (is_image)
            m_image_queue.push_front(std::move(job));
        else
            m_video_queue.push_front(std::move(job));
    }
    m_cv.notify_all();
}

void FileBrowserThumbnailThread::clear_pending() {
    std::lock_guard lock(m_mutex);
    m_image_queue.clear();
    m_video_queue.clear();
}

void FileBrowserThumbnailThread::shutdown() {
    for (auto& t : m_workers)
        t.request_stop();
    m_cv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable())
            t.join();
    m_workers.clear();
}

void FileBrowserThumbnailThread::worker_loop(std::stop_token st, bool image_pool) {
    auto& queue = image_pool ? m_image_queue : m_video_queue;
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, st, [&] { return !queue.empty(); });
            if (st.stop_requested())
                break;
            job = std::move(queue.front());
            queue.pop_front();
        }

        // Per-job abort flag kept alive via shared_ptr captured BY VALUE in the
        // kill lambda. ThreadOverwatch copies kill_request and may call it AFTER
        // unwatch() returns (it invokes outside its lock), so a stack-local flag
        // would be a use-after-scope.
        auto abort = std::make_shared<std::atomic<bool>>(false);
        const std::uint64_t watch_id = ThreadOverwatch::instance().watch(
            image_pool ? "FileBrowserThumb::img" : "FileBrowserThumb::vid",
            std::chrono::milliseconds(8000),
            [abort] { abort->store(true, std::memory_order_release); }, nullptr,
            ThreadOverwatch::RecoveryPolicy::KillOnly);
        ThreadOverwatch::instance().heartbeat(watch_id);

        std::vector<std::uint8_t> rgba = generate(job.file, job.out_png, st, watch_id, *abort);

        ThreadOverwatch::instance().unwatch(watch_id);

        if (st.stop_requested())
            break;
        const bool ok = !rgba.empty();
        if (m_on_done)
            m_on_done(job.key, std::move(rgba), ok);
    }
}

std::vector<std::uint8_t> FileBrowserThumbnailThread::generate(
    const std::filesystem::path& file, const std::filesystem::path& out_png,
    const std::stop_token& st, std::uint64_t watch_id, const std::atomic<bool>& abort) {
    if (is_image_ext(file))
        return gen_image(file, out_png); // fast; watch_id/abort unused

    const auto cancelled = [&] {
        return st.stop_requested() || abort.load(std::memory_order_acquire);
    };

    mpv_handle* mpv = mpv_create();
    if (!mpv)
        return {};

    mpv_set_option_string(mpv, "vo",        "libmpv");
    mpv_set_option_string(mpv, "pause",     "yes");
    mpv_set_option_string(mpv, "mute",      "yes");
    mpv_set_option_string(mpv, "hwdec",     "no");
    mpv_set_option_string(mpv, "loop-file", "no");
    mpv_set_option_string(mpv, "cache",     "no");
    mpv_set_option_string(mpv, "ytdl",      "no");
    mpv_set_option_string(mpv, "terminal",  "no");
    mpv_set_option_string(mpv, "msg-level", "all=no");

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        return {};
    }

    std::atomic<bool> frame_ready{false};

    mpv_render_param init_params[] = {
        {MPV_RENDER_PARAM_API_TYPE,
         static_cast<void*>(const_cast<char*>(MPV_RENDER_API_TYPE_SW))},
        {MPV_RENDER_PARAM_INVALID, nullptr}};

    mpv_render_context* render_ctx = nullptr;
    if (mpv_render_context_create(&render_ctx, mpv, init_params) < 0) {
        mpv_terminate_destroy(mpv);
        return {};
    }

    mpv_render_context_set_update_callback(
        render_ctx,
        [](void* ctx) { static_cast<std::atomic<bool>*>(ctx)->store(true); },
        &frame_ready);

    const std::string path_str = file.string();
    const char* cmd[] = {"loadfile", path_str.c_str(), nullptr};
    mpv_command(mpv, cmd);

    // Phase 1: wait for VIDEO_RECONFIG so we know the file opened.
    bool got_reconfig = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        ThreadOverwatch::instance().heartbeat(watch_id);
        mpv_event* ev = mpv_wait_event(mpv, 0.05);
        if (!ev) break;
        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) { got_reconfig = true; break; }
        if (ev->event_id == MPV_EVENT_END_FILE)        break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN)        break;
    }

    if (!got_reconfig || cancelled()) {
        mpv_render_context_free(render_ctx);
        mpv_terminate_destroy(mpv);
        return {};
    }

    // Seek to ~35% of duration (if the file is long enough).
    double duration = 0.0;
    if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 &&
        duration > 5.0) {
        const double seek_pos = duration * 0.35;
        const std::string pos_str = std::to_string(seek_pos);
        const char* seek_cmd[] = {"seek", pos_str.c_str(), "absolute", nullptr};
        mpv_command(mpv, seek_cmd);
    }

    mpv_set_property_string(mpv, "pause", "no");

    // Phase 2: wait for at least one rendered frame.
    bool rendered = false;
    std::vector<std::uint8_t> buf(static_cast<size_t>(k_thumb_w) * k_thumb_h * 4);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        ThreadOverwatch::instance().heartbeat(watch_id);
        {
            mpv_event* ev = mpv_wait_event(mpv, 0.0);
            if (ev && ev->event_id == MPV_EVENT_END_FILE) break;
        }

        if (frame_ready.exchange(false)) {
            int    size[2] = {k_thumb_w, k_thumb_h};
            size_t stride  = static_cast<size_t>(k_thumb_w) * 4;

            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_SW_SIZE,    size},
                {MPV_RENDER_PARAM_SW_FORMAT,  static_cast<void*>(const_cast<char*>("rgba"))},
                {MPV_RENDER_PARAM_SW_STRIDE,  &stride},
                {MPV_RENDER_PARAM_SW_POINTER, buf.data()},
                {MPV_RENDER_PARAM_INVALID,    nullptr}};

            if (mpv_render_context_render(render_ctx, rp) >= 0) {
                rendered = true;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    mpv_render_context_free(render_ctx);
    mpv_terminate_destroy(mpv);

    if (rendered && !cancelled()) {
        std::error_code ec;
        std::filesystem::create_directories(out_png.parent_path(), ec);
        stbi_write_png(out_png.string().c_str(), k_thumb_w, k_thumb_h, 4,
                       buf.data(), k_thumb_w * 4);
        return buf;
    }
    return {};
}
