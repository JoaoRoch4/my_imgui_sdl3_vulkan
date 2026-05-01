#pragma once

#include "vulkan_context.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Shared hover-thumbnail mpv instance.  One per VideoPlayer, used by
/// HistoryPreview and OpenedFilesWindow to show tooltip previews.
///
/// Call setup() once when the Vulkan device is ready.
/// Call thumbnail() each frame while hovering over a source path/URL.
/// Call shutdown() before destroying the Vulkan device.
class VideoHoverPreview {
public:
    VideoHoverPreview();
    ~VideoHoverPreview();

    VideoHoverPreview(const VideoHoverPreview &) = delete;
    VideoHoverPreview &operator=(const VideoHoverPreview &) = delete;

    void setup(vulkan_context *vk);
    void shutdown();

    /// Preview thumbnail dimensions.  Change before calling setup().
    static inline ImVec2 preview_size{800.0f, 600.0f};

    /// Stop the background thread without freeing GPU resources.
    /// Call this before vkDeviceWaitIdle to allow safe resource destruction.
    void stop_thread();

    /// Load source into the shared hover mpv and return the latest thumbnail
    /// descriptor set.  Call once per frame while hovering.
    /// Returns VK_NULL_HANDLE until the first frame is ready.
    [[nodiscard]] VkDescriptorSet thumbnail(const std::string &source);

private:
    void init_mpv();
    bool create_gpu_resources();
    void destroy_gpu_resources();
    void upload_frame();
    void start_thread();

    std::string            m_source;
    int                    m_w;
    int                    m_h;
    mpv_handle            *m_mpv;
    mpv_render_context    *m_render_ctx;
    std::atomic<bool>      m_frame_dirty;
    std::atomic<bool>      m_buf_ready;
    std::mutex             m_buf_mutex;
    std::vector<uint8_t>   m_buf;
    VkImage                m_image;
    VkDeviceMemory         m_image_memory;
    VkImageView            m_image_view;
    VkSampler              m_sampler;
    VkDescriptorSet        m_descriptor_set;
    VkBuffer               m_staging_buf;
    VkDeviceMemory         m_staging_mem;
    void                  *m_staging_mapped;
    VkCommandPool          m_cmd_pool;
    VkCommandBuffer        m_cmd_buf;
    VkFence                m_fence;
    std::jthread           m_thread;
    vulkan_context        *m_vk;
};
