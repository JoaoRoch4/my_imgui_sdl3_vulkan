#include "vulkan_emoji_atlas.hpp"
#include "vulkan_context.hpp"

#include <cstring> // std::memcpy

VulkanEmojiAtlas::VulkanEmojiAtlas(vulkan_context& vk)
    : m_vk(vk)
{
}

VulkanEmojiAtlas::~VulkanEmojiAtlas()
{
    shutdown();
}

void VulkanEmojiAtlas::shutdown()
{
    if (m_descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_descriptor_set);
        m_descriptor_set = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vk.device, m_sampler, m_vk.allocator);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vk.device, m_image_view, m_vk.allocator);
        m_image_view = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_vk.device, m_image, m_vk.allocator);
        m_image = VK_NULL_HANDLE;
    }
    if (m_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk.device, m_image_memory, m_vk.allocator);
        m_image_memory = VK_NULL_HANDLE;
    }
    TextureID_ = ImTextureID_Invalid;
}

uint32_t VulkanEmojiAtlas::find_memory_type(VkPhysicalDevice physical_device,
                                             uint32_t type_filter,
                                             VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool is_supported = static_cast<bool>(type_filter & (1U << i));
        const bool has_properties =
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (is_supported && has_properties)
            return i;
    }
    return UINT32_MAX;
}

ImTextureID VulkanEmojiAtlas::UploadRGBA(const std::vector<uint8_t>& pixels,
                                          int width, int height)
{
    // Free any previous texture before re-uploading.
    shutdown();

    const VkDeviceSize image_size =
        static_cast<VkDeviceSize>(width) * height * 4;

    // ── 1. Create GPU image ────────────────────────────────────────────────
    {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent      = { static_cast<uint32_t>(width),
                             static_cast<uint32_t>(height), 1u };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_vk.device, &info, m_vk.allocator, &m_image) != VK_SUCCESS)
            return ImTextureID_Invalid;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_vk.device, m_image, &req);

        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = find_memory_type(m_vk.physical_device,
                                                  req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX)
            return ImTextureID_Invalid;

        vulkan_context::check_result(
            vkAllocateMemory(m_vk.device, &alloc, m_vk.allocator, &m_image_memory));
        vkBindImageMemory(m_vk.device, m_image, m_image_memory, 0);
    }

    // ── 2. Image view + sampler ────────────────────────────────────────────
    {
        VkImageViewCreateInfo v_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        v_info.image    = m_image;
        v_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
        v_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(m_vk.device, &v_info, m_vk.allocator, &m_image_view);

        VkSamplerCreateInfo s_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        s_info.magFilter    = VK_FILTER_LINEAR;
        s_info.minFilter    = VK_FILTER_LINEAR;
        s_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s_info.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        vkCreateSampler(m_vk.device, &s_info, m_vk.allocator, &m_sampler);
    }

    // ── 3. Register with ImGui ─────────────────────────────────────────────
    m_descriptor_set = ImGui_ImplVulkan_AddTexture(
        m_sampler, m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ── 4. Staging buffer ──────────────────────────────────────────────────
    VkBuffer       staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo b_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        b_info.size  = image_size;
        b_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_vk.device, &b_info, m_vk.allocator, &staging_buf);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_vk.device, staging_buf, &req);

        VkMemoryAllocateInfo a_info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        a_info.allocationSize  = req.size;
        a_info.memoryTypeIndex = find_memory_type(m_vk.physical_device,
                                                   req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_vk.device, &a_info, m_vk.allocator, &staging_mem);
        vkBindBufferMemory(m_vk.device, staging_buf, staging_mem, 0);

        void* map_ptr = nullptr;
        vkMapMemory(m_vk.device, staging_mem, 0, image_size, 0, &map_ptr);
        std::memcpy(map_ptr, pixels.data(), static_cast<size_t>(image_size));
        vkUnmapMemory(m_vk.device, staging_mem);
    }

    // ── 5. Transfer: undefined → transfer_dst → shader_read ───────────────
    {
        const uint32_t frame_idx = m_vk.main_window_data.FrameIndex;
        VkCommandPool cmd_pool =
            m_vk.main_window_data.Frames[static_cast<int>(frame_idx)].CommandPool;

        VkCommandBufferAllocateInfo c_info = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        c_info.commandPool        = cmd_pool;
        c_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_vk.device, &c_info, &cmd);

        VkCommandBufferBeginInfo b_info = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        b_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b_info);

        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image            = m_image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent      = { static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height), 1 };
        vkCmdCopyBufferToImage(cmd, staging_buf, m_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo f_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(m_vk.device, &f_info, m_vk.allocator, &fence);

        VkSubmitInfo s_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        s_info.commandBufferCount = 1;
        s_info.pCommandBuffers    = &cmd;
        m_vk.queue_submit(1, &s_info, fence);

        vkWaitForFences(m_vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_vk.device, fence, m_vk.allocator);
        vkFreeCommandBuffers(m_vk.device, cmd_pool, 1, &cmd);
    }

    // ── 6. Free staging resources ──────────────────────────────────────────
    vkDestroyBuffer(m_vk.device, staging_buf, m_vk.allocator);
    vkFreeMemory(m_vk.device, staging_mem, m_vk.allocator);

    return std::bit_cast<ImTextureID>(m_descriptor_set);
}
