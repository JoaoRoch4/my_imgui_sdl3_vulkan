#pragma once

#include "pch.hpp"

#include "file_browser_thumbnail_context_thread.hpp"

class vulkan_context;
class VulkanTexture;

/// Render-thread thumbnail cache + GPU upload for the file browser.
///
/// Owns a FileBrowserThumbnailThread. get() returns a ready ImTextureID or 0 while
/// the thumbnail is being generated/uploaded. Anti-lag: cheap lexical key (no
/// per-frame syscall), an upload budget (k_max_uploads_per_frame), and deferred-free
/// eviction (no vkDeviceWaitIdle). setup()/get()/evict()/clear()/shutdown() are
/// render-thread only (they touch Vulkan).
class FileBrowserThumbnailContext {
public:
    FileBrowserThumbnailContext();
    ~FileBrowserThumbnailContext();

    FileBrowserThumbnailContext(const FileBrowserThumbnailContext&)            = delete;
    FileBrowserThumbnailContext& operator=(const FileBrowserThumbnailContext&) = delete;

    void setup(vulkan_context* vk, std::filesystem::path thumb_dir);
    void shutdown();
    [[nodiscard]] bool is_setup() const noexcept { return m_setup; }

    /// Top of Display(): reset the per-frame upload budget + free retired textures.
    void new_frame();

    [[nodiscard]] ImTextureID get(const std::filesystem::path& path);
    void evict(const std::filesystem::path& path);
    void clear();

    /// True for the media types the browser shows thumbnails for (images + videos).
    [[nodiscard]] static bool is_thumbnailable(const std::filesystem::path& p);

    static constexpr int k_max_uploads_per_frame = 4;

private:
    enum class State { Queued, Generating, PixelsReady, DiskReady, Ready, Failed };
    struct Entry {
        State                          state = State::Queued;
        std::filesystem::path          png_path;
        std::vector<std::uint8_t>      pixels;   // RGBA handed back by the worker
        std::unique_ptr<VulkanTexture> texture;
    };
    struct Retired { std::unique_ptr<VulkanTexture> texture; int frames_left; };

    static std::string           key_for(const std::filesystem::path& path);
    std::filesystem::path        png_for(const std::filesystem::path& file) const;
    void                         on_generated(const std::string& key,
                                              std::vector<std::uint8_t> rgba, bool ok);

    std::mutex                             m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<Retired>                   m_retire;
    int                                    m_uploads_this_frame = 0;
    int                                    m_retire_frames = 4; // set to ImageCount+1 in setup()
    vulkan_context*                        m_vk = nullptr;
    std::filesystem::path                  m_thumb_dir;
    bool                                   m_setup = false;
    FileBrowserThumbnailThread             m_thread; // declared last → joined first on destroy
};
