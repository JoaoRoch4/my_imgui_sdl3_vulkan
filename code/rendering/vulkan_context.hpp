#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <vector>

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

class vulkan_context {
public:
    vulkan_context();

    VkAllocationCallbacks *allocator;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkPipelineCache pipeline_cache;
    VkDescriptorPool descriptor_pool;

    ImGui_ImplVulkanH_Window main_window_data;
    uint32_t min_image_count;
    bool swap_chain_rebuild;

    void setup(std::vector<const char *> instance_extensions);
    void setup_window(ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int width, int height) const;
    void set_vsync(ImGui_ImplVulkanH_Window *wd, bool vsync);
    void resize_window(ImGui_ImplVulkanH_Window *wd, int width, int height);
    void cleanup();
    void cleanup_window(ImGui_ImplVulkanH_Window *wd) const;
    void frame_render(ImGui_ImplVulkanH_Window *wd, ImDrawData *draw_data, const ImVec4 &clear_color);
    void frame_present(ImGui_ImplVulkanH_Window *wd);

    static void check_result(VkResult err);

private:
    bool is_extension_available(const std::vector<VkExtensionProperties> &properties, const char *extension);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT debug_report_cb;
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_fn(
        VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
        uint64_t object, size_t location, int32_t messageCode,
        const char *pLayerPrefix, const char *pMessage, void *pUserData);
#endif
};
