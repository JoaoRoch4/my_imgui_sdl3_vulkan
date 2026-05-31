#pragma once
#include "pch.hpp"
#include "emoji_atlas.hpp"

class vulkan_context;

// Vulkan backend for EmojiAtlas.
// Uploads the atlas RGBA pixels to a GPU texture via the same Vulkan pipeline
// used by VulkanTexture.  Call shutdown() before ImGui_ImplVulkan_Shutdown().
class VulkanEmojiAtlas : public EmojiAtlas
{
public:
    explicit VulkanEmojiAtlas(vulkan_context& vk);
    ~VulkanEmojiAtlas() override;

    // Free all GPU resources.  Must be called before ImGui_ImplVulkan_Shutdown().
    void shutdown();

protected:
    ImTextureID UploadRGBA(const std::vector<uint8_t>& pixels,
                           int width, int height) override;

private:
    vulkan_context& m_vk;

    VkDescriptorSet m_descriptor_set { VK_NULL_HANDLE };
    VkSampler       m_sampler        { VK_NULL_HANDLE };
    VkImageView     m_image_view     { VK_NULL_HANDLE };
    VkImage         m_image          { VK_NULL_HANDLE };
    VkDeviceMemory  m_image_memory   { VK_NULL_HANDLE };

    static uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                     uint32_t type_filter,
                                     VkMemoryPropertyFlags properties);
};
