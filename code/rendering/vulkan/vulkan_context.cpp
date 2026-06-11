#include "pch.hpp"

#include "vulkan_context.hpp"
#include "debug_log.hpp"



vulkan_context::vulkan_context()
    : allocator(nullptr)
    , instance(VK_NULL_HANDLE)
    , physical_device(VK_NULL_HANDLE)
    , device(VK_NULL_HANDLE)
    , queue_family(static_cast<uint32_t>(-1))
    , queue(VK_NULL_HANDLE)
    , pipeline_cache(VK_NULL_HANDLE)
    , descriptor_pool(VK_NULL_HANDLE)
    , vram_reserve_buffer(VK_NULL_HANDLE)
    , vram_reserve_memory(VK_NULL_HANDLE)
    , vram_reserve_bytes(0)
    , vram_reserve_active(false)
    , min_image_count(2)
    , swap_chain_rebuild(false)
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    , debug_report_cb(VK_NULL_HANDLE)
#endif
{
}

void vulkan_context::check_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_context::debug_report_fn(
    VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
    uint64_t object, size_t location, int32_t messageCode,
    const char *pLayerPrefix, const char *pMessage, void *pUserData) {
    (void)flags;
    (void)object;
    (void)location;
    (void)messageCode;
    (void)pUserData;
    (void)pLayerPrefix;
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif

bool vulkan_context::is_extension_available(const std::vector<VkExtensionProperties> &properties, const char *extension) {
    for (const VkExtensionProperties &p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

void vulkan_context::setup(std::vector<const char *> instance_extensions) {
    VkResult err;

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    APP_DEBUG_LOG("[vulkan_context] setup begin");

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Request Vulkan 1.2 so the device may enable the promoted 1.2 feature
        // structs (hostQueryReset / timelineSemaphore) that libplacebo's
        // imported-device path requires.
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.apiVersion = VK_API_VERSION_1_2;
        create_info.pApplicationInfo = &app_info;

        uint32_t properties_count;
        std::vector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.data());
        check_result(err);

        if (is_extension_available(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (is_extension_available(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        std::array<const char *, 1> layers = {"VK_LAYER_KHRONOS_validation"};
        create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        APP_DEBUG_LOG("[vulkan_context] instance extensions requested: {}", instance_extensions.size());
        for (const char *ext : instance_extensions) {
            (void)ext;
            APP_DEBUG_LOG("[vulkan_context]   instance ext: {}", ext ? ext : "<null>");
        }

        create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        create_info.ppEnabledExtensionNames = instance_extensions.data();
        err = vkCreateInstance(&create_info, allocator, &instance);
        check_result(err);
        APP_DEBUG_LOG("[vulkan_context] vkCreateInstance OK");

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(instance);
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report_fn;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(instance, &debug_report_ci, allocator, &debug_report_cb);
        check_result(err);
#endif
    }

    // Select Physical Device (GPU)
    physical_device = ImGui_ImplVulkanH_SelectPhysicalDevice(instance);
    IM_ASSERT(physical_device != VK_NULL_HANDLE);
    APP_DEBUG_LOG("[vulkan_context] selected physical device: {}", static_cast<const void *>(physical_device));

    // Select graphics queue family
    queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device);
    IM_ASSERT(queue_family != static_cast<uint32_t>(-1));
    APP_DEBUG_LOG("[vulkan_context] selected queue family: {}", queue_family);

    // Create Logical Device
    {
        std::vector<const char *> requested_device_extensions;
        requested_device_extensions.push_back("VK_KHR_swapchain");
        // Required for Vulkan ↔ OpenGL/libplacebo zero-copy interop
        requested_device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        requested_device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        requested_device_extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        requested_device_extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

        uint32_t properties_count;
        std::vector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &properties_count, properties.data());
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (is_extension_available(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            requested_device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        std::vector<const char *> device_extensions;
        for (const char *ext : requested_device_extensions) {
            if (!ext)
                continue;

            if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                APP_DEBUG_LOG("[vulkan_context] skip invalid device extension: {}", ext);
                continue;
            }

            if (!is_extension_available(properties, ext)) {
                APP_DEBUG_LOG("[vulkan_context] skip unavailable device extension: {}", ext);
                continue;
            }

            device_extensions.push_back(ext);
        }

        APP_DEBUG_LOG("[vulkan_context] device extensions enabled: {}", device_extensions.size());
        for (const char *ext : device_extensions) {
            (void)ext;
            APP_DEBUG_LOG("[vulkan_context]   device ext: {}", ext ? ext : "<null>");
        }

        std::array<float, 1> queue_priority = {1.0f};
        std::array<VkDeviceQueueCreateInfo, 1> queue_info = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = queue_family;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority.data();
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_info.size());
        create_info.pQueueCreateInfos = queue_info.data();
        create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        create_info.ppEnabledExtensionNames = device_extensions.data();

        // libplacebo's imported-device path (zero-copy video) requires the
        // Vulkan 1.2 features hostQueryReset and timelineSemaphore.  Query
        // support first and only enable them when present, so the application
        // still runs (via the software video path) on GPUs/drivers that lack
        // them rather than failing vkCreateDevice outright.
        VkPhysicalDeviceVulkan12Features vk12_supported = {};
        vk12_supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 features_query = {};
        features_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features_query.pNext = &vk12_supported;
        vkGetPhysicalDeviceFeatures2(physical_device, &features_query);

        VkPhysicalDeviceVulkan12Features vk12_enable = {};
        vk12_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        if (vk12_supported.hostQueryReset && vk12_supported.timelineSemaphore) {
            vk12_enable.hostQueryReset   = VK_TRUE;
            vk12_enable.timelineSemaphore = VK_TRUE;
            create_info.pNext = &vk12_enable; // pEnabledFeatures must stay null
            placebo_features_enabled = true;
            APP_DEBUG_LOG("[vulkan_context] libplacebo features enabled "
                          "(hostQueryReset + timelineSemaphore)");
        } else {
            APP_DEBUG_LOG("[vulkan_context] libplacebo features unavailable — "
                          "zero-copy video path disabled");
        }

        err = vkCreateDevice(physical_device, &create_info, allocator, &device);
        check_result(err);
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        APP_DEBUG_LOG("[vulkan_context] vkCreateDevice OK");
    }

    // Create Descriptor Pool
    {

        // Reserve a chunk of VRAM up front to keep memory headroom predictable
        // for this application workload. Best-effort only: failure is logged.
        constexpr VkDeviceSize k_vram_reserve_size =
            static_cast<VkDeviceSize>(2ULL) * 1024ULL * 1024ULL;

        VkBufferCreateInfo reserve_buffer_info = {};
        reserve_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        reserve_buffer_info.size = k_vram_reserve_size;
        reserve_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        reserve_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult reserve_err = vkCreateBuffer(device, &reserve_buffer_info, allocator, &vram_reserve_buffer);
        if (reserve_err == VK_SUCCESS) {
            VkMemoryRequirements reserve_mem_req{};
            vkGetBufferMemoryRequirements(device, vram_reserve_buffer, &reserve_mem_req);

            VkPhysicalDeviceMemoryProperties mem_props{};
            vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

            auto find_memory_type = [&](uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *out_index) {
                for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
                    const bool type_supported = (type_bits & (1u << i)) != 0;
                    const bool flags_match = (mem_props.memoryTypes[i].propertyFlags & required) == required;
                    if (type_supported && flags_match) {
                        *out_index = i;
                        return true;
                    }
                }
                return false;
            };

            uint32_t memory_type_index = 0;
            bool memory_type_found =
                find_memory_type(reserve_mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index);
            if (!memory_type_found)
                memory_type_found = find_memory_type(reserve_mem_req.memoryTypeBits, 0, &memory_type_index);

            if (memory_type_found) {
                VkMemoryAllocateInfo reserve_alloc_info = {};
                reserve_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                reserve_alloc_info.allocationSize = reserve_mem_req.size;
                reserve_alloc_info.memoryTypeIndex = memory_type_index;

                reserve_err = vkAllocateMemory(device, &reserve_alloc_info, allocator, &vram_reserve_memory);
                if (reserve_err == VK_SUCCESS) {
                    reserve_err = vkBindBufferMemory(device, vram_reserve_buffer, vram_reserve_memory, 0);
                    if (reserve_err == VK_SUCCESS) {
                        vram_reserve_bytes = reserve_mem_req.size;
                        vram_reserve_active = true;
                        APP_DEBUG_LOG("[vulkan_context] reserved VRAM bytes={}",
                                      static_cast<unsigned long long>(vram_reserve_bytes));
                    }
                }
            } else {
                reserve_err = VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }

        if (!vram_reserve_active) {
            if (vram_reserve_memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, vram_reserve_memory, allocator);
                vram_reserve_memory = VK_NULL_HANDLE;
            }
            if (vram_reserve_buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, vram_reserve_buffer, allocator);
                vram_reserve_buffer = VK_NULL_HANDLE;
            }
            vram_reserve_bytes = 0;
            APP_DEBUG_LOG("[vulkan_context] VRAM reserve skipped (2GB unavailable)");
        }

        // ImGui 1.92's Vulkan backend allocates a separate SAMPLER descriptor (its
        // global sampler) plus a SAMPLED_IMAGE descriptor per texture via
        // ImGui_ImplVulkan_AddTexture(view, layout). The pool MUST therefore carry
        // those types — a COMBINED_IMAGE_SAMPLER-only pool yields OUT_OF_POOL_MEMORY on
        // strict drivers. COMBINED_IMAGE_SAMPLER is kept for any legacy use.
        std::array<VkDescriptorPoolSize, 3> pool_sizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024}};
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // Crucial: Allows freeing sets (RemoveTexture) so evicted thumbnails recycle.
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 8192;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();

        err = vkCreateDescriptorPool(device, &pool_info, allocator, &descriptor_pool);
        check_result(err);
    }

    APP_DEBUG_LOG("[vulkan_context] setup done");
}

void vulkan_context::setup_window(ImGui_ImplVulkanH_Window *wd, VkSurfaceKHR surface, int width, int height) const {
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family, surface, &res);
    if (res != VK_TRUE) {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    std::array<VkFormat, 4> requestSurfaceImageFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->Surface = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(physical_device, wd->Surface, requestSurfaceImageFormat.data(), requestSurfaceImageFormat.size(), requestSurfaceColorSpace);

#ifdef APP_USE_UNLIMITED_FRAME_RATE
    std::array<VkPresentModeKHR, 3> present_modes = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
#else
    std::array<VkPresentModeKHR, 1> present_modes = {VK_PRESENT_MODE_FIFO_KHR};
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_device, wd->Surface, present_modes.data(), static_cast<int>(present_modes.size()));

    IM_ASSERT(min_image_count >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, wd, queue_family, allocator, width, height, min_image_count, 0);
}

void vulkan_context::set_vsync(ImGui_ImplVulkanH_Window *wd, bool vsync) {
    if (vsync) {
        std::array<VkPresentModeKHR, 1> modes = {VK_PRESENT_MODE_FIFO_KHR};
        wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_device, wd->Surface, modes.data(), static_cast<int>(modes.size()));
    } else {
        std::array<VkPresentModeKHR, 3> modes = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
        wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_device, wd->Surface, modes.data(), static_cast<int>(modes.size()));
    }
    swap_chain_rebuild = true;
}

void vulkan_context::resize_window(ImGui_ImplVulkanH_Window *wd, int width, int height) {
    ImGui_ImplVulkan_SetMinImageCount(min_image_count);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, wd, queue_family, allocator, width, height, min_image_count, 0);
    wd->FrameIndex = 0;
    swap_chain_rebuild = false;
}

void vulkan_context::cleanup() {
    APP_DEBUG_LOG("[vulkan_context] cleanup begin");

    if (vram_reserve_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, vram_reserve_memory, allocator);
        vram_reserve_memory = VK_NULL_HANDLE;
    }
    if (vram_reserve_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vram_reserve_buffer, allocator);
        vram_reserve_buffer = VK_NULL_HANDLE;
    }
    vram_reserve_active = false;
    vram_reserve_bytes = 0;

    vkDestroyDescriptorPool(device, descriptor_pool, allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(instance, debug_report_cb, allocator);
#endif

    vkDestroyDevice(device, allocator);
    vkDestroyInstance(instance, allocator);
    APP_DEBUG_LOG("[vulkan_context] cleanup done");
}

void vulkan_context::cleanup_window(ImGui_ImplVulkanH_Window *wd) const {
    ImGui_ImplVulkanH_DestroyWindow(instance, device, wd, allocator);
    vkDestroySurfaceKHR(instance, wd->Surface, allocator);
}

void vulkan_context::frame_render(ImGui_ImplVulkanH_Window *wd, ImDrawData *draw_data, const ImVec4 &clear_color) {
    wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
    wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
    wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
    wd->ClearValue.color.float32[3] = clear_color.w;

    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[static_cast<int>(wd->SemaphoreIndex)].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[static_cast<int>(wd->SemaphoreIndex)].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        swap_chain_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_result(err);

    ImGui_ImplVulkanH_Frame *fd = &wd->Frames[static_cast<int>(wd->FrameIndex)];
    {
        err = vkWaitForFences(device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_result(err);
        err = vkResetFences(device, 1, &fd->Fence);
        check_result(err);
    }
    {
        err = vkResetCommandPool(device, fd->CommandPool, 0);
        check_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;
        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_result(err);
        err = queue_submit(1, &info, fd->Fence);
        check_result(err);
    }
}

void vulkan_context::frame_present(ImGui_ImplVulkanH_Window *wd) {
    if (swap_chain_rebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[static_cast<int>(wd->SemaphoreIndex)].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = queue_present(&info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        swap_chain_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

VkResult vulkan_context::queue_submit(uint32_t submit_count, const VkSubmitInfo *submits, VkFence fence) {
    std::scoped_lock lock(queue_mutex);
    return vkQueueSubmit(queue, submit_count, submits, fence);
}

VkResult vulkan_context::queue_present(const VkPresentInfoKHR *present_info) {
    std::scoped_lock lock(queue_mutex);
    return vkQueuePresentKHR(queue, present_info);
}
