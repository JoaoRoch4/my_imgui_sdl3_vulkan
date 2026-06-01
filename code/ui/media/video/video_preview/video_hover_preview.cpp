
#include "pch.hpp" // NOLINT

#include "video_hover_preview.hpp"
#include "core/thread/thread_overwatch.hpp"
#include "vulkan_context.hpp"


#ifndef VIDEO_HOVER_DEBUG
    #ifdef NDEBUG
        #define VIDEO_HOVER_DEBUG 0
    #else
        #define VIDEO_HOVER_DEBUG 1
    #endif
#endif

#if VIDEO_HOVER_DEBUG
#define _Debug(fmt, ...) std::println("[VideoHoverPreview] " fmt, ##__VA_ARGS__)
#else
#define _Debug(fmt, ...) ((void)0)
#endif


#include <stb_image.h>
#include <stb_image_write.h>

namespace {

uint32_t find_memory_type(VkPhysicalDevice physical_device,
                          uint32_t type_filter,
                          VkMemoryPropertyFlags properties) {
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

VideoHoverPreview::VideoHoverPreview() {
    _Debug("ctor");
}

VideoHoverPreview::~VideoHoverPreview() {
    _Debug("dtor");
    shutdown();
}

void VideoHoverPreview::setup(vulkan_context *vk) {
    _Debug("setup");

    m_vk = vk;
    m_w  = static_cast<int>(preview_size.x);
    m_h  = static_cast<int>(preview_size.y);

    init_mpv();
    create_shared();
    start_thread();
    m_last_use_time = std::chrono::steady_clock::now();

    m_buf.resize(m_w * m_h * 4);
}

void VideoHoverPreview::shutdown() {
    _Debug("shutdown");

    if (!m_vk)
        return;

    stop_thread();

    if (m_render) {
        _Debug("free mpv render");
        mpv_render_context_free(m_render);
        m_render = nullptr;
    }

    if (m_mpv) {
        _Debug("destroy mpv");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }

    destroy_slot();
    destroy_shared();

    if (m_watchdog_restart_count > 0 || m_idle_stop_count > 0) {
        _Debug("summary restarts={} idle_stops={} last_restart_source='{}'",
               m_watchdog_restart_count,
               m_idle_stop_count,
               m_last_restart_source);
    }

    m_vk = nullptr;
}

// ============================================================
// MPV
// ============================================================

void VideoHoverPreview::init_mpv() {
    _Debug("init_mpv");

    m_mpv = mpv_create();

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "pause", "yes");
    mpv_set_option_string(m_mpv, "mute", preview_sound ? "no" : "yes");
    mpv_set_option_string(m_mpv, "loop-file", "inf");
    mpv_set_option_string(m_mpv, "hwdec", "nvdec");
    mpv_set_option_string(m_mpv, "hwdec-codecs", "h264,hevc,av1,vp9,mpeg4,vc1");
    mpv_set_option_string(m_mpv, "ytdl", "yes");
    mpv_set_option_string(m_mpv, "ytdl-format",
                          "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best");
    mpv_set_option_string(m_mpv, "cache", "no");
    // Use Lanczos downscaling when the video's native resolution exceeds preview_size.
    // Only activates when mpv scales *down*; upscaling is unaffected.
    mpv_set_option_string(m_mpv, "dscale", "lanczos");

    mpv_initialize(m_mpv);

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, static_cast<void *>(const_cast<char *>(MPV_RENDER_API_TYPE_SW))},
        {MPV_RENDER_PARAM_INVALID, nullptr}};

    mpv_render_context_create(&m_render, m_mpv, params);

    mpv_render_context_set_update_callback(
        m_render,
        [](void *ctx) {
            static_cast<VideoHoverPreview *>(ctx)->m_frame_dirty.store(true);
        },
        this);
}

// ============================================================
// Thread
// ============================================================

void VideoHoverPreview::start_thread() {
    _Debug("start_thread");

    if (m_thread.joinable())
        return;

    m_thread = std::jthread([this](const std::stop_token& st) { 
        _Debug("thread started");

        while (!st.stop_requested()) {
            const uint64_t watch_id = m_thread_watch_id.load(std::memory_order_acquire);
            ThreadOverwatch::instance().heartbeat(watch_id);

            mpv_event *ev = mpv_wait_event(m_mpv, 0.01);

            if (ev && ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                m_waiting.store(false);
                int64_t w = 0, h = 0;
                if (mpv_get_property(m_mpv, "width",  MPV_FORMAT_INT64, &w) == 0 &&
                    mpv_get_property(m_mpv, "height", MPV_FORMAT_INT64, &h) == 0 &&
                    w > 0 && h > 0) {
                    last_source_size = ImVec2{static_cast<float>(w), static_cast<float>(h)};
                }
            }

            if (!m_frame_dirty.exchange(false)) {
                continue;
            }

            if (m_waiting.load()) {
                continue;
            }

            std::lock_guard lock(m_buf_mutex);

            int size[2]  = {m_w, m_h};
            size_t stride = static_cast<size_t>(m_w) * 4;

            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_SW_SIZE,    size},
                {MPV_RENDER_PARAM_SW_FORMAT,  static_cast<void *>(const_cast<char *>("rgba"))},
                {MPV_RENDER_PARAM_SW_STRIDE,  &stride},
                {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
                {MPV_RENDER_PARAM_INVALID,    nullptr}};

            if (mpv_render_context_render(m_render, rp) >= 0) {
                m_pending_source = m_current;
                m_upload_pending.store(true, std::memory_order_release);
            }

            ThreadOverwatch::instance().heartbeat(watch_id);
        }

        _Debug("thread stopped");
    });

    if (m_thread_watch_id.load(std::memory_order_acquire) == 0) {
        const auto watch_id = ThreadOverwatch::instance().watch(
            "VideoHoverPreview::thread",
            std::chrono::milliseconds(5000),
            [this]() { stop_thread(false); },
            [this]() {
                if (m_vk != nullptr && m_mpv != nullptr && m_render != nullptr)
                    start_thread();
            });
        m_thread_watch_id.store(watch_id, std::memory_order_release);
    }

    ThreadOverwatch::instance().heartbeat(
        m_thread_watch_id.load(std::memory_order_acquire));
}

void VideoHoverPreview::stop_thread(bool unregister_watch) {
    _Debug("stop_thread");

    if (unregister_watch) {
        const uint64_t watch_id = m_thread_watch_id.exchange(0, std::memory_order_acq_rel);
        ThreadOverwatch::instance().unwatch(watch_id);
    }

    m_thread = std::jthread{};
}

// ============================================================
// Playback
// ============================================================

void VideoHoverPreview::load_source(const std::string &source) {
    using namespace std::chrono_literals;

    static std::string s_last_load_source;

    const auto now = std::chrono::steady_clock::now();
    if (source == m_current && m_waiting.load()) {
        // Avoid restarting loadfile every frame while waiting for first frame.
        if (m_last_load_time.time_since_epoch().count() != 0 &&
            (now - m_last_load_time) < 1200ms) {
            return;
        }
    }

    if (s_last_load_source != source) {
        _Debug("load {}", source);
        s_last_load_source = source;
    }

    m_current = source;
    m_waiting.store(true);
    m_last_load_time = now;

    const char *cmd[] = {
        "loadfile", source.c_str(), "replace", nullptr};

    mpv_command_async(m_mpv, 0, cmd);
}

void VideoHoverPreview::start_playback(const std::string &source) {
    _Debug("play {}", source);

    if (source != m_current)
        load_source(source);

    m_playing = source;
    mpv_set_property_string(m_mpv, "mute", preview_sound ? "no" : "yes");
    mpv_set_property_string(m_mpv, "pause", "no");
}

void VideoHoverPreview::stop_playback() {
    if (!m_playing.empty()) {
        _Debug("stop {}", m_playing);
        mpv_set_property_string(m_mpv, "pause", "yes");
        m_playing.clear();
    }
}

VkDescriptorSet VideoHoverPreview::thumbnail(const std::string &source) {
    if (!enabled)
        return VK_NULL_HANDLE;

    const auto now = std::chrono::steady_clock::now();
    m_last_use_time = now;

    // hover dwell delay
    if (source != m_hovered_source) {
        m_hovered_source = source;
        m_hover_start    = now;
        m_popup_reopen_requested.store(true, std::memory_order_release);
        return VK_NULL_HANDLE;
    }
    if ((now - m_hover_start) < hover_delay)
        return VK_NULL_HANDLE;

    if (!m_thread.joinable())
        start_thread();

    flush_pending_upload();

    // Reset slot when source changes
    if (m_slot_source != source) {
        destroy_slot();
        create_slot();
        m_slot_source = source;
    }

    if (!m_slot.has_frame) {
        load_source(source);
    } else {
        if (m_playing != source) {
            stop_playback();
            start_playback(source);
        }
    }

    return m_slot.has_frame ? m_slot.descriptor : VK_NULL_HANDLE;
}

bool VideoHoverPreview::is_hover_dwell_pending(const std::string &source) const {
    if (source != m_hovered_source)
        return true; // not yet registered — treat as pending
    return (std::chrono::steady_clock::now() - m_hover_start) < hover_delay;
}

void VideoHoverPreview::notify_hover(const std::string &source) {
    const auto now = std::chrono::steady_clock::now();
    if (source != m_hovered_source) {
        m_hovered_source = source;
        m_hover_start    = now;
        m_popup_reopen_requested.store(true, std::memory_order_release);
    }
}

bool VideoHoverPreview::consume_popup_reopen_request() {
    return m_popup_reopen_requested.exchange(false, std::memory_order_acq_rel);
}

void VideoHoverPreview::tick_idle() {
    if (!m_thread.joinable())
        return;

    const auto now = std::chrono::steady_clock::now();
    const bool can_restart_now =
        (m_last_restart_time.time_since_epoch().count() == 0 ||
         (now - m_last_restart_time) >= loading_restart_cooldown);

    const auto restart_hover_thread = [this, now]() {
        m_last_restart_time = now;
        ++m_watchdog_restart_count;
        m_last_restart_source = m_current;
        _Debug("watchdog restart #{} source='{}'",
               m_watchdog_restart_count,
               m_last_restart_source);
        m_popup_reopen_requested.store(true, std::memory_order_release);
        stop_thread();
        start_thread();
        m_waiting.store(false, std::memory_order_release);
        if (!m_current.empty())
            load_source(m_current);
    };

    if (m_waiting.load(std::memory_order_acquire) &&
        m_last_load_time.time_since_epoch().count() != 0 &&
        (now - m_last_load_time) >= loading_restart_timeout &&
        can_restart_now) {
        _Debug("loading timeout (>1s) -> restarting hover thread");
        restart_hover_thread();
        return;
    }

    if ((now - m_last_use_time) <= idle_thread_timeout &&
        !m_current.empty() &&
        m_last_load_time.time_since_epoch().count() != 0 &&
        (now - m_last_load_time) >= no_frame_restart_timeout &&
        m_waiting.load(std::memory_order_acquire) &&
        can_restart_now) {
        _Debug("no-frame timeout -> restarting hover thread");
        restart_hover_thread();
        return;
    }

    if (!m_slot.has_frame)
        return;
    if (m_last_use_time.time_since_epoch().count() == 0)
        return;
    if ((now - m_last_use_time) < idle_thread_timeout)
        return;

    ++m_idle_stop_count;
    _Debug("idle timeout -> stopping hover thread (count={})", m_idle_stop_count);
    m_popup_reopen_requested.store(true, std::memory_order_release);
    stop_playback();
    stop_thread();
}

// ============================================================
// GPU Upload
// ============================================================

void VideoHoverPreview::flush_pending_upload() {
    if (!m_upload_pending.load(std::memory_order_acquire))
        return;
    if (vkGetFenceStatus(m_vk->device, m_fence) == VK_NOT_READY)
        return;

    std::lock_guard lock(m_buf_mutex);
    if (m_pending_source.empty() || m_pending_source != m_slot_source || !m_slot.image) {
        m_upload_pending.store(false, std::memory_order_release);
        return;
    }

    vkResetFences(m_vk->device, 1, &m_fence);
    vkResetCommandBuffer(m_cmd, 0);

    std::memcpy(m_mapped, m_buf.data(), m_buf.size());

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmd, &begin);

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout     = m_slot.layout;
    to_transfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = 0;
    if (m_slot.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        src_stage                 = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    to_transfer.dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image           = m_slot.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(m_cmd, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {static_cast<uint32_t>(m_w), static_cast<uint32_t>(m_h), 1};

    vkCmdCopyBufferToImage(m_cmd, m_staging, m_slot.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_shader.oldLayout      = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask  = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask  = VK_ACCESS_SHADER_READ_BIT;
    to_shader.image          = m_slot.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_shader);

    vkEndCommandBuffer(m_cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &m_cmd;
    m_vk->queue_submit(1, &submit, m_fence);

    m_slot.layout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_slot.has_frame = true;
    m_pending_source.clear();
    m_upload_pending.store(false, std::memory_order_release);
}

// ============================================================
// SLOT
// ============================================================

bool VideoHoverPreview::create_slot() {
    _Debug("create_slot");

    VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img.imageType   = VK_IMAGE_TYPE_2D;
    img.format      = VK_FORMAT_R8G8B8A8_UNORM;
    img.extent      = {static_cast<uint32_t>(m_w), static_cast<uint32_t>(m_h), 1};
    img.mipLevels   = 1;
    img.arrayLayers = 1;
    img.samples     = VK_SAMPLE_COUNT_1_BIT;
    img.tiling      = VK_IMAGE_TILING_OPTIMAL;
    img.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    vkCreateImage(m_vk->device, &img, nullptr, &m_slot.image);

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_vk->device, m_slot.image, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        m_vk->physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == 0xFFFFFFFFu) {
        vkDestroyImage(m_vk->device, m_slot.image, nullptr);
        m_slot.image = VK_NULL_HANDLE;
        return false;
    }

    vkAllocateMemory(m_vk->device, &alloc, nullptr, &m_slot.memory);
    vkBindImageMemory(m_vk->device, m_slot.image, m_slot.memory, 0);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image            = m_slot.image;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = img.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_vk->device, &view, nullptr, &m_slot.view);

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(m_vk->device, &sampler, nullptr, &m_slot.sampler);

    m_slot.descriptor = ImGui_ImplVulkan_AddTexture(
        m_slot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_slot.layout    = VK_IMAGE_LAYOUT_UNDEFINED;
    m_slot.has_frame = false;

    return true;
}

void VideoHoverPreview::destroy_slot() {
    if (!m_slot.image)
        return;

    _Debug("destroy_slot");

    // Wait for any in-flight upload to finish before freeing GPU resources.
    if (m_fence)
        vkWaitForFences(m_vk->device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    m_upload_pending.store(false, std::memory_order_release);
    m_pending_source.clear();

    ImGui_ImplVulkan_RemoveTexture(m_slot.descriptor);
    vkDestroySampler(m_vk->device, m_slot.sampler, nullptr);
    vkDestroyImageView(m_vk->device, m_slot.view, nullptr);
    vkDestroyImage(m_vk->device, m_slot.image, nullptr);
    vkFreeMemory(m_vk->device, m_slot.memory, nullptr);

    m_slot        = {};
    m_slot_source.clear();
}

bool VideoHoverPreview::create_shared() {
    _Debug("create_shared");

    VkDevice device         = m_vk->device;
    VkDeviceSize size       = static_cast<VkDeviceSize>(m_w) * m_h * 4;

    VkBufferCreateInfo buf{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf.size  = size;
    buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &buf, nullptr, &m_staging);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, m_staging, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(
        m_vk->physical_device, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == 0xFFFFFFFFu)
        return false;

    vkAllocateMemory(device, &alloc, nullptr, &m_staging_mem);
    vkBindBufferMemory(device, m_staging, m_staging_mem, 0);
    vkMapMemory(device, m_staging_mem, 0, req.size, 0, &m_mapped);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = m_vk->queue_family;
    pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device, &pool, nullptr, &m_pool);

    VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd.commandPool        = m_pool;
    cmd.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmd, &m_cmd);

    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fence, nullptr, &m_fence);

    return true;
}

void VideoHoverPreview::destroy_shared() {
    _Debug("destroy_shared");

    VkDevice device = m_vk->device;

    if (m_fence)       vkDestroyFence(device, m_fence, nullptr);
    if (m_pool)        vkDestroyCommandPool(device, m_pool, nullptr);
    if (m_mapped)      vkUnmapMemory(device, m_staging_mem);
    if (m_staging)     vkDestroyBuffer(device, m_staging, nullptr);
    if (m_staging_mem) vkFreeMemory(device, m_staging_mem, nullptr);

    m_fence       = VK_NULL_HANDLE;
    m_pool        = VK_NULL_HANDLE;
    m_staging     = VK_NULL_HANDLE;
    m_staging_mem = VK_NULL_HANDLE;
    m_mapped      = nullptr;
}

bool VideoHoverPreview::save_frame(const std::filesystem::path &path) {
    _Debug("save {}", path.string());
    std::lock_guard lock(m_buf_mutex);

    // Reject mostly-black frames so the caller retries on the next frame.
    // Sample every 8th pixel (RGBA stride 4), compute average luma.
    if (!m_buf.empty()) {
        const int   total   = m_w * m_h;
        const int   step    = 8;
        uint64_t    sum     = 0;
        int         count   = 0;
        const auto *px      = m_buf.data();
        for (int i = 0; i < total; i += step) {
            const int base = i * 4;
            // BT.601 integer luma: (77*R + 150*G + 29*B) / 256
            sum += (77u  * px[base + 0] +
                    150u * px[base + 1] +
                    29u  * px[base + 2]) >> 8;
            ++count;
        }
        if (count > 0 && (sum / static_cast<uint64_t>(count)) < 8u)
            return false; // black frame — skip
    }

    bool res = static_cast<bool> (stbi_write_png(
               path.string().c_str(),
               m_w, m_h, 4,
               m_buf.data(),
               m_w * 4) != 0);
    return res;
}
