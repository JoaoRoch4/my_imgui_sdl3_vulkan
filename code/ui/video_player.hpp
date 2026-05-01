#pragma once

#include "video_hover_preview.hpp"
#include "video_seek_preview.hpp"
#include "vulkan_context.hpp"

#include "imgui.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/// Manages one or more mpv-backed video/GIF/audio windows rendered into
/// Vulkan textures via the libmpv software render API.
///
/// Each open media file gets its own mpv_handle so playback state is
/// independent.  Frame data is rendered by libmpv into a CPU pixel buffer
/// then uploaded to a per-entry VkImage every time the render-update
/// callback fires.
class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    void setup(vulkan_context *vk);
    void shutdown();

    /// Open a local file.  Returns false on mpv initialisation failure.
    bool add_from_path(const std::filesystem::path &path);

    /// Stream a URL (mpv handles the network protocol).  Returns false on failure.
    bool add_from_url(const std::string &url, const std::string &title);

    /// Upload new frames to GPU and poll mpv events.  Call once per frame.
    void update_frames();

    /// Draw all open video windows.  Call once per frame inside an ImGui frame.
    void draw();

    /// Returns true when at least one video window is open.
    [[nodiscard]] bool has_open_windows() const;

    /// Return the current playback frame descriptor for an already-open entry,
    /// or VK_NULL_HANDLE if no such entry exists.
    [[nodiscard]] VkDescriptorSet get_open_thumbnail(const std::string &source) const;

    /// Loads source into the shared hover preview and returns the latest
    /// thumbnail descriptor set.  Call once per frame while hovering.
    /// Returns VK_NULL_HANDLE until the first frame is ready.
    [[nodiscard]] VkDescriptorSet hover_thumbnail(const std::string &source);

    /// Return true if the given path should be opened in the video player.
    static bool is_video_path(const std::filesystem::path &path);

    /// Return true if the given URL should be streamed in the video player.
    static bool is_video_url(const std::string &url);

    /// Size used for hover/seek-preview thumbnails.
    static constexpr ImVec2 k_preview_size{320.0f, 180.0f};

private:
    /// All per-video runtime state.
    struct VideoEntry {
        VideoEntry();

        mpv_handle         *mpv;
        mpv_render_context *render_ctx;
        std::atomic<bool>   frame_dirty;

        int                  video_w;
        int                  video_h;
        std::vector<uint8_t> pixel_buf;

        // Vulkan resources (created once VIDEO_RECONFIG fires with valid dims)
        VkImage         image;
        VkDeviceMemory  image_memory;
        VkImageView     image_view;
        VkSampler       sampler;
        VkDescriptorSet descriptor_set;
        VkBuffer        staging_buf;
        VkDeviceMemory  staging_mem;
        void           *staging_mapped;
        VkCommandPool   cmd_pool;
        VkCommandBuffer cmd_buf;
        VkFence         upload_fence;

        // Entry metadata
        std::string title;
        std::string source;
        std::string kind; ///< "video", "gif", or "audio"
        int         id;
        bool        open;
        bool        fullscreen;

        // Seek-preview thumbnail (dedicated mpv + jthread via VideoSeekPreview)
        VideoSeekPreview seek_preview;
    };

    static uint32_t find_memory_type(VkPhysicalDevice phys,
                                     uint32_t filter,
                                     VkMemoryPropertyFlags props);

    bool create_gpu_resources(VideoEntry &e);
    void destroy_gpu_resources(VideoEntry &e);
    void upload_frame(VideoEntry &e);
    void draw_window(VideoEntry &e, int idx);
    void poll_events(VideoEntry &e);

    // Shared hover thumbnail (one per VideoPlayer)
    VideoHoverPreview m_hover;

    vulkan_context *m_vk;
    std::vector<std::unique_ptr<VideoEntry>> m_entries;
    int m_next_id;
};
