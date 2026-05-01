#pragma once

#include "vulkan_context.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Per-entry seek-preview thumbnail.  One instance per open VideoEntry.
///
/// Call setup() once the mpv source is known.
/// Call seek() each frame while the seekbar is hovered to queue a seek.
/// Call update() each frame to upload a new frame if one is ready.
/// Call shutdown() before destroying the Vulkan device.
class VideoSeekPreview {
public:
    VideoSeekPreview();
    ~VideoSeekPreview();

    VideoSeekPreview(const VideoSeekPreview &) = delete;
    VideoSeekPreview &operator=(const VideoSeekPreview &) = delete;

    /// Preview thumbnail dimensions.  Change before calling setup().
    static inline ImVec2 preview_size{800.0f, 600.0f};

    /// Initialise the mpv instance and GPU resources for the given source.
    void setup(vulkan_context *vk, const std::string &source);
    void shutdown();

    /// Queue a seek to position @p time_sec (seconds).  Thread-safe.
    void seek(double time_sec);

    /// Upload a pending rendered frame to the GPU.  Call on the main thread
    /// once per frame (e.g. from VideoPlayer::update_frames).
    void update();

    /// Stop the background thread without freeing GPU resources.
    /// Call this before vkDeviceWaitIdle to allow safe resource destruction.
    void stop_thread();

    /// Returns VK_NULL_HANDLE until the first frame is ready.
    [[nodiscard]] VkDescriptorSet descriptor_set() const;

    /// Returns the actual pixel dimensions this instance was allocated with.
    [[nodiscard]] ImVec2 size() const;

private:
    void init_mpv(const std::string &source);
    bool create_gpu_resources(vulkan_context *vk);
    void destroy_gpu_resources(vulkan_context *vk);
    void upload_frame(vulkan_context *vk);
    void start_thread();

    mpv_handle            *m_mpv;
    mpv_render_context    *m_render_ctx;
    int                    m_w;
    int                    m_h;
    std::atomic<bool>      m_frame_dirty;
    std::atomic<double>    m_seek_req;       ///< -1.0 = idle
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
