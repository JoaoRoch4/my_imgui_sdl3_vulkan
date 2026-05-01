#pragma once
#include <vulkan/vulkan.h>
#include <imgui.h>

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

class vulkan_context;
class VulkanUploadContext;
class HistoryPreview;

class VideoSeekPreview
{
public:
    static inline ImVec2 preview_size{320.0f, 180.0f};

public:
    VideoSeekPreview();
    ~VideoSeekPreview();

    void setup(vulkan_context* vk,
               VulkanUploadContext* uploader,
               const std::string& source);

    void shutdown();
    void stop_thread();

    void update();
    void seek(double time_sec);

    [[nodiscard]] VkDescriptorSet descriptor_set() const { return m_descriptor_set; }
    [[nodiscard]] ImVec2 size() const { return { (float)m_w, (float)m_h }; }

private:
    void init_mpv(const std::string& source);
    void start_thread();

    bool create_gpu_resources(vulkan_context* vk);
    void destroy_gpu_resources(vulkan_context* vk);

private:
    // mpv
    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_render_ctx = nullptr;

    // threading
    std::jthread m_thread;
    std::atomic<bool> m_frame_dirty{false};
    std::atomic<bool> m_buf_ready{false};
    std::atomic<double> m_seek_req{-1.0};

    std::mutex m_buf_mutex;
    std::vector<uint8_t> m_buf;

    // GPU
    vulkan_context* m_vk = nullptr;
    VulkanUploadContext* m_uploader = nullptr;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_image_memory = VK_NULL_HANDLE;
    VkImageView m_image_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptor_set = VK_NULL_HANDLE;

    int m_w = 0;
    int m_h = 0;
};