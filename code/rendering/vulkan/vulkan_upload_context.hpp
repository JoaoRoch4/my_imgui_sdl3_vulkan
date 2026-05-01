#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

class vulkan_context;

class VulkanUploadContext
{
public:
    VulkanUploadContext() = default;
    ~VulkanUploadContext();

    void init(vulkan_context* vk, size_t staging_size = 16 * 1024 * 1024);
    void shutdown();

    // API limpa (único ponto de entrada)
    void upload_to_image(
        const void* data,
        VkImage image,
        uint32_t width,
        uint32_t height
    );

private:
    vulkan_context* m_vk = nullptr;

    VkBuffer        m_staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory  m_staging_memory = VK_NULL_HANDLE;
    void*           m_mapped = nullptr;
    size_t          m_capacity = 0;

private:
    void ensure_capacity(size_t size);
};