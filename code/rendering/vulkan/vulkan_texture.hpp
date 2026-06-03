#pragma once

#include "vulkan_context.hpp"


// Owns a GPU-side RGBA texture loaded from disk via stb_image.
// Usage:
//   VulkanTexture tex;
//   tex.load("path/to/image.png", vk);
//   ImGui::Image(tex.imgui_id(), ImVec2(tex.width, tex.height));
//   tex.unload(vk);  // before ImGui_ImplVulkan_Shutdown
#include "pch.hpp"

class VulkanTexture
{
public:
    VulkanTexture();
    ~VulkanTexture() = default;

    VulkanTexture(const VulkanTexture&)            = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VulkanTexture(VulkanTexture&&) noexcept;
    VulkanTexture& operator=(VulkanTexture&&) noexcept;

    // Load image from disk and upload to GPU. Returns false on failure.
    bool load(const std::filesystem::path& path, vulkan_context& vk);

    // Upload an already-decoded RGBA8 buffer (w*h*4 bytes) to the GPU.
    // Returns false on failure. Use this to avoid a disk round-trip.
    bool load_from_rgba(std::span<const std::uint8_t> rgba, int w, int h, vulkan_context& vk);

    // Free all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void unload(vulkan_context& vk);

    [[nodiscard]] bool is_loaded() const;

    // Pass to ImGui::Image().
    [[nodiscard]] ImTextureID imgui_id() const;

    int width;
    int height;

private:
    static uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                     uint32_t type_filter,
                                     VkMemoryPropertyFlags properties);

    VkDescriptorSet m_descriptor_set;
    VkSampler       m_sampler;
    VkImageView     m_image_view;
    VkImage         m_image;
    VkDeviceMemory  m_image_memory;
    VkBuffer        m_upload_buffer;
    VkDeviceMemory  m_upload_buffer_memory;
};
