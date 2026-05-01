#include "video_hover_preview.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <chrono>
#include <cstring>
#include <thread>

// ============================================================================
// Module-local constants
// ============================================================================

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t filter,
                          VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && ((mem.memoryTypes[i].propertyFlags & props) == props))
            return i;
    return 0xFFFFFFFFu;
}

} // namespace

// ============================================================================
// Constructor / destructor
// ============================================================================

VideoHoverPreview::VideoHoverPreview()
    : m_source{}
    , m_w{0}
    , m_h{0}
    , m_mpv{nullptr}
    , m_render_ctx{nullptr}
    , m_frame_dirty{false}
    , m_buf_ready{false}
    , m_buf{}
    , m_image{VK_NULL_HANDLE}
    , m_image_memory{VK_NULL_HANDLE}
    , m_image_view{VK_NULL_HANDLE}
    , m_sampler{VK_NULL_HANDLE}
    , m_descriptor_set{VK_NULL_HANDLE}
    , m_staging_buf{VK_NULL_HANDLE}
    , m_staging_mem{VK_NULL_HANDLE}
    , m_staging_mapped{nullptr}
    , m_cmd_pool{VK_NULL_HANDLE}
    , m_cmd_buf{VK_NULL_HANDLE}
    , m_fence{VK_NULL_HANDLE}
    , m_vk{nullptr}
{
}

VideoHoverPreview::~VideoHoverPreview()
{
    if (m_vk)
        shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

void VideoHoverPreview::setup(vulkan_context *vk)
{
    m_vk = vk;
    init_mpv();
    create_gpu_resources();
    start_thread();
}

void VideoHoverPreview::shutdown()
{
    if (!m_vk)
        return;

    m_thread = std::jthread{};

    if (m_render_ctx) {
        mpv_render_context_free(m_render_ctx);
        m_render_ctx = nullptr;
    }
    destroy_gpu_resources();
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }

    m_vk = nullptr;
}

// ============================================================================
// mpv init
// ============================================================================

void VideoHoverPreview::stop_thread()
{
    m_thread = std::jthread{};
}

void VideoHoverPreview::init_mpv()
{
    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    mpv_set_option_string(m_mpv, "hwdec", "no");
    mpv_set_option_string(m_mpv, "vo",    "libmpv");
    mpv_set_option_string(m_mpv, "pause", "yes");
    mpv_set_option_string(m_mpv, "ytdl",  "no");

    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID,  nullptr},
    };
    if (mpv_render_context_create(&m_render_ctx, m_mpv, params) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    mpv_render_context_set_update_callback(
        m_render_ctx,
        [](void *ctx) {
            static_cast<VideoHoverPreview *>(ctx)->m_frame_dirty.store(
                true, std::memory_order_release);
        },
        this);
}

// ============================================================================
// GPU resources
// ============================================================================

bool VideoHoverPreview::create_gpu_resources()
{
    if (!m_vk)
        return false;

    const int          pw    = static_cast<int>(VideoHoverPreview::preview_size.x);
    const int          ph    = static_cast<int>(VideoHoverPreview::preview_size.y);
    m_w = pw;
    m_h = ph;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pw) * ph * 4;

    // VkImage (device-local, OPTIMAL tiling)
    {
        VkImageCreateInfo info   = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType           = VK_IMAGE_TYPE_2D;
        info.format              = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent              = {static_cast<uint32_t>(pw), static_cast<uint32_t>(ph), 1u};
        info.mipLevels           = 1;
        info.arrayLayers         = 1;
        info.samples             = VK_SAMPLE_COUNT_1_BIT;
        info.tiling              = VK_IMAGE_TILING_OPTIMAL;
        info.usage               = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode         = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout       = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_vk->device, &info, m_vk->allocator, &m_image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_vk->device, m_image, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize       = req.size;
        alloc.memoryTypeIndex      = find_memory_type(m_vk->physical_device, req.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &m_image_memory) != VK_SUCCESS)
            return false;

        vkBindImageMemory(m_vk->device, m_image, m_image_memory, 0);
    }

    // VkImageView
    {
        VkImageViewCreateInfo info  = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image                  = m_image;
        info.viewType               = VK_IMAGE_VIEW_TYPE_2D;
        info.format                 = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange       = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_vk->device, &info, m_vk->allocator, &m_image_view);
    }

    // VkSampler
    {
        VkSamplerCreateInfo info  = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter            = VK_FILTER_LINEAR;
        info.minFilter            = VK_FILTER_LINEAR;
        info.addressModeU         = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.addressModeV         = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.borderColor          = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        vkCreateSampler(m_vk->device, &info, m_vk->allocator, &m_sampler);
    }

    m_descriptor_set = ImGui_ImplVulkan_AddTexture(
        m_sampler, m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Staging buffer (HOST_VISIBLE, persistently mapped)
    {
        VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size               = bytes;
        info.usage              = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_vk->device, &info, m_vk->allocator, &m_staging_buf);

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_vk->device, m_staging_buf, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize       = req.size;
        alloc.memoryTypeIndex      = find_memory_type(
            m_vk->physical_device, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &m_staging_mem);
        vkBindBufferMemory(m_vk->device, m_staging_buf, m_staging_mem, 0);
        vkMapMemory(m_vk->device, m_staging_mem, 0, bytes, 0, &m_staging_mapped);
    }

    // Dedicated command pool (resettable)
    {
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex        = m_vk->queue_family;
        info.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_vk->device, &info, m_vk->allocator, &m_cmd_pool);

        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool               = m_cmd_pool;
        alloc.level                     = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount        = 1;
        vkAllocateCommandBuffers(m_vk->device, &alloc, &m_cmd_buf);
    }

    // Reusable fence
    {
        VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(m_vk->device, &info, m_vk->allocator, &m_fence);
    }

    m_buf.assign(static_cast<size_t>(pw) * ph * 4, 0);

    // Initial layout transition: UNDEFINED → SHADER_READ_ONLY
    {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd_buf, &begin);

        VkImageMemoryBarrier barrier  = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout             = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image                 = m_image;
        barrier.subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask         = 0;
        barrier.dstAccessMask         = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(m_cmd_buf,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(m_cmd_buf);

        VkSubmitInfo submit       = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &m_cmd_buf;
        vkQueueSubmit(m_vk->queue, 1, &submit, m_fence);
        vkWaitForFences(m_vk->device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_vk->device, 1, &m_fence);
        vkResetCommandBuffer(m_cmd_buf, 0);
    }

    return true;
}

void VideoHoverPreview::destroy_gpu_resources()
{
    if (!m_vk)
        return;

    if (m_descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_descriptor_set);
        m_descriptor_set = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vk->device, m_sampler, m_vk->allocator);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vk->device, m_image_view, m_vk->allocator);
        m_image_view = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_vk->device, m_image, m_vk->allocator);
        m_image = VK_NULL_HANDLE;
    }
    if (m_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, m_image_memory, m_vk->allocator);
        m_image_memory = VK_NULL_HANDLE;
    }
    if (m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_vk->device, m_fence, m_vk->allocator);
        m_fence = VK_NULL_HANDLE;
    }
    if (m_cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_vk->device, m_cmd_pool, m_vk->allocator);
        m_cmd_pool = VK_NULL_HANDLE;
        m_cmd_buf  = VK_NULL_HANDLE;
    }
    if (m_staging_buf != VK_NULL_HANDLE) {
        if (m_staging_mapped) {
            vkUnmapMemory(m_vk->device, m_staging_mem);
            m_staging_mapped = nullptr;
        }
        vkDestroyBuffer(m_vk->device, m_staging_buf, m_vk->allocator);
        m_staging_buf = VK_NULL_HANDLE;
    }
    if (m_staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, m_staging_mem, m_vk->allocator);
        m_staging_mem = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Upload frame (main thread)
// ============================================================================

void VideoHoverPreview::upload_frame()
{
    if (!m_staging_mapped || m_image == VK_NULL_HANDLE)
        return;

    const int pw = m_w;
    const int ph = m_h;

    {
        std::lock_guard lock{m_buf_mutex};
        std::memcpy(m_staging_mapped, m_buf.data(), m_buf.size());
    }

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmd_buf, &begin);

    VkImageMemoryBarrier b1  = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.newLayout             = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.image                 = m_image;
    b1.subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b1.srcAccessMask         = VK_ACCESS_SHADER_READ_BIT;
    b1.dstAccessMask         = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(m_cmd_buf,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region    = {};
    region.imageSubresource     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent          = {static_cast<uint32_t>(pw), static_cast<uint32_t>(ph), 1u};
    vkCmdCopyBufferToImage(m_cmd_buf, m_staging_buf, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier b2  = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b2.oldLayout             = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.image                 = m_image;
    b2.subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b2.srcAccessMask         = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask         = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd_buf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(m_cmd_buf);

    VkSubmitInfo submit       = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &m_cmd_buf;
    vkQueueSubmit(m_vk->queue, 1, &submit, m_fence);
    vkWaitForFences(m_vk->device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_vk->device, 1, &m_fence);
    vkResetCommandBuffer(m_cmd_buf, 0);
}

// ============================================================================
// Background thread
// ============================================================================

void VideoHoverPreview::start_thread()
{
    m_thread = std::jthread([this](const std::stop_token& stoken) {
        while (!stoken.stop_requested()) {
            if (m_mpv)
                mpv_wait_event(m_mpv, 0.02);
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            if (m_frame_dirty.exchange(false, std::memory_order_acq_rel)) {
                const int pw       = m_w;
                const int ph       = m_h;
                int    size_arr[2] = {pw, ph};
                size_t stride      = static_cast<size_t>(pw) * 4;
                const char *fmt    = "rgba";

                std::lock_guard lock{m_buf_mutex};
                mpv_render_param params[] = {
                    {MPV_RENDER_PARAM_SW_SIZE,    size_arr},
                    {MPV_RENDER_PARAM_SW_FORMAT,  const_cast<char *>(fmt)},
                    {MPV_RENDER_PARAM_SW_STRIDE,  &stride},
                    {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
                    {MPV_RENDER_PARAM_INVALID,    nullptr},
                };
                if (mpv_render_context_render(m_render_ctx, params) >= 0)
                    m_buf_ready.store(true, std::memory_order_release);
            }
        }
    });
}

// ============================================================================
// thumbnail
// ============================================================================

VkDescriptorSet VideoHoverPreview::thumbnail(const std::string &source)
{
    if (!m_mpv || !m_vk)
        return VK_NULL_HANDLE;

    // Reinitialise GPU resources if the desired size has changed since setup.
    const int new_w = static_cast<int>(preview_size.x);
    const int new_h = static_cast<int>(preview_size.y);
    if (new_w != m_w || new_h != m_h) {
        m_thread = std::jthread{};
        vkDeviceWaitIdle(m_vk->device);
        destroy_gpu_resources();
        m_buf_ready.store(false, std::memory_order_release);
        m_source.clear();
        create_gpu_resources();
        start_thread();
    }

    if (source != m_source) {
        m_source = source;
        m_buf_ready.store(false, std::memory_order_release);
        const char *cmd[] = {"loadfile", source.c_str(), "replace", nullptr};
        mpv_command_async(m_mpv, 0, cmd);
    }

    if (m_buf_ready.exchange(false, std::memory_order_acq_rel) &&
        m_image != VK_NULL_HANDLE) {
        upload_frame();
    }

    return m_descriptor_set;
}
