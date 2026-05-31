#include <cstdint>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "pch.hpp"
#include "vulkan_texture.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

VulkanTexture::VulkanTexture()
    : width{0}
    , height{0}
    , m_descriptor_set{VK_NULL_HANDLE}
    , m_sampler{VK_NULL_HANDLE}
    , m_image_view{VK_NULL_HANDLE}
    , m_image{VK_NULL_HANDLE}
    , m_image_memory{VK_NULL_HANDLE}
    , m_upload_buffer{VK_NULL_HANDLE}
    , m_upload_buffer_memory{VK_NULL_HANDLE} {
}

VulkanTexture::VulkanTexture(VulkanTexture &&other) noexcept
    : width{other.width}
    , height{other.height}
    , m_descriptor_set{other.m_descriptor_set}
    , m_sampler{other.m_sampler}
    , m_image_view{other.m_image_view}
    , m_image{other.m_image}
    , m_image_memory{other.m_image_memory}
    , m_upload_buffer{other.m_upload_buffer}
    , m_upload_buffer_memory{other.m_upload_buffer_memory} {
    other.width = 0;
    other.height = 0;
    other.m_descriptor_set = VK_NULL_HANDLE;
    other.m_sampler = VK_NULL_HANDLE;
    other.m_image_view = VK_NULL_HANDLE;
    other.m_image = VK_NULL_HANDLE;
    other.m_image_memory = VK_NULL_HANDLE;
    other.m_upload_buffer = VK_NULL_HANDLE;
    other.m_upload_buffer_memory = VK_NULL_HANDLE;
}

VulkanTexture &VulkanTexture::operator=(VulkanTexture &&other) noexcept {
    if (this != &other) {
        width = other.width;
        height = other.height;
        m_descriptor_set = other.m_descriptor_set;
        m_sampler = other.m_sampler;
        m_image_view = other.m_image_view;
        m_image = other.m_image;
        m_image_memory = other.m_image_memory;
        m_upload_buffer = other.m_upload_buffer;
        m_upload_buffer_memory = other.m_upload_buffer_memory;

        other.width = 0;
        other.height = 0;
        other.m_descriptor_set = VK_NULL_HANDLE;
        other.m_sampler = VK_NULL_HANDLE;
        other.m_image_view = VK_NULL_HANDLE;
        other.m_image = VK_NULL_HANDLE;
        other.m_image_memory = VK_NULL_HANDLE;
        other.m_upload_buffer = VK_NULL_HANDLE;
        other.m_upload_buffer_memory = VK_NULL_HANDLE;
    }
    return *this;
}

bool VulkanTexture::is_loaded() const {
    return m_descriptor_set != VK_NULL_HANDLE;
}

ImTextureID VulkanTexture::imgui_id() const {
    return std::bit_cast<ImTextureID>(m_descriptor_set);
}

uint32_t VulkanTexture::find_memory_type(VkPhysicalDevice physical_device,
                                         uint32_t type_filter,
                                         VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        // Check if this memory type is supported by the resource (type_filter)
        const auto is_supported = static_cast<bool>(type_filter & (1U << i));

        // Check if this memory type has all the required properties (e.g., DEVICE_LOCAL)
        const auto has_properties = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;

        if (is_supported && has_properties) {
            return i;
        }
    }
    // No suitable memory type found
    // Return an invalid index if no suitable memory type is found
    return UINT32_MAX;
}
bool VulkanTexture::load(const std::filesystem::path &path, vulkan_context &vk) {
    constexpr int k_channels = 4;
    int ch = 0;
    unsigned char *pixels = nullptr;
    bool is_webp = false;

    // 1. Decode Image Data (RAII-style cleanup)
    if (path.extension() == ".webp" || path.extension() == ".WEBP") {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) { return false;
}
        std::vector<uint8_t> buf(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        auto *res = std::bit_cast<char *>(buf.data());
        if (res == nullptr)
            return false;
        auto buf_size = static_cast<std::streamsize>(buf.size());
        file.read(res, buf_size);
        pixels = WebPDecodeRGBA(buf.data(), buf.size(), &width, &height);
        is_webp = true;
    } else {
        pixels = stbi_load(path.string().c_str(), &width, &height, &ch, k_channels);
    }

    if (!pixels)
        return false;

    // Lambda to ensure CPU memory is freed even on Vulkan failure
    auto cleanup_pixels = [&]() {
        if (is_webp)
            WebPFree(pixels);
        else
            stbi_image_free(pixels);
    };

    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * k_channels;
    VkResult err;

    // 2. Create GPU Image
    {
        VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        err = vkCreateImage(vk.device, &info, vk.allocator, &m_image);
        if (err != VK_SUCCESS) {
            cleanup_pixels();
            return false;
        }

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, m_image, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size; // MUST be this
        alloc.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (alloc.memoryTypeIndex == 0xFFFFFFFFu) {
            cleanup_pixels();
            return false;
        }

        err = vkAllocateMemory(vk.device, &alloc, vk.allocator, &m_image_memory);
        vulkan_context::check_result(err);
        vkBindImageMemory(vk.device, m_image, m_image_memory, 0);
    }

    // 3. Create View & Sampler
    {
        VkImageViewCreateInfo v_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v_info.image = m_image;
        v_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        v_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &v_info, vk.allocator, &m_image_view);

        VkSamplerCreateInfo s_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s_info.magFilter = VK_FILTER_LINEAR;
        s_info.minFilter = VK_FILTER_LINEAR;
        s_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        vkCreateSampler(vk.device, &s_info, vk.allocator, &m_sampler);
    }

    // 4. ImGui Registration
    m_descriptor_set = ImGui_ImplVulkan_AddTexture(m_sampler, m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 5. Staging & Upload Logic
    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    {
        VkBufferCreateInfo b_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b_info.size = image_size;
        b_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(vk.device, &b_info, vk.allocator, &staging_buf);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.device, staging_buf, &req);
        VkMemoryAllocateInfo a_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a_info.allocationSize = req.size;
        a_info.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(vk.device, &a_info, vk.allocator, &staging_mem);
        vkBindBufferMemory(vk.device, staging_buf, staging_mem, 0);

        void *map_ptr;
        vkMapMemory(vk.device, staging_mem, 0, image_size, 0, &map_ptr);
        std::memcpy(map_ptr, pixels, static_cast<size_t>(image_size));
        vkUnmapMemory(vk.device, staging_mem);
    }

    // 6. Execute Transfer
    {
        VkCommandBufferAllocateInfo c_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        c_info.commandPool = vk.main_window_data.Frames[static_cast<int>(vk.main_window_data.FrameIndex)].CommandPool;
        c_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c_info.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vk.device, &c_info, &cmd);

        VkCommandBufferBeginInfo b_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b_info);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = m_image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(cmd, staging_buf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        // Submit and Wait for Fence (more stable than QueueWaitIdle)
        VkFence fence;
        VkFenceCreateInfo f_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(vk.device, &f_info, vk.allocator, &fence);

        VkSubmitInfo s_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s_info.commandBufferCount = 1;
        s_info.pCommandBuffers = &cmd;
        vk.queue_submit(1, &s_info, fence);

        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk.device, fence, vk.allocator);

        vkFreeCommandBuffers(vk.device, c_info.commandPool, 1, &cmd);
    }

    // 7. Cleanup Temporary Resources
    vkDestroyBuffer(vk.device, staging_buf, vk.allocator);
    vkFreeMemory(vk.device, staging_mem, vk.allocator);
    cleanup_pixels();

    return true;
}
void VulkanTexture::unload(vulkan_context &vk) {
    // 1. Safety check: Don't attempt to free null handles
    if (!is_loaded())
        return;

    // 2. Unregister from ImGui first
    // This tells the backend the descriptor set is no longer in use
    if (m_descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_descriptor_set);
        m_descriptor_set = VK_NULL_HANDLE;
    }

    // 3. Destroy view-dependent resources
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk.device, m_sampler, vk.allocator);
        m_sampler = VK_NULL_HANDLE;
    }

    if (m_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk.device, m_image_view, vk.allocator);
        m_image_view = VK_NULL_HANDLE;
    }

    // 4. Destroy the image and free its VRAM
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, m_image, vk.allocator);
        m_image = VK_NULL_HANDLE;
    }

    if (m_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, m_image_memory, vk.allocator);
        m_image_memory = VK_NULL_HANDLE;
    }

    // 5. Clean up any leftover staging buffers
    // (Though these should ideally be freed at the end of load())
    if (m_upload_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk.device, m_upload_buffer, vk.allocator);
        m_upload_buffer = VK_NULL_HANDLE;
    }

    if (m_upload_buffer_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, m_upload_buffer_memory, vk.allocator);
        m_upload_buffer_memory = VK_NULL_HANDLE;
    }

    // 6. Reset metadata
    width = 0;
    height = 0;
}
