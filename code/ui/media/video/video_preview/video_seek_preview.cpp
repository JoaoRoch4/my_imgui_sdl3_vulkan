#include "video_seek_preview.hpp"
#include "vulkan_upload_context.hpp"
#include "vulkan_context.hpp"

#include "imgui_impl_vulkan.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <print>

#ifndef VIDEO_SEEK_DEBUG
    #define VIDEO_SEEK_DEBUG 1
#endif

#if VIDEO_SEEK_DEBUG
    #define _SeekDebug(fmt, ...) std::println("[VideoSeekPreview] " fmt, ##__VA_ARGS__)
#else
    #define _SeekDebug(fmt, ...) ((void)0)
#endif

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t filter,
                          VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);

    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) &&
            ((mem.memoryTypes[i].propertyFlags & props) == props))
            return i;

    return 0xFFFFFFFFu;
}

}

// ============================================================================
// Lifecycle
// ============================================================================

VideoSeekPreview::VideoSeekPreview() = default;

VideoSeekPreview::~VideoSeekPreview()
{
    shutdown();
}

void VideoSeekPreview::setup(vulkan_context* vk,
                            VulkanUploadContext* uploader,
                            const std::string& source)
{
    _SeekDebug("setup source={}", source);

    m_vk = vk;
    m_uploader = uploader;

    init_mpv(source);
    create_gpu_resources(vk);
    start_thread();
}

void VideoSeekPreview::stop_thread()
{
    _SeekDebug("stop_thread");
    m_thread = std::jthread{};
}

void VideoSeekPreview::shutdown()
{
    _SeekDebug("shutdown");
    stop_thread();

    if (m_render_ctx) {
        mpv_render_context_free(m_render_ctx);
        m_render_ctx = nullptr;
    }

    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }

    destroy_gpu_resources(m_vk);

    m_vk = nullptr;
    m_uploader = nullptr;
}

// ============================================================================
// MPV
// ============================================================================

void VideoSeekPreview::init_mpv(const std::string& source)
{
    _SeekDebug("init_mpv source={}", source);
    m_mpv = mpv_create();
    if (!m_mpv) return;

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "pause", "yes");
    mpv_set_option_string(m_mpv, "hr-seek", "yes");

    if (mpv_initialize(m_mpv) < 0)
        return;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    mpv_render_context_create(&m_render_ctx, m_mpv, params);

    mpv_render_context_set_update_callback(
        m_render_ctx,
        [](void* ctx) {
            auto* self = static_cast<VideoSeekPreview*>(ctx);
            self->m_frame_dirty.store(true, std::memory_order_release);
        },
        this);

    const char* cmd[] = {"loadfile", source.c_str(), nullptr};
    mpv_command_async(m_mpv, 0, cmd);
}

// ============================================================================
// GPU
// ============================================================================

bool VideoSeekPreview::create_gpu_resources(vulkan_context* vk)
{
    m_w = (int)preview_size.x;
    m_h = (int)preview_size.y;
    _SeekDebug("create_gpu_resources {}x{}", m_w, m_h);

    VkDevice device = vk->device;

    // Image
    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.extent = {(uint32_t)m_w, (uint32_t)m_h, 1};
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    vkCreateImage(device, &img, nullptr, &m_image);

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, m_image, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        vk->physical_device,
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &alloc, nullptr, &m_image_memory);
    vkBindImageMemory(device, m_image, m_image_memory, 0);

    // View
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = m_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = img.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCreateImageView(device, &view, nullptr, &m_image_view);

    // Sampler
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;

    vkCreateSampler(device, &samp, nullptr, &m_sampler);

    m_descriptor_set = ImGui_ImplVulkan_AddTexture(
        m_sampler,
        m_image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // CPU buffer
    m_buf.resize(m_w * m_h * 4);

    return true;
}

void VideoSeekPreview::destroy_gpu_resources(vulkan_context* vk)
{
    _SeekDebug("destroy_gpu_resources");
    if (!vk) return;

    VkDevice d = vk->device;

    if (m_descriptor_set)
        ImGui_ImplVulkan_RemoveTexture(m_descriptor_set);

    if (m_sampler)
        vkDestroySampler(d, m_sampler, nullptr);

    if (m_image_view)
        vkDestroyImageView(d, m_image_view, nullptr);

    if (m_image)
        vkDestroyImage(d, m_image, nullptr);

    if (m_image_memory)
        vkFreeMemory(d, m_image_memory, nullptr);

    m_descriptor_set = VK_NULL_HANDLE;
}

// ============================================================================
// THREAD
// ============================================================================

void VideoSeekPreview::start_thread()
{
    _SeekDebug("start_thread");
    m_thread = std::jthread([this](std::stop_token stoken) {

        while (!stoken.stop_requested()) {

            double req = m_seek_req.exchange(-1.0);

            if (req >= 0.0 && m_mpv && m_render_ctx) {

                char t[64];
                snprintf(t, sizeof(t), "%.4f", req);

                const char* cmd[] = {"seek", t, "absolute+exact", nullptr};
                mpv_command_async(m_mpv, 0, cmd);

                m_frame_dirty = false;

                for (int i = 0; i < 50; ++i) {
                    mpv_wait_event(m_mpv, 0.01);
                    if (m_frame_dirty) break;
                }

                if (m_frame_dirty) {

                    int size[2] = {m_w, m_h};
                    size_t stride = m_w * 4;

                    std::lock_guard lock(m_buf_mutex);

                    mpv_render_param params[] = {
                        {MPV_RENDER_PARAM_SW_SIZE, size},
                        {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgba"},
                        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                        {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
                        {MPV_RENDER_PARAM_INVALID, nullptr},
                    };

                    if (mpv_render_context_render(m_render_ctx, params) >= 0)
                        m_buf_ready = true;
                }
            }

            mpv_wait_event(m_mpv, 0.01);
        }
    });
}

// ============================================================================
// UPDATE (IMPORTANT PART)
// ============================================================================

void VideoSeekPreview::update()
{
    if (!m_buf_ready.exchange(false))
        return;

    if (!m_uploader)
        return;

    std::lock_guard lock(m_buf_mutex);
    m_uploader->upload_to_image(
        m_buf.data(),
        m_image,
        (uint32_t)m_w,
        (uint32_t)m_h);
}

// ============================================================================

void VideoSeekPreview::seek(double t)
{
    m_seek_req.store(t);
}