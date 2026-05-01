#include "video_hover_preview.hpp"
#include "vulkan_context.hpp"
#include "imgui_impl_vulkan.h"
#include <chrono>
#include <cstring>
#include <print>
#include <stb_image_write.h>

#ifndef VIDEO_HOVER_DEBUG
    #define VIDEO_HOVER_DEBUG 1
#endif

#if VIDEO_HOVER_DEBUG
    #define _Debug(fmt, ...) std::println("[VideoHoverPreview] " fmt, ##__VA_ARGS__)
#else
    #define _Debug(fmt, ...) ((void)0)
#endif

namespace {

uint32_t find_memory_type(VkPhysicalDevice physical_device,
                          uint32_t type_filter,
                          VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool is_supported = (type_filter & (1u << i)) != 0;
        const bool has_properties =
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (is_supported && has_properties)
            return i;
    }

    return 0xFFFFFFFFu;
}

} // namespace

// ============================================================
// Lifecycle
// ============================================================

VideoHoverPreview::VideoHoverPreview()
{
    _Debug("ctor");
}

VideoHoverPreview::~VideoHoverPreview()
{
    _Debug("dtor");
    shutdown();
}

void VideoHoverPreview::setup(vulkan_context* vk)
{
    _Debug("setup");

    m_vk = vk;

    m_w = (int)preview_size.x;
    m_h = (int)preview_size.y;

    init_mpv();
    create_shared();
    start_thread();

    m_buf.resize(m_w * m_h * 4);
}

void VideoHoverPreview::shutdown()
{
    _Debug("shutdown");

    if (!m_vk) return;

    stop_thread();

    if (m_render) {
        _Debug("free mpv render");
        mpv_render_context_free(m_render);
    }

    if (m_mpv) {
        _Debug("destroy mpv");
        mpv_terminate_destroy(m_mpv);
    }

    while (!m_cache.empty()) {
        const std::string key = m_cache.begin()->first;
        _Debug("destroy slot {}", key);
        destroy_slot(key);
    }

    destroy_shared();

    m_cache.clear();
    m_lru.clear();
    m_vk = nullptr;
}

// ============================================================
// MPV
// ============================================================

void VideoHoverPreview::init_mpv()
{
    _Debug("init_mpv");

    m_mpv = mpv_create();

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "pause", "yes");
    mpv_set_option_string(m_mpv, "mute", "yes");
    mpv_set_option_string(m_mpv, "loop-file", "inf");

    mpv_initialize(m_mpv);

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_create(&m_render, m_mpv, params);

    mpv_render_context_set_update_callback(
        m_render,
        [](void* ctx){
            auto* self = static_cast<VideoHoverPreview*>(ctx);
            self->m_frame_dirty.store(true);
        },
        this
    );
}

// ============================================================
// Thread
// ============================================================

void VideoHoverPreview::start_thread()
{
    _Debug("start_thread");

    m_thread = std::jthread([this](std::stop_token st)
    {
        _Debug("thread started");

        while (!st.stop_requested())
        {
            mpv_event* ev = mpv_wait_event(m_mpv, 0.01);

            if (ev && ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                // Only emit when this event actually transitions us out of waiting.
                if (m_waiting.exchange(false))
                    _Debug("video reconfig");
            }

            if (!m_frame_dirty.exchange(false))
                continue;

            if (m_waiting.load())
                continue;

            std::lock_guard lock(m_buf_mutex);

            int size[2] = {m_w, m_h};
            size_t stride = m_w * 4;

            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_SW_SIZE, size},
                {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgba"},
                {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
                {MPV_RENDER_PARAM_INVALID, nullptr}
            };

            if (mpv_render_context_render(m_render, params) >= 0)
                upload_frame_async(m_current);
        }

        _Debug("thread stopped");
    });
}

void VideoHoverPreview::stop_thread()
{
    _Debug("stop_thread");
    m_thread = std::jthread{};
}

// ============================================================
// Playback
// ============================================================

void VideoHoverPreview::load_source(const std::string& source)
{
    using namespace std::chrono_literals;

    static std::string s_last_skip_waiting_source;
    static std::string s_last_load_source;

    const auto now = std::chrono::steady_clock::now();
    if (source == m_current && m_waiting.load()) {
        // Avoid restarting loadfile every frame while waiting for first frame.
        if (m_last_load_time.time_since_epoch().count() != 0 &&
            (now - m_last_load_time) < 1200ms) {
            if (s_last_skip_waiting_source != source) {
                _Debug("load skipped (waiting) {}", source);
                s_last_skip_waiting_source = source;
            }
            return;
        }
    }

    s_last_skip_waiting_source.clear();
    if (s_last_load_source != source) {
        _Debug("load {}", source);
        s_last_load_source = source;
    }

    m_current = source;
    m_waiting.store(true);
    m_last_load_time = now;

    const char* cmd[] = {
        "loadfile", source.c_str(), "replace", "0", "start=3", nullptr
    };

    mpv_command_async(m_mpv, 0, cmd);
}

void VideoHoverPreview::start_playback(const std::string& source)
{
    _Debug("play {}", source);

    if (source != m_current)
        load_source(source);

    m_playing = source;
    mpv_set_property_string(m_mpv, "pause", "no");
}

void VideoHoverPreview::stop_playback()
{
    if (!m_playing.empty()) {
        _Debug("stop {}", m_playing);
        mpv_set_property_string(m_mpv, "pause", "yes");
        m_playing.clear();
    }
}

// ============================================================
// GPU Upload
// ============================================================

void VideoHoverPreview::upload_frame_async(const std::string& source)
{
    auto it = m_cache.find(source);
    if (it == m_cache.end()) return;
    auto& slot = it->second;

    if (vkGetFenceStatus(m_vk->device, m_fence) == VK_NOT_READY)
        return;

    vkResetFences(m_vk->device, 1, &m_fence);
    vkResetCommandBuffer(m_cmd, 0);

    std::memcpy(m_mapped, m_buf.data(), m_buf.size());

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmd, &begin);

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout = slot.layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = 0;
    if (slot.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = slot.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(
        m_cmd,
        src_stage,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.imageExtent = {(uint32_t)m_w,(uint32_t)m_h,1};

    vkCmdCopyBufferToImage(
        m_cmd,
        m_staging,
        slot.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);

    VkImageMemoryBarrier to_shader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_shader.image = slot.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(
        m_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_shader);

    vkEndCommandBuffer(m_cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_cmd;

    m_vk->queue_submit(1, &submit, m_fence);

    slot.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    slot.has_frame = true;
    m_waiting.store(false);
}

// ============================================================
// CACHE + API (trimmed logs for sanity)
// ============================================================

VkDescriptorSet VideoHoverPreview::thumbnail(const std::string& source)
{
    if (!m_cache.contains(source)) {
        _Debug("cache miss {}", source);
        evict_if_needed();
        create_slot(source);
    }

    auto& slot = m_cache[source];
    touch_lru(source);

    if (!slot.has_frame) {
        load_source(source);
    } else {
        // This API is called only while drawing the hovered preview tooltip,
        // so treat calls as active hover and keep playback live.
        if (m_playing != source) {
            stop_playback();
            start_playback(source);
        }
    }

    return slot.has_frame ? slot.descriptor : VK_NULL_HANDLE;
}

// ============================================================
// SAVE
// ============================================================

bool VideoHoverPreview::save_frame(const std::filesystem::path& path)
{
    _Debug("save {}", path.string());

    std::lock_guard lock(m_buf_mutex);

    return stbi_write_png(
        path.string().c_str(),
        m_w, m_h, 4,
        m_buf.data(),
        m_w * 4
    ) != 0;
}

// ============================================================
// CACHE
// ============================================================

bool VideoHoverPreview::create_slot(const std::string& source)
{
    _Debug("create_slot {}", source);

    GpuSlot slot{};

    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.extent = {(uint32_t)m_w,(uint32_t)m_h,1};
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    vkCreateImage(m_vk->device, &img, nullptr, &slot.image);

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_vk->device, slot.image, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        m_vk->physical_device,
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == 0xFFFFFFFFu) {
        vkDestroyImage(m_vk->device, slot.image, nullptr);
        return false;
    }

    vkAllocateMemory(m_vk->device, &alloc, nullptr, &slot.memory);
    vkBindImageMemory(m_vk->device, slot.image, slot.memory, 0);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = slot.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = img.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};

    vkCreateImageView(m_vk->device, &view, nullptr, &slot.view);

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;

    vkCreateSampler(m_vk->device, &sampler, nullptr, &slot.sampler);

    slot.descriptor = ImGui_ImplVulkan_AddTexture(
        slot.sampler,
        slot.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_lru.push_front(source);
    slot.lru_it = m_lru.begin();
    slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    m_cache[source] = slot;

    return true;
}

void VideoHoverPreview::destroy_slot(const std::string& source)
{
    _Debug("destroy_slot {}", source);

    auto it = m_cache.find(source);
    if (it == m_cache.end()) return;

    auto& slot = it->second;

    ImGui_ImplVulkan_RemoveTexture(slot.descriptor);

    vkDestroySampler(m_vk->device, slot.sampler, nullptr);
    vkDestroyImageView(m_vk->device, slot.view, nullptr);
    vkDestroyImage(m_vk->device, slot.image, nullptr);
    vkFreeMemory(m_vk->device, slot.memory, nullptr);

    m_lru.erase(slot.lru_it);
    m_cache.erase(it);
}

void VideoHoverPreview::touch_lru(const std::string& source)
{
    auto& slot = m_cache[source];

    m_lru.erase(slot.lru_it);
    m_lru.push_front(source);

    slot.lru_it = m_lru.begin();
}

void VideoHoverPreview::evict_if_needed()
{
    while (m_cache.size() >= max_cache_size)
    {
        _Debug("evict {}", m_lru.back());
        destroy_slot(m_lru.back());
    }
}

bool VideoHoverPreview::create_shared()
{
    _Debug("create_shared");

    VkDevice device = m_vk->device;
    VkDeviceSize size = m_w * m_h * 4;

    VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf.size = size;
    buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    vkCreateBuffer(device, &buf, nullptr, &m_staging);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, m_staging, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        m_vk->physical_device,
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == 0xFFFFFFFFu)
        return false;

    vkAllocateMemory(device, &alloc, nullptr, &m_staging_mem);
    vkBindBufferMemory(device, m_staging, m_staging_mem, 0);

    vkMapMemory(device, m_staging_mem, 0, req.size, 0, &m_mapped);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = m_vk->queue_family;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    vkCreateCommandPool(device, &pool, nullptr, &m_pool);

    VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd.commandPool = m_pool;
    cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd.commandBufferCount = 1;

    vkAllocateCommandBuffers(device, &cmd, &m_cmd);

    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    vkCreateFence(device, &fence, nullptr, &m_fence);

    return true;
}

void VideoHoverPreview::destroy_shared()
{
    _Debug("destroy_shared");

    VkDevice device = m_vk->device;

    if (m_fence)
        vkDestroyFence(device, m_fence, nullptr);

    if (m_pool)
        vkDestroyCommandPool(device, m_pool, nullptr);

    if (m_mapped)
        vkUnmapMemory(device, m_staging_mem);

    if (m_staging)
        vkDestroyBuffer(device, m_staging, nullptr);

    if (m_staging_mem)
        vkFreeMemory(device, m_staging_mem, nullptr);

    m_fence = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_staging = VK_NULL_HANDLE;
    m_staging_mem = VK_NULL_HANDLE;
    m_mapped = nullptr;
}