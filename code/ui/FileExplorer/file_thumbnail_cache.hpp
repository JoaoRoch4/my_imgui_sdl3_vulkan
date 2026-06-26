#pragma once

#include "pch.hpp"

class vulkan_context;
class VulkanTexture;

/// Asynchronous on-disk + GPU thumbnail cache for media files.
///
/// Two independent thread pools are maintained:
///   - Image pool  (jpg/jpeg/png/webp) — up to k_max_img_generators threads.
///   - Video pool  (mpv SW-render)     — up to k_max_vid_generators threads.
/// Paths stay in InQueue until a slot in the appropriate pool is free.
///
/// Thread safety:
///   - setup() / shutdown() / get() must all be called from the render thread.
///   - Generator threads never touch Vulkan.
class FileThumbnailCache {
public:
    FileThumbnailCache();
    ~FileThumbnailCache();

    FileThumbnailCache(const FileThumbnailCache &)            = delete;
    FileThumbnailCache &operator=(const FileThumbnailCache &) = delete;

    /// Attach the Vulkan context and the directory where PNGs are stored.
    /// Must be called from the render thread before any get() call.
    void setup(vulkan_context *vk, const std::filesystem::path &thumb_dir);

    /// Release all GPU resources and join all generator threads.
    /// Must be called from the render thread.
    void shutdown();

    /// Stop all generator threads, delete all on-disk PNGs, and flush the
    /// in-memory cache so thumbnails are regenerated on the next get() call.
    /// Must be called from the render thread.
    void clear();

    /// Remove a single file's entry from the cache (in-memory + on-disk PNG)
    /// so it will be regenerated on the next get() call.
    /// Must be called from the render thread.
    void evict(const std::filesystem::path &path);

    /// Return the ImTextureID for @p path if the thumbnail is ready, or 0
    /// while it is being generated / uploaded.
    /// Automatically spawns a generation thread if the PNG does not exist.
    /// Must be called from the render thread (may upload a GPU texture).
    [[nodiscard]] ImTextureID get(const std::filesystem::path &path);

    /// Sentinel value returned when a thumbnail is not yet available.
    static constexpr ImTextureID k_no_texture = 0;

    /// Maximum concurrent image-thumbnail threads (stb_image — lightweight).
    uint64_t k_max_img_generators = std::thread::hardware_concurrency();
    /// Maximum concurrent video-thumbnail threads (libmpv SW-render — heavy).
    uint64_t k_max_vid_generators = std::thread::hardware_concurrency();

    /// On-disk thumbnail dimensions (pixels).
    static constexpr uint64_t k_thumb_w = std::numeric_limits<uint64_t>::max();
    static constexpr uint64_t k_thumb_h = std::numeric_limits<uint64_t>::max();
	

private:
    enum class State { InQueue, Generating, DiskReady, Ready, Failed };

    struct Entry {
        State                          state;
        std::filesystem::path          png_path;
        std::unique_ptr<VulkanTexture> texture;
    };

    std::filesystem::path png_for(const std::filesystem::path &file) const;

    /// Spawn a new jthread in the appropriate pool to generate the thumbnail.
    /// Called from get() while m_mutex is held.
    void spawn_generator(const std::filesystem::path &path,
                         const std::filesystem::path &out_png,
                         bool                         is_image);

    void generate_thumbnail(const std::filesystem::path &file,
                            const std::filesystem::path &out_png,
                            std::stop_token              st);

    vulkan_context       *m_vk{};
    std::filesystem::path m_thumb_dir;
    bool                  m_setup{false};

    // Protects m_entries, m_active_img_generators, m_active_vid_generators.
    // Lock order: always acquire m_mutex before m_gen_mutex.
    std::mutex                             m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    int                                    m_active_img_generators{0};
    int                                    m_active_vid_generators{0};

    // Protects m_img_threads and m_vid_threads.
    std::mutex                m_gen_mutex;
    std::vector<std::jthread> m_img_threads; ///< image thumbnail pool
    std::vector<std::jthread> m_vid_threads; ///< video thumbnail pool
};
