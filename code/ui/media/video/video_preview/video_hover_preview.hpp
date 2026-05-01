#pragma once

#include <imgui.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class vulkan_context;

class VideoHoverPreview {
public:
    static inline ImVec2 preview_size = {320, 180};
    static constexpr size_t max_cache_size = 32;

    VideoHoverPreview();
    ~VideoHoverPreview();

    void setup(vulkan_context *vk);
    void shutdown();

    VkDescriptorSet thumbnail(const std::string &source);
    bool save_frame(const std::filesystem::path &path);

    struct GpuSlot {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        VkDescriptorSet descriptor{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool has_frame = false;

        std::list<std::string>::iterator lru_it;
    };

    // core
    void init_mpv();
    void start_thread();
    void stop_thread();

    void load_source(const std::string &source);
    void start_playback(const std::string &source);
    void stop_playback();

    void upload_frame_async(const std::string &source);

    // cache
    bool create_slot(const std::string &source);
    void destroy_slot(const std::string &source);
    void touch_lru(const std::string &source);
    void evict_if_needed();

    // vulkan
    bool create_shared();
    void destroy_shared();

private:
    vulkan_context *m_vk{};

    std::unordered_map<std::string, GpuSlot> m_cache;
    std::list<std::string> m_lru;

    std::string m_current;
    std::string m_playing;
    std::chrono::steady_clock::time_point m_last_load_time{};

    mpv_handle *m_mpv{};
    mpv_render_context *m_render{};

    std::atomic<bool> m_frame_dirty{false};
    std::atomic<bool> m_waiting{false};

    std::jthread m_thread;

    std::vector<uint8_t> m_buf;
    std::mutex m_buf_mutex;

    VkBuffer m_staging{};
    VkDeviceMemory m_staging_mem{};
    void *m_mapped{};

    VkCommandPool m_pool{};
    VkCommandBuffer m_cmd{};
    VkFence m_fence{};

    int m_w{}, m_h{};
};