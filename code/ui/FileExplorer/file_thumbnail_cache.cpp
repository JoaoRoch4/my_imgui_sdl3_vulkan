#include "pch.hpp"

#include "file_thumbnail_cache.hpp"
#include "rendering/vulkan/vulkan_texture.hpp"
#include "rendering/vulkan/vulkan_context.hpp"
#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <stb_image_write.h>

namespace {

uint64_t fnv1a_hash(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

bool is_image_ext(const std::filesystem::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp";
}

void generate_image_thumbnail(const std::filesystem::path &file,
                              const std::filesystem::path &out_png) {
    constexpr int k_channels = 4;
    int src_w = 0, src_h = 0;
    uint8_t *pixels = nullptr;
    bool is_webp = false;

    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".webp") {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return;
        std::vector<uint8_t> buf(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(std::bit_cast<char *>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
        pixels = WebPDecodeRGBA(buf.data(), buf.size(), &src_w, &src_h);
        is_webp = true;
    } else {
        int ch = 0;
        pixels = stbi_load(file.string().c_str(), &src_w, &src_h, &ch, k_channels);
    }

    if (!pixels)
        return;

    std::vector<uint8_t> dst(static_cast<size_t>(FileThumbnailCache::k_thumb_w) *
                              FileThumbnailCache::k_thumb_h * k_channels);

    stbir_resize_uint8_linear(pixels, src_w, src_h, 0,
                               dst.data(),
                               FileThumbnailCache::k_thumb_w,
                               FileThumbnailCache::k_thumb_h,
                               0, STBIR_RGBA);

    if (is_webp)
        WebPFree(pixels);
    else
        stbi_image_free(pixels);

    std::error_code ec;
    std::filesystem::create_directories(out_png.parent_path(), ec);
    stbi_write_png(out_png.string().c_str(),
                   FileThumbnailCache::k_thumb_w,
                   FileThumbnailCache::k_thumb_h,
                   k_channels,
                   dst.data(),
                   FileThumbnailCache::k_thumb_w * k_channels);
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

FileThumbnailCache::FileThumbnailCache() = default;

FileThumbnailCache::~FileThumbnailCache() {
    shutdown();
}

void FileThumbnailCache::setup(vulkan_context *vk,
                               const std::filesystem::path &thumb_dir) {
    m_vk        = vk;
    m_thumb_dir = thumb_dir;
    m_setup     = true;

    std::error_code ec;
    std::filesystem::create_directories(m_thumb_dir, ec);
}

void FileThumbnailCache::shutdown() {
    m_setup = false;

    // Move both generator thread vectors out and join them outside any lock.
    std::vector<std::jthread> img_threads;
    std::vector<std::jthread> vid_threads;
    {
        std::lock_guard glock(m_gen_mutex);
        img_threads = std::move(m_img_threads);
        vid_threads = std::move(m_vid_threads);
    }
    img_threads.clear(); // request_stop() + join
    vid_threads.clear();

    if (!m_vk)
        return;

    std::lock_guard lock(m_mutex);
    for (auto &[key, entry] : m_entries) {
        if (entry.texture && entry.texture->is_loaded())
            entry.texture->unload(*m_vk);
    }
    m_entries.clear();
    m_vk = nullptr;
}

void FileThumbnailCache::clear() {
    if (!m_setup)
        return;

    // Stop all in-flight generator threads.
    std::vector<std::jthread> img_threads;
    std::vector<std::jthread> vid_threads;
    {
        std::lock_guard glock(m_gen_mutex);
        img_threads = std::move(m_img_threads);
        vid_threads = std::move(m_vid_threads);
    }
    img_threads.clear(); // request_stop() + join
    vid_threads.clear();

    // Delete all PNGs and release GPU textures; reset counts.
    {
        std::lock_guard lock(m_mutex);
        m_active_img_generators = 0;
        m_active_vid_generators = 0;

        for (auto &[key, entry] : m_entries) {
            if (entry.texture && entry.texture->is_loaded())
                entry.texture->unload(*m_vk);
            // Delete the PNG on disk.
            if (!entry.png_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(entry.png_path, ec);
            }
        }
        m_entries.clear();
    }
}

void FileThumbnailCache::evict(const std::filesystem::path &path) {
    if (!m_setup)
        return;

    const std::string key = std::filesystem::weakly_canonical(path).string();

    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;

    Entry &entry = it->second;
    if (entry.texture && entry.texture->is_loaded()) {
        // Wait for the GPU to finish using this texture before freeing it.
        vkDeviceWaitIdle(m_vk->device);
        entry.texture->unload(*m_vk);
    }
    if (!entry.png_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(entry.png_path, ec);
    }
    m_entries.erase(it);
}

// ============================================================================
// Public get() — render thread
// ============================================================================

std::filesystem::path FileThumbnailCache::png_for(const std::filesystem::path &file) const {
    const std::string canonical = std::filesystem::weakly_canonical(file).string();
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a_hash(canonical)));
    return m_thumb_dir / (std::string(hex) + ".png");
}

ImTextureID FileThumbnailCache::get(const std::filesystem::path &path) {
    if (!m_setup)
        return 0;

    const std::string key = std::filesystem::weakly_canonical(path).string();

    std::lock_guard lock(m_mutex);

    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        Entry entry;
        entry.png_path = png_for(path);

        std::error_code ec;
        entry.state = (std::filesystem::exists(entry.png_path, ec) && !ec)
                          ? State::DiskReady
                          : State::InQueue;
        it = m_entries.emplace(key, std::move(entry)).first;
    }

    Entry &entry = it->second;

    // Spawn a generator if the entry is waiting and the matching pool has capacity.
    if (entry.state == State::InQueue) {
        const bool img = is_image_ext(path);
        const int  limit  = img ? k_max_img_generators : k_max_vid_generators;
        int       &active = img ? m_active_img_generators : m_active_vid_generators;
        if (active < limit) {
            entry.state = State::Generating;
            ++active;
            spawn_generator(path, entry.png_path, img);
        }
    }

    // Upload PNG to GPU texture on the main (render) thread.
    if (entry.state == State::DiskReady) {
        entry.texture = std::make_unique<VulkanTexture>();
        if (entry.texture->load(entry.png_path, *m_vk)) {
            entry.state = State::Ready;
        } else {
            entry.state = State::Failed;
            entry.texture.reset();
        }
    }

    if (entry.state == State::Ready && entry.texture)
        return entry.texture->imgui_id();

    return 0;
}

// ============================================================================
// Per-thumbnail generator threads
// ============================================================================

void FileThumbnailCache::spawn_generator(const std::filesystem::path &path,
                                         const std::filesystem::path &out_png,
                                         bool                         is_image) {
    // Called while m_mutex is held (lock order: m_mutex → m_gen_mutex).
    std::lock_guard glock(m_gen_mutex);
    auto &pool = is_image ? m_img_threads : m_vid_threads;
    pool.emplace_back(
        [this, path, out_png, is_image](std::stop_token st) {
            generate_thumbnail(path, out_png, std::move(st));

            // Update entry state and release the concurrency slot.
            {
                std::lock_guard elock(m_mutex);
                const std::string key =
                    std::filesystem::weakly_canonical(path).string();
                if (auto it = m_entries.find(key); it != m_entries.end()) {
                    std::error_code ec;
                    it->second.state =
                        (std::filesystem::exists(out_png, ec) && !ec)
                            ? State::DiskReady
                            : State::Failed;
                }
                int &active = is_image ? m_active_img_generators
                                       : m_active_vid_generators;
                --active;
            }
        });
}

// ============================================================================
// Thumbnail generation (generator thread — no Vulkan calls)
// ============================================================================

void FileThumbnailCache::generate_thumbnail(const std::filesystem::path &file,
                                            const std::filesystem::path &out_png,
                                            std::stop_token              st) {
    // For image files use a fast stb_image + resize path — no mpv needed.
    if (is_image_ext(file)) {
        generate_image_thumbnail(file, out_png);
        return;
    }
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return;

    mpv_set_option_string(mpv, "vo",       "libmpv");
    mpv_set_option_string(mpv, "pause",    "yes");
    mpv_set_option_string(mpv, "mute",     "yes");
    mpv_set_option_string(mpv, "hwdec",    "no");
    mpv_set_option_string(mpv, "loop-file","no");
    mpv_set_option_string(mpv, "cache",    "no");
    mpv_set_option_string(mpv, "ytdl",     "no");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "msg-level","all=no");

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        return;
    }

    std::atomic<bool> frame_ready{false};

    mpv_render_param init_params[] = {
        {MPV_RENDER_PARAM_API_TYPE,
         static_cast<void *>(const_cast<char *>(MPV_RENDER_API_TYPE_SW))},
        {MPV_RENDER_PARAM_INVALID, nullptr}};

    mpv_render_context *render_ctx = nullptr;
    if (mpv_render_context_create(&render_ctx, mpv, init_params) < 0) {
        mpv_terminate_destroy(mpv);
        return;
    }

    mpv_render_context_set_update_callback(
        render_ctx,
        [](void *ctx) { static_cast<std::atomic<bool> *>(ctx)->store(true); },
        &frame_ready);

    // Load the file.
    const std::string path_str = file.string();
    const char *cmd[] = {"loadfile", path_str.c_str(), nullptr};
    mpv_command(mpv, cmd);

    // Phase 1: wait for VIDEO_RECONFIG so we know the file has been opened.
    bool got_reconfig = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

    while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        mpv_event *ev = mpv_wait_event(mpv, 0.05);
        if (!ev) break;
        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) { got_reconfig = true; break; }
        if (ev->event_id == MPV_EVENT_END_FILE)        break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN)        break;
    }

    if (!got_reconfig || st.stop_requested()) {
        mpv_render_context_free(render_ctx);
        mpv_terminate_destroy(mpv);
        return;
    }

    // Seek to ~10 % of duration (if the file is long enough).
    double duration = 0.0;
    if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 &&
        duration > 5.0) {
        const double seek_pos = duration * 0.35;
        const std::string pos_str = std::to_string(seek_pos);
        const char *seek_cmd[] = {"seek", pos_str.c_str(), "absolute", nullptr};
        mpv_command(mpv, seek_cmd);
    }

    mpv_set_property_string(mpv, "pause", "no");

    // Phase 2: wait for at least one rendered frame.
    bool rendered = false;
    std::vector<uint8_t> buf(static_cast<size_t>(k_thumb_w) * k_thumb_h * 4);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        // Drain mpv events to keep playback progressing.
        {
            mpv_event *ev = mpv_wait_event(mpv, 0.0);
            if (ev && ev->event_id == MPV_EVENT_END_FILE) break;
        }

        if (frame_ready.exchange(false)) {
            int    size[2] = {k_thumb_w, k_thumb_h};
            size_t stride  = static_cast<size_t>(k_thumb_w) * 4;

            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_SW_SIZE,    size},
                {MPV_RENDER_PARAM_SW_FORMAT,  static_cast<void *>(const_cast<char *>("rgba"))},
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

    if (rendered && !st.stop_requested()) {
        std::error_code ec;
        std::filesystem::create_directories(out_png.parent_path(), ec);
        stbi_write_png(out_png.string().c_str(),
                       k_thumb_w, k_thumb_h, 4,
                       buf.data(),
                       k_thumb_w * 4);
    }
}
