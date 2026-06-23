#pragma once

#include "pch.hpp"

#include "image_buffer.hpp"
#include "image_types.hpp"

#include <cstddef>
#include <deque>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

class vulkan_context;

// GPU-side BC1/DXT1 encoder driven by a compute shader (bc1_encode.comp).
//
// Submission model:
//   * submit(rgba) from any thread queues the request and returns a future.
//   * pump_once(vk) runs once per frame on the render thread; it (a) checks
//     prior batches for fence completion and fulfils their futures, then
//     (b) drains pending submissions into a new batched dispatch.
//   * Each pending request is encoded as one VkBufferImageCopy-free dispatch
//     where the input RGBA lives in a per-slot HOST_VISIBLE storage buffer
//     and the output BC1 blocks land in a per-slot HOST_VISIBLE readback buffer.
//
// Lifecycle:
//   * setup(vk) once. Returns false when the device or compute path is unavailable.
//   * shutdown(vk) once, before Vulkan teardown.
//
// Thread safety:
//   * submit() is thread-safe (internal mutex).
//   * pump_once() MUST run on the render thread; it owns the queue + command pool.
//   * setup/shutdown MUST run on the render thread.
class VulkanBc1Encoder {
public:
    using Bytes  = std::vector<std::byte>;
    using Result = std::expected<Bytes, img::ImageError>;

    VulkanBc1Encoder();
    ~VulkanBc1Encoder();
    VulkanBc1Encoder(VulkanBc1Encoder const &)            = delete;
    VulkanBc1Encoder &operator=(VulkanBc1Encoder const &) = delete;

    // Process-wide instance, like ImageJobSystem. setup/shutdown is the
    // FileBrowserThumbnailContext's responsibility (render thread).
    static VulkanBc1Encoder &instance();

    [[nodiscard]] bool setup(vulkan_context &vk);
    void               shutdown(vulkan_context &vk);
    [[nodiscard]] bool is_ready() const noexcept;

    // Queue one RGBA8 thumbnail for GPU encode. Returns a future that resolves
    // when the next render-thread pump completes the dispatch.  When the encoder
    // is not ready, or the in-flight cap is reached, returns a future that is
    // already set to ImageError::EncodeFailed — the caller MUST fall back to CPU.
    [[nodiscard]] std::future<Result> submit(img::ImageBuffer src);

    // Run on the render thread once per frame.  Cheap when nothing is pending.
    void pump_once(vulkan_context &vk);

    // Coarse stats for the run-config UI (in-flight + completed counters).
    [[nodiscard]] std::size_t in_flight_count() const noexcept;

    // Soft cap on simultaneous in-flight batches (a batch is one cmdbuf+fence).
    // submit() falls back to CPU when the cap is reached.
    static constexpr std::size_t k_max_in_flight_batches = 4;
    // Max blocks per dispatch — bounds the per-slot buffer size.  320x180 = 3600.
    static constexpr std::size_t k_max_blocks_per_image  = 4096*2;

private:
    struct Pending {
        img::ImageBuffer          src;
        std::promise<Result>      promise;
    };

    struct Slot {
        VkBuffer       in_buf      = VK_NULL_HANDLE;
        VkDeviceMemory in_mem      = VK_NULL_HANDLE;
        VkBuffer       out_buf     = VK_NULL_HANDLE;
        VkDeviceMemory out_mem     = VK_NULL_HANDLE;
        VkDeviceSize   in_capacity = 0; // bytes
        VkDeviceSize   out_capacity= 0; // bytes
        VkDescriptorSet desc_set   = VK_NULL_HANDLE;
        std::size_t    block_count = 0; // for readback size
        std::size_t    expected_bytes = 0; // bc1_size(W, H) — exactly what the caller waits for
        Pending        request;
    };

    struct Batch {
        VkCommandBuffer  cmd   = VK_NULL_HANDLE;
        VkFence          fence = VK_NULL_HANDLE;
        std::vector<std::unique_ptr<Slot>> slots;
    };

    bool allocate_slot_buffers(vulkan_context &vk, Slot &s, VkDeviceSize in_bytes, VkDeviceSize out_bytes);
    void destroy_slot(vulkan_context &vk, Slot &s);
    void fail_request(Pending &p);

    bool m_ready = false;

    VkDescriptorSetLayout m_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipe_layout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline    = VK_NULL_HANDLE;
    VkShaderModule        m_shader      = VK_NULL_HANDLE;
    VkCommandPool         m_cmd_pool    = VK_NULL_HANDLE;
    VkDescriptorPool      m_desc_pool   = VK_NULL_HANDLE;

    mutable std::mutex   m_queue_mutex;
    std::deque<Pending>  m_pending;     // workers push, pump drains
    std::deque<Batch>    m_in_flight;   // batches awaiting fence signal
};
