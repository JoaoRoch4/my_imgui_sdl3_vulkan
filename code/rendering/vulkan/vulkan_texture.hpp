#pragma once

#include "image_buffer.hpp" // img::ImageBuffer (CPU RGBA8 currency)
#include "vulkan_context.hpp"


// Owns a GPU-side RGBA texture loaded from disk via stb_image.
// Usage:
//   VulkanTexture tex;
//   tex.load("path/to/image.png", vk);
//   ImGui::Image(tex.imgui_id(), ImVec2(tex.width, tex.height));
//   tex.unload(vk);  // before ImGui_ImplVulkan_Shutdown
#include "pch.hpp"

class VulkanTexture {
	public:

		VulkanTexture();
		~VulkanTexture() = default;

		VulkanTexture(VulkanTexture const&)            = delete;
		VulkanTexture& operator=(VulkanTexture const&) = delete;

		VulkanTexture(VulkanTexture&&) noexcept;
		VulkanTexture& operator=(VulkanTexture&&) noexcept;

		// Load image from disk and upload to GPU. Returns false on failure.
		bool load(std::filesystem::path const& path, vulkan_context& vk);

		// Upload an already-decoded CPU RGBA8 buffer to the GPU (no file I/O, no decode).
		// Lets the render thread consume buffers produced by ImageJobSystem. False on failure.
		bool upload(img::ImageBuffer const& buf, vulkan_context& vk);

		// Upload already-compressed BC1/DXT1 blocks into a VK_FORMAT_BC1_RGBA_UNORM_BLOCK
		// image (no decode — the GPU samples the compressed data directly). blocks.size()
		// must equal ceil(w/4)*ceil(h/4)*8. False on size mismatch or any Vulkan failure.
		bool upload_bc1(std::span<std::byte const> blocks, int w, int h, vulkan_context& vk);

		// Free all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
		void unload(vulkan_context& vk);

		[[nodiscard]] bool is_loaded() const;

		// Pass to ImGui::Image().
		[[nodiscard]] ImTextureID imgui_id() const;

		int width;
		int height;

	private:

		static uint32_t
		find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties);

		// Create the GPU image/view/sampler, register with ImGui, and stage-upload the
		// given interleaved RGBA8 pixels (w*h*4 bytes). Caller owns `pixels`. Sets
		// width/height. Shared by load() (from a decoded file) and upload() (from a buffer).
		bool upload_pixels(unsigned char const* pixels, int w, int h, vulkan_context& vk);

		VkDescriptorSet m_descriptor_set;
		VkSampler       m_sampler;
		VkImageView     m_image_view;
		VkImage         m_image;
		VkDeviceMemory  m_image_memory;
		VkBuffer        m_upload_buffer;
		VkDeviceMemory  m_upload_buffer_memory;
};
