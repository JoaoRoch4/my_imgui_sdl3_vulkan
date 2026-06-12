#include "pch.hpp" // NOLINT

#include "file_browser_thumbnail_context.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "image_job_system.hpp"
#include "image_ops.hpp"
#include "rendering/vulkan/vulkan_texture.hpp"

namespace {

std::uint64_t fnv1a_hash(const std::string &s) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string lower_ext(const std::filesystem::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool is_image_ext(const std::filesystem::path &p) {
    const std::string e = lower_ext(p);
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp";
}

bool is_video_ext(const std::filesystem::path &p) {
    const std::string e = lower_ext(p);
    return e == ".mp4" || e == ".mkv" || e == ".webm" || e == ".mov" || e == ".avi" ||
           e == ".m4v" || e == ".wmv" || e == ".flv" || e == ".ts" || e == ".mpg" ||
           e == ".mpeg" || e == ".m2ts" || e == ".3gp" || e == ".ogv";
}

} // namespace

FileBrowserThumbnailContext::FileBrowserThumbnailContext()  = default;
FileBrowserThumbnailContext::~FileBrowserThumbnailContext() { shutdown(); }

bool FileBrowserThumbnailContext::is_thumbnailable(const std::filesystem::path &path) {
    return is_image_ext(path) || is_video_ext(path);
}

void FileBrowserThumbnailContext::setup(vulkan_context *vk, std::filesystem::path thumb_dir) {
    m_vk        = vk;
    m_thumb_dir = std::move(thumb_dir);
    m_setup     = true;
    std::error_code ec;
    std::filesystem::create_directories(m_thumb_dir, ec);

    m_video.start([this](const std::string &key, std::vector<std::uint8_t> rgba, bool ok) {
        on_video_done(key, std::move(rgba), ok);
    });
}

void FileBrowserThumbnailContext::shutdown() {
    if (!m_setup)
        return;
    m_setup = false;

    m_video.shutdown();                              // stop + join the video worker
    img::ImageJobSystem::instance().clear_pending(); // drop queued image jobs

    if (m_vk) {
        for (auto &[k, e] : m_entries)
            if (e.texture)
                e.texture->unload(*m_vk);
        for (auto &r : m_retire)
            if (r.texture)
                r.texture->unload(*m_vk);
    }
    m_entries.clear();
    m_retire.clear();
    {
        std::lock_guard lk(m_video_mutex);
        m_video_results.clear();
    }
    m_vk = nullptr;
}

std::string FileBrowserThumbnailContext::key_for(const std::filesystem::path &path) const {
    // currentDirectory_ is already absolute, so a lexical normalize is enough — no
    // weakly_canonical() syscall on the render thread (the old cache's per-frame stall).
    return path.lexically_normal().string();
}

std::filesystem::path FileBrowserThumbnailContext::png_for(const std::string &key) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(fnv1a_hash(key)));
    return m_thumb_dir / (std::string(hex) + ".png");
}

void FileBrowserThumbnailContext::submit_image(const std::filesystem::path &file, Entry &e) {
    // One composite pool job: decode -> resize(320x180) -> encode PNG (persistence) ->
    // return the RGBA for in-memory GPU upload. encode_png takes const&, so the same
    // buffer is both written to disk and handed back (no extra copy, no re-decode).
    // A cached PNG from a prior session is just decoded (already thumbnail-sized).
    e.img_future = img::ImageJobSystem::instance().submit(
        [file, out = e.png_path]() -> ImgResult {
            std::error_code ec;
            if (std::filesystem::exists(out, ec) && !ec)
                return img::ops::decode_file(out, 4);

            auto dec = img::ops::decode_file(file, 4);
            if (!dec)
                return std::unexpected(dec.error());
            auto rz = img::ops::resize(*dec, k_thumb_w, k_thumb_h);
            if (!rz)
                return std::unexpected(rz.error());
            std::filesystem::create_directories(out.parent_path(), ec);
            (void)img::ops::encode_png(*rz, out);
            return rz;
        },
        img::Priority::Low);
    e.state = State::Generating;
}

void FileBrowserThumbnailContext::on_video_done(const std::string &key,
                                                std::vector<std::uint8_t> rgba, bool ok) {
    ImgResult res = std::unexpected(img::ImageError::DecodeFailed);
    if (ok && rgba.size() == static_cast<std::size_t>(k_thumb_w) * k_thumb_h * 4) {
        img::ImageBuffer b;
        b.width    = k_thumb_w;
        b.height   = k_thumb_h;
        b.channels = 4;
        b.data     = std::move(rgba);
        res        = std::move(b);
    }
    std::lock_guard lk(m_video_mutex);
    m_video_results.emplace_back(key, std::move(res));
}

void FileBrowserThumbnailContext::enforce_texture_cap() {
    int live = 0;
    for (auto &[k, e] : m_entries)
        if (e.texture)
            ++live;
    if (live <= k_max_live_textures)
        return;

    // Retire the least-recently-used live textures down to the cap. Visible thumbnails
    // were touched this/last frame so their last_used is newest -> never evicted here.
    std::vector<std::unordered_map<std::string, Entry>::iterator> live_its;
    live_its.reserve(static_cast<std::size_t>(live));
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (it->second.texture)
            live_its.push_back(it);
    std::sort(live_its.begin(), live_its.end(), [](const auto &a, const auto &b) {
        return a->second.last_used < b->second.last_used;
    });

    const int to_evict = live - k_max_live_textures;
    for (int i = 0; i < to_evict; ++i) {
        auto it = live_its[static_cast<std::size_t>(i)];
        m_retire.push_back({std::move(it->second.texture), k_retire_frames});
        m_entries.erase(it); // regenerated from the cached PNG on next get() if revisited
    }
}

void FileBrowserThumbnailContext::begin_frame() {
    if (!m_setup)
        return;
    ++m_frame;
    m_uploads_this_frame = 0;

    // Tick the retire queue; free textures that have aged out (GPU no longer sampling).
    for (auto it = m_retire.begin(); it != m_retire.end();) {
        if (--it->frames_left <= 0) {
            if (it->texture && m_vk)
                it->texture->unload(*m_vk);
            it = m_retire.erase(it);
        } else {
            ++it;
        }
    }

    // Drain results produced by the video worker thread.
    std::vector<std::pair<std::string, ImgResult>> results;
    {
        std::lock_guard lk(m_video_mutex);
        results.swap(m_video_results);
    }
    for (auto &[key, res] : results) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue;
        if (res && res->valid()) {
            it->second.pixels      = std::move(*res);
            it->second.have_pixels = true;
            it->second.state       = State::PixelsReady;
        } else {
            it->second.state = State::Failed;
        }
    }

    enforce_texture_cap(); // bound live textures (LRU) so huge folders can't exhaust the GPU
}

ImTextureID FileBrowserThumbnailContext::get(const std::filesystem::path &path) { // super hot MUST BE IN ITS OWN THREAD
    if (!m_setup || !is_thumbnailable(path))
        return 0; // skip non-image/-video files (replaces app_coordinator's is_thumb_path)

    const std::string key = key_for(path);
    Entry            &e   = m_entries.try_emplace(key).first->second;
    e.last_used           = m_frame; // mark touched this frame (LRU)

    if (e.state == State::Queued) {
        e.png_path = png_for(key);
        e.is_video = is_video_ext(path);
        if (e.is_video) {
            m_video.submit(key, path, e.png_path);
            e.state = State::Generating;
        } else {
            submit_image(path, e);
        }
    }

    // Image jobs deliver via a future; poll without blocking. Video jobs deliver via
    // begin_frame draining the worker's results into PixelsReady.
    if (e.state == State::Generating && !e.is_video && e.img_future.valid()) {
        if (e.img_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ImgResult res = e.img_future.get();
            if (res && res->valid()) {
                e.pixels      = std::move(*res);
                e.have_pixels = true;
                e.state       = State::PixelsReady;
            } else {
                e.state = State::Failed;
            }
        }
    }

    // Upload at most k_max_uploads_per_frame textures per frame so a fresh grid of
    // ready thumbnails can't stall a single frame.
    if (e.state == State::PixelsReady && e.have_pixels &&
        m_uploads_this_frame < k_max_uploads_per_frame) {
        auto tex = std::make_unique<VulkanTexture>();
        if (tex->upload(e.pixels, *m_vk)) {
            e.texture = std::move(tex);
            e.state   = State::Ready;
            ++m_uploads_this_frame;
        } else {
            e.state = State::Failed;
        }
        e.pixels      = img::ImageBuffer{}; // release CPU pixels either way
        e.have_pixels = false;
    }

    if (e.state == State::Ready && e.texture)
        return e.texture->imgui_id();
    return 0;
}

void FileBrowserThumbnailContext::evict(const std::filesystem::path &path) {
    if (!m_setup)
        return;
    auto it = m_entries.find(key_for(path));
    if (it == m_entries.end())
        return;
    if (it->second.texture)
        m_retire.push_back({std::move(it->second.texture), k_retire_frames});
    if (!it->second.png_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(it->second.png_path, ec);
    }
    m_entries.erase(it);
}

void FileBrowserThumbnailContext::clear() {
    if (!m_setup)
        return;
    m_video.clear_pending();
    img::ImageJobSystem::instance().clear_pending();
    std::error_code ec;
    for (auto &[k, e] : m_entries) {
        if (e.texture)
            m_retire.push_back({std::move(e.texture), k_retire_frames});
        if (!e.png_path.empty())
            std::filesystem::remove(e.png_path, ec);
    }
    m_entries.clear();
}
