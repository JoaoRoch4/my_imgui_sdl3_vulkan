#include "pch.hpp"

#include "file_browser_thumbnail_context.hpp"

#include "rendering/vulkan/vulkan_texture.hpp"
#include "rendering/vulkan/vulkan_context.hpp"
#include "video_player.hpp"

namespace {
std::uint64_t fnv1a_hash(const std::string& s) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}
} // namespace

FileBrowserThumbnailContext::FileBrowserThumbnailContext() = default;
FileBrowserThumbnailContext::~FileBrowserThumbnailContext() { shutdown(); }

bool FileBrowserThumbnailContext::is_thumbnailable(const std::filesystem::path& p) {
    return FileBrowserThumbnailThread::is_image_ext(p) || VideoPlayer::is_video_path(p);
}

std::string FileBrowserThumbnailContext::key_for(const std::filesystem::path& path) {
    // Cheap, no filesystem syscall (paths from the listing are already absolute).
    return path.lexically_normal().string();
}

std::filesystem::path FileBrowserThumbnailContext::png_for(const std::filesystem::path& file) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a_hash(key_for(file))));
    return m_thumb_dir / (std::string(hex) + ".png");
}

void FileBrowserThumbnailContext::setup(vulkan_context* vk, std::filesystem::path thumb_dir) {
    m_vk = vk;
    m_thumb_dir = std::move(thumb_dir);
    m_setup = true;
    // A texture last drawn in frame N is safe to free only after the GPU finishes
    // the ImageCount in-flight frames cycle; +1 for margin. Replaces the old
    // evict() vkDeviceWaitIdle full-pipeline stall with a non-stalling delay.
    if (vk)
        m_retire_frames = static_cast<int>(vk->main_window_data.ImageCount) + 1;
    std::error_code ec;
    std::filesystem::create_directories(m_thumb_dir, ec);
    m_thread.start([this](const std::string& key, std::vector<std::uint8_t> rgba, bool ok) {
        on_generated(key, std::move(rgba), ok);
    });
}

void FileBrowserThumbnailContext::shutdown() {
    m_thread.shutdown(); // join workers before touching m_entries / Vulkan
    m_setup = false;
    if (!m_vk) {
        m_entries.clear();
        m_retire.clear();
        return;
    }
    std::lock_guard lock(m_mutex);
    for (auto& [k, e] : m_entries)
        if (e.texture && e.texture->is_loaded())
            e.texture->unload(*m_vk);
    for (auto& r : m_retire)
        if (r.texture && r.texture->is_loaded())
            r.texture->unload(*m_vk);
    m_entries.clear();
    m_retire.clear();
    m_vk = nullptr;
}

void FileBrowserThumbnailContext::new_frame() {
    m_uploads_this_frame = 0;
    if (!m_vk)
        return;
    std::lock_guard lock(m_mutex);
    for (auto it = m_retire.begin(); it != m_retire.end();) {
        if (--it->frames_left <= 0) {
            if (it->texture && it->texture->is_loaded())
                it->texture->unload(*m_vk);
            it = m_retire.erase(it);
        } else {
            ++it;
        }
    }
}

void FileBrowserThumbnailContext::on_generated(const std::string& key,
                                               std::vector<std::uint8_t> rgba, bool ok) {
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (ok && !rgba.empty()) {
        it->second.pixels = std::move(rgba);
        it->second.state = State::PixelsReady;
    } else {
        it->second.state = State::Failed;
    }
}

ImTextureID FileBrowserThumbnailContext::get(const std::filesystem::path& path) {
    if (!m_setup || !m_vk)
        return 0;
    // Gate non-media here (the retired provider lambda did this before calling the
    // cache). Without it, browsing .cpp/.txt files would each spawn a 13s mpv decode.
    if (!is_thumbnailable(path))
        return 0;
    const std::string key = key_for(path);

    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        Entry e;
        e.png_path = png_for(path);
        std::error_code ec;
        e.state = (std::filesystem::exists(e.png_path, ec) && !ec) ? State::DiskReady : State::Queued;
        it = m_entries.emplace(key, std::move(e)).first;
        if (it->second.state == State::Queued) {
            it->second.state = State::Generating;
            m_thread.submit(key, path, it->second.png_path,
                            FileBrowserThumbnailThread::is_image_ext(path));
        }
    }

    Entry& e = it->second;

    // Budgeted GPU upload (render thread).
    if ((e.state == State::PixelsReady || e.state == State::DiskReady)
        && m_uploads_this_frame < k_max_uploads_per_frame) {
        e.texture = std::make_unique<VulkanTexture>();
        bool ok = false;
        if (e.state == State::PixelsReady) {
            ok = e.texture->load_from_rgba(e.pixels, FileBrowserThumbnailThread::k_thumb_w,
                                           FileBrowserThumbnailThread::k_thumb_h, *m_vk);
            e.pixels.clear();
            e.pixels.shrink_to_fit();
        } else {
            ok = e.texture->load(e.png_path, *m_vk);
        }
        ++m_uploads_this_frame;
        e.state = ok ? State::Ready : State::Failed;
        if (!ok)
            e.texture.reset();
    }

    if (e.state == State::Ready && e.texture)
        return e.texture->imgui_id();
    return 0;
}

void FileBrowserThumbnailContext::evict(const std::filesystem::path& path) {
    if (!m_setup)
        return;
    const std::string key = key_for(path);
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (it->second.texture)
        m_retire.push_back({std::move(it->second.texture), m_retire_frames}); // deferred free
    if (!it->second.png_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(it->second.png_path, ec);
    }
    m_entries.erase(it);
}

void FileBrowserThumbnailContext::clear() {
    if (!m_setup)
        return;
    m_thread.clear_pending();
    std::lock_guard lock(m_mutex);
    for (auto& [k, e] : m_entries) {
        if (e.texture)
            m_retire.push_back({std::move(e.texture), m_retire_frames});
        if (!e.png_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(e.png_path, ec);
        }
    }
    m_entries.clear();
}
