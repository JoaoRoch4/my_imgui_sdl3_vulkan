#include "vulkan_upload_context.hpp"
#include "vulkan_context.hpp"

#include <algorithm>
#include <cstring>

namespace {

uint32_t find_memory_type(VkPhysicalDevice physical_device,
                          uint32_t type_filter,
                          VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool is_supported = (type_filter & (1u << i)) != 0;
        const bool has_properties =
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (is_supported && has_properties)
            return i;
    }

    return 0xFFFFFFFFu;
}

} // namespace

void VulkanUploadContext::init(vulkan_context* vk, size_t staging_size)
{
    m_vk = vk;
    m_capacity = staging_size;

    VkBufferCreateInfo b_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b_info.size = static_cast<VkDeviceSize>(staging_size);
    b_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    b_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(vk->device, &b_info, vk->allocator, &m_staging_buffer);

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vk->device, m_staging_buffer, &req);

    VkMemoryAllocateInfo a_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a_info.allocationSize = req.size;
    a_info.memoryTypeIndex = find_memory_type(
        vk->physical_device,
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(vk->device, &a_info, vk->allocator, &m_staging_memory);
    vkBindBufferMemory(vk->device, m_staging_buffer, m_staging_memory, 0);
    vkMapMemory(vk->device, m_staging_memory, 0, req.size, 0, &m_mapped);
}

VulkanUploadContext::~VulkanUploadContext()
{
    shutdown();
}

void VulkanUploadContext::shutdown()
{
    if (!m_vk) return;

    if (m_mapped)
        vkUnmapMemory(m_vk->device, m_staging_memory);
    if (m_staging_buffer)
        vkDestroyBuffer(m_vk->device, m_staging_buffer, m_vk->allocator);
    if (m_staging_memory)
        vkFreeMemory(m_vk->device, m_staging_memory, m_vk->allocator);

    m_staging_buffer = VK_NULL_HANDLE;
    m_staging_memory = VK_NULL_HANDLE;
    m_mapped = nullptr;
    m_capacity = 0;
    m_vk = nullptr;
}

void VulkanUploadContext::ensure_capacity(size_t size)
{
    if (size <= m_capacity)
        return;

    vulkan_context* vk = m_vk;
    const size_t new_capacity = std::max(size, m_capacity == 0 ? size : m_capacity * 2);
    shutdown();
    init(vk, new_capacity);
}

void VulkanUploadContext::upload_to_image(
    const void* data,
    VkImage image,
    uint32_t width,
    uint32_t height
)
{
    size_t size = width * height * 4; // RGBA8

    if (!m_vk || !data)
        return;

    ensure_capacity(size);

    // copiar CPU -> staging
    std::memcpy(m_mapped, data, size);

    VkCommandPool pool =
        m_vk->main_window_data.Frames[static_cast<int>(m_vk->main_window_data.FrameIndex)].CommandPool;

    VkCommandBufferAllocateInfo c_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    c_info.commandPool = pool;
    c_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    c_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_vk->device, &c_info, &cmd);

    VkCommandBufferBeginInfo b_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &b_info);

    // layout: undefined -> transfer dst
    VkImageMemoryBarrier barrier1{};
    barrier1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier1.srcAccessMask = 0;
    barrier1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier1.image = image;
    barrier1.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier1
    );

    // copiar buffer -> imagem
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(
        cmd,
        m_staging_buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    // layout: transfer -> shader read
    VkImageMemoryBarrier barrier2 = barrier1;
    barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier2
    );

    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo f_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m_vk->device, &f_info, m_vk->allocator, &fence);

    VkSubmitInfo s_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s_info.commandBufferCount = 1;
    s_info.pCommandBuffers = &cmd;
    m_vk->queue_submit(1, &s_info, fence);

    vkWaitForFences(m_vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(m_vk->device, fence, m_vk->allocator);
    vkFreeCommandBuffers(m_vk->device, pool, 1, &cmd);
}