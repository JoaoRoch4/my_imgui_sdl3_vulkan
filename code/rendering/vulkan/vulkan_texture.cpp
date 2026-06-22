#include "pch.hpp" // NOLINT
#include "vulkan_texture.hpp"

// stb implementations are compiled in the standalone `stb` library (see
// CMakeLists.txt); include the headers here declaration-only.
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
    constexpr int  k_channels = 4;
    int            ch         = 0;
    int            w          = 0;
    int            h          = 0;
    unsigned char *pixels     = nullptr;
    bool           is_webp    = false;

    // 1. Decode Image Data
    if (path.extension() == ".webp" || path.extension() == ".WEBP") {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;
        std::vector<uint8_t> buf(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(std::bit_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
        pixels  = WebPDecodeRGBA(buf.data(), buf.size(), &w, &h);
        is_webp = true;
    } else {
        pixels = stbi_load(path.string().c_str(), &w, &h, &ch, k_channels);
    }

    if (!pixels)
        return false;

    // 2. Upload to the GPU, then free the decoded CPU pixels (we own them here).
    const bool ok = upload_pixels(pixels, w, h, vk);
    if (is_webp)
        WebPFree(pixels);
    else
        stbi_image_free(pixels);
    return ok;
}

bool VulkanTexture::upload(const img::ImageBuffer &buf, vulkan_context &vk) {
    // Only RGBA8 is supported by the fixed VK_FORMAT_R8G8B8A8_UNORM path below.
    if (!buf.valid() || buf.channels != 4)
        return false;
    return upload_pixels(buf.data.data(), buf.width, buf.height, vk);
}

bool VulkanTexture::upload_bc1(std::span<std::byte const> blocks, int w, int h, vulkan_context &vk) {
    if (w <= 0 || h <= 0)
        return false;
    const auto bx = static_cast<std::size_t>((w + 3) / 4);
    const auto by = static_cast<std::size_t>((h + 3) / 4);
    if (blocks.size() != bx * by * 8u)
        return false;

    width  = w;
    height = h;
    const auto image_size = static_cast<VkDeviceSize>(blocks.size());

    // 1. Compressed image (extent is texel dims; the driver maps to 4x4 blocks).
    {
        VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        info.extent        = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u};
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vk.device, &info, vk.allocator, &m_image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, m_image, &req);
        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == 0xFFFFFFFFu)
            return false;
        vulkan_context::check_result(vkAllocateMemory(vk.device, &alloc, vk.allocator, &m_image_memory));
        vkBindImageMemory(vk.device, m_image, m_image_memory, 0);
    }

    // 2. View (format must match the compressed image) + ImGui registration.
    {
        VkImageViewCreateInfo v_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v_info.image            = m_image;
        v_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        v_info.format           = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        v_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &v_info, vk.allocator, &m_image_view);
    }
    m_descriptor_set = ImGui_ImplVulkan_AddTexture(m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 3. Staging buffer holds the raw BC1 blocks.
    VkBuffer       staging_buf;
    VkDeviceMemory staging_mem;
    {
        VkBufferCreateInfo b_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b_info.size  = image_size;
        b_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(vk.device, &b_info, vk.allocator, &staging_buf);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.device, staging_buf, &req);
        VkMemoryAllocateInfo a_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a_info.allocationSize  = req.size;
        a_info.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(vk.device, &a_info, vk.allocator, &staging_mem);
        vkBindBufferMemory(vk.device, staging_buf, staging_mem, 0);

        void *map_ptr;
        vkMapMemory(vk.device, staging_mem, 0, image_size, 0, &map_ptr);
        std::memcpy(map_ptr, blocks.data(), static_cast<size_t>(image_size));
        vkUnmapMemory(vk.device, staging_mem);
    }

    // 4. Transfer (same barriers as upload_pixels; copy extent is texel dims).
    {
        VkCommandBufferAllocateInfo c_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        c_info.commandPool        = vk.main_window_data.Frames[static_cast<int>(vk.main_window_data.FrameIndex)].CommandPool;
        c_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c_info.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vk.device, &c_info, &cmd);
        VkCommandBufferBeginInfo b_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b_info);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image            = m_image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(cmd, staging_buf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);

        VkFence           fence;
        VkFenceCreateInfo f_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(vk.device, &f_info, vk.allocator, &fence);
        VkSubmitInfo s_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s_info.commandBufferCount = 1;
        s_info.pCommandBuffers    = &cmd;
        vk.queue_submit(1, &s_info, fence);
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk.device, fence, vk.allocator);
        vkFreeCommandBuffers(vk.device, c_info.commandPool, 1, &cmd);
    }

    vkDestroyBuffer(vk.device, staging_buf, vk.allocator);
    vkFreeMemory(vk.device, staging_mem, vk.allocator);
    return true;
}

bool VulkanTexture::upload_pixels(const unsigned char *pixels, int w, int h, vulkan_context &vk) {
    constexpr int k_channels = 4;
    if (pixels == nullptr || w <= 0 || h <= 0)
        return false;

    width  = w;
    height = h;

    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * k_channels;
    VkResult           err;

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
            return false;
        }

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, m_image, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size; // MUST be this
        alloc.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (alloc.memoryTypeIndex == 0xFFFFFFFFu) {
            return false;
        }

        err = vkAllocateMemory(vk.device, &alloc, vk.allocator, &m_image_memory);
        vulkan_context::check_result(err);
        vkBindImageMemory(vk.device, m_image, m_image_memory, 0);
    }

    // 3. Create View only. ImGui 1.92's ImGui_ImplVulkan_AddTexture(view, layout) binds
    // its own GLOBAL sampler (a separate VK_DESCRIPTOR_TYPE_SAMPLER descriptor) — a
    // per-texture sampler is unused and, at thumbnail scale, exhausts the device's
    // maxSamplerAllocationCount (4000) and aborts. So we no longer create one;
    // m_sampler stays VK_NULL_HANDLE.
    {
        VkImageViewCreateInfo v_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v_info.image = m_image;
        v_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        v_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &v_info, vk.allocator, &m_image_view);
    }

    // 4. ImGui Registration
    m_descriptor_set = ImGui_ImplVulkan_AddTexture(m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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

    // 7. Cleanup Temporary Resources (the caller owns `pixels`, not us).
    vkDestroyBuffer(vk.device, staging_buf, vk.allocator);
    vkFreeMemory(vk.device, staging_mem, vk.allocator);

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
