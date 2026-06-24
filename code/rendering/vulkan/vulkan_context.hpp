#pragma once
#include "pch.hpp"

class vulkan_context {

    public:
    
	vulkan_context();

	VkAllocationCallbacks* allocator;
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	VkPipelineCache pipeline_cache;
	VkDescriptorPool descriptor_pool;
	VkBuffer vram_reserve_buffer;
	VkDeviceMemory vram_reserve_memory;
	VkDeviceSize vram_reserve_bytes;
	bool vram_reserve_active;

	ImGui_ImplVulkanH_Window main_window_data;
	uint32_t min_image_count;
	bool swap_chain_rebuild;

	/// True when the device was created with the Vulkan 1.2 features libplacebo
	/// requires for an imported device (hostQueryReset + timelineSemaphore).
	/// The libplacebo zero-copy video path is only attempted when this is set.
	bool placebo_features_enabled = false;

	/// True when the device supports BC (S3TC/DXT) block-compressed textures, i.e.
	/// the textureCompressionBC feature was present and enabled. The BC1 thumbnail
	/// cache backend is only used when this is set; otherwise it falls back to PNG.
	bool bc_textures_enabled = false;

	/// True when VK_EXT_memory_budget was enabled on the device, so available_vram_bytes()
	/// can report the live per-process budget instead of the static heap size.
	bool memory_budget_enabled = false;

	/// Best-effort largest device-local (VRAM) heap's currently-available bytes. Uses the
	/// VK_EXT_memory_budget live budget when present (accounts for other apps' usage),
	/// else the static heap size. Returns 0 if it cannot be determined. Used to auto-size
	/// the thumbnail image-decode resolution cap.
	[[nodiscard]] VkDeviceSize available_vram_bytes() const;

	void setup(std::vector<const char*> instance_extensions);
	void setup_window(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) const;
	void set_vsync(ImGui_ImplVulkanH_Window* wd, bool vsync);
	void resize_window(ImGui_ImplVulkanH_Window* wd, int width, int height);
	void cleanup();
	void cleanup_window(ImGui_ImplVulkanH_Window* wd) const;
	void frame_render(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, const ImVec4& clear_color);
	void frame_present(ImGui_ImplVulkanH_Window* wd);
	VkResult queue_submit(uint32_t submit_count, const VkSubmitInfo* submits, VkFence fence);
	VkResult queue_present(const VkPresentInfoKHR* present_info);

	static void check_result(VkResult err);

    private:
	bool is_extension_available(const std::vector<VkExtensionProperties>& properties, const char* extension);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
	VkDebugReportCallbackEXT debug_report_cb;
	static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_fn(
		VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
		uint64_t object, size_t location, int32_t messageCode,
		const char* pLayerPrefix, const char* pMessage, void* pUserData);
#endif

	std::mutex queue_mutex;
};
