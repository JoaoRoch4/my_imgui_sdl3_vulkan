#include "video_player.hpp"
#include "video_context_menu.hpp"
#include "history_preview.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <unordered_set>

// ============================================================================
// Extension sets
// ============================================================================

namespace {

const std::unordered_set<std::string> k_video_exts = {
    ".mp4",
    ".mkv",
    ".avi",
    ".mov",
    ".webm",
    ".flv",
    ".wmv",
    ".m4v",
    ".ts",
    ".m2ts",
    ".mpeg",
    ".mpg",
    ".ogv",
    ".3gp",
    ".rm",
    ".rmvb",
    ".divx",
    ".xvid",
    ".gif",
};

const std::unordered_set<std::string> k_audio_exts = {
    ".mp3",
    ".flac",
    ".ogg",
    ".wav",
    ".aac",
    ".opus",
    ".m4a",
    ".wma",
    ".ac3",
    ".dts",
    ".tta",
    ".wv",
};

// Image extensions that should NOT be routed to the video player.
const std::unordered_set<std::string> k_image_exts = {
    ".jpg",
    ".jpeg",
    ".png",
    ".bmp",
    ".tga",
    ".webp",
};

// Hostnames whose URLs mpv/yt-dlp can stream directly.
const std::unordered_set<std::string> k_streaming_hosts = {
    "youtube.com",
    "www.youtube.com",
    "youtu.be",
    "vimeo.com",
    "www.vimeo.com",
    "twitch.tv",
    "www.twitch.tv",
    "dailymotion.com",
    "www.dailymotion.com",
    "reddit.com",
    "www.reddit.com",
    "v.redd.it",
    "twitter.com",
    "www.twitter.com",
    "x.com",
    "tiktok.com",
    "www.tiktok.com",
    "streamable.com",
    "www.streamable.com",
    "nicovideo.jp",
    "www.nicovideo.jp",
    "bilibili.com",
    "www.bilibili.com",
    "soundcloud.com",
    "www.soundcloud.com",
    "bandcamp.com",
};

} // namespace

// ============================================================================
// VideoEntry constructor
// ============================================================================

VideoPlayer::VideoEntry::VideoEntry()
    : mpv{nullptr}
    , render_ctx{nullptr}
    , frame_dirty{false}
    , video_w{0}
    , video_h{0}
    , pixel_buf{}
    , image{VK_NULL_HANDLE}
    , image_memory{VK_NULL_HANDLE}
    , image_view{VK_NULL_HANDLE}
    , sampler{VK_NULL_HANDLE}
    , descriptor_set{VK_NULL_HANDLE}
    , staging_buf{VK_NULL_HANDLE}
    , staging_mem{VK_NULL_HANDLE}
    , staging_mapped{nullptr}
    , cmd_pool{VK_NULL_HANDLE}
    , cmd_buf{VK_NULL_HANDLE}
    , upload_fence{VK_NULL_HANDLE}
    , title{}
    , source{}
    , kind{}
    , id{0}
    , open{true}
    , fullscreen{false}
    , loop{false}
{
}

// ============================================================================
// VideoPlayer constructor / destructor
// ============================================================================

VideoPlayer::VideoPlayer()
    : m_vk{nullptr}
    , m_entries{}
    , m_next_id{0}
    , m_ctx_menu{nullptr}
    , m_ctx_lookup{}
    , m_ctx_on_erase{}
    , m_on_open_image{}
    , m_on_open_online{}
    , m_on_open_recent{}
    , m_history_provider{}
    , m_history_preview{nullptr}
{
}

VideoPlayer::~VideoPlayer() {
    if (m_vk)
        shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

void VideoPlayer::setup(vulkan_context *vk) {
    std::println("[VideoPlayer] setup");
    m_vk = vk;
    constexpr VkDeviceSize k_max_seek_preview_bytes =
        static_cast<VkDeviceSize>(1920) * 1920 * 4;
    m_seek_uploader.init(m_vk,
                         static_cast<size_t>(k_max_seek_preview_bytes));
    m_hover.setup(vk);
}

void VideoPlayer::shutdown() {
    std::println("[VideoPlayer] shutdown begin");
    if (!m_vk)
        return;

    // Stop all background threads before vkDeviceWaitIdle
    m_hover.stop_thread();
    for (auto &ep : m_entries)
        ep->seek_preview.stop_thread();

    vkDeviceWaitIdle(m_vk->device);

    for (auto &ep : m_entries) {
        if (ep->render_ctx) {
            mpv_render_context_free(ep->render_ctx);
            ep->render_ctx = nullptr;
        }
        destroy_gpu_resources(*ep);
        if (ep->mpv) {
            mpv_terminate_destroy(ep->mpv);
            ep->mpv = nullptr;
        }
        ep->seek_preview.shutdown();
    }
    m_entries.clear();

    m_hover.shutdown();
    m_seek_uploader.shutdown();

    m_vk = nullptr;
    std::println("[VideoPlayer] shutdown done");
}

// ============================================================================
// Static helpers
// ============================================================================

bool VideoPlayer::is_video_path(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    return k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0;
}

bool VideoPlayer::is_video_url(const std::string &url) {
    // 1. Known streaming hostnames — always route to mpv regardless of extension
    const auto host_start = url.find("://");
    if (host_start != std::string::npos) {
        const auto path_start = url.find('/', host_start + 3);
        const std::string host = url.substr(
            host_start + 3,
            path_start == std::string::npos ? std::string::npos : path_start - (host_start + 3));
        if (k_streaming_hosts.count(host) > 0)
            return true;
    }

    // 2. Check extension (strip query string first)
    const auto clean = url.substr(0, url.find('?'));
    const auto ext = std::filesystem::path(clean).extension().string();
    if (k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0)
        return true;

    // 3. Any http/https URL that doesn't look like a still image → let mpv try
    const bool is_http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    if (is_http && k_image_exts.count(ext) == 0 && ext.empty())
        return true;

    return false;
}

uint32_t VideoPlayer::find_memory_type(VkPhysicalDevice phys,
                                       uint32_t filter,
                                       VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && ((mem.memoryTypes[i].propertyFlags & props) == props))
            return i;
    return 0xFFFFFFFFu;
}

// ============================================================================
// Add entries
// ============================================================================

bool VideoPlayer::add_from_path(const std::filesystem::path &path) {
    std::println("[VideoPlayer] add_from_path: {}", path.string());
    auto ep = std::make_unique<VideoEntry>();
    ep->title = path.filename().string();
    ep->source = path.string();
    ep->kind = (path.extension().string() == ".gif") ? "gif" : "video";
    ep->id = m_next_id++;

    ep->mpv = mpv_create();
    if (!ep->mpv)
        return false;

    mpv_set_option_string(ep->mpv, "hwdec", "yes");
    mpv_set_option_string(ep->mpv, "vo", "libmpv");
    if (ep->kind == "gif") {
        mpv_set_option_string(ep->mpv, "loop-file", "inf");
        ep->loop = true;
    }

    if (mpv_initialize(ep->mpv) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&ep->render_ctx, ep->mpv, params) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    VideoEntry *raw = ep.get();
    mpv_render_context_set_update_callback(
        ep->render_ctx,
        [](void *ctx) {
            static_cast<VideoEntry *>(ctx)->frame_dirty.store(
                true, std::memory_order_release);
        },
        raw);

    const char *cmd[] = {"loadfile", ep->source.c_str(), nullptr};
    mpv_command_async(ep->mpv, 0, cmd);

    ep->seek_preview.setup(m_vk, &m_seek_uploader, ep->source);

    m_entries.push_back(std::move(ep));
    return true;
}

bool VideoPlayer::add_from_url(const std::string &url, const std::string &title) {
    std::println("[VideoPlayer] add_from_url: {} (title='{}')", url, title);
    auto ep = std::make_unique<VideoEntry>();
    ep->title = title;
    ep->source = url;
    ep->kind = "video";
    ep->id = m_next_id++;

    ep->mpv = mpv_create();
    if (!ep->mpv)
        return false;

    mpv_set_option_string(ep->mpv, "hwdec", "yes");
    mpv_set_option_string(ep->mpv, "vo", "libmpv");
    // yt-dlp integration — lets mpv stream YouTube, Vimeo, Twitch, etc.
    mpv_set_option_string(ep->mpv, "ytdl", "yes");
    mpv_set_option_string(ep->mpv, "ytdl-format",
                          "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best");
    // Network buffering
    mpv_set_option_string(ep->mpv, "cache", "yes");
    mpv_set_option_string(ep->mpv, "demuxer-max-bytes", "150MiB");
    mpv_set_option_string(ep->mpv, "demuxer-max-back-bytes", "50MiB");
    mpv_set_option_string(ep->mpv, "demuxer-readahead-secs", "30");
    mpv_set_option_string(ep->mpv, "stream-buffer-size", "4MiB");

    if (mpv_initialize(ep->mpv) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&ep->render_ctx, ep->mpv, params) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    VideoEntry *raw = ep.get();
    mpv_render_context_set_update_callback(
        ep->render_ctx,
        [](void *ctx) {
            static_cast<VideoEntry *>(ctx)->frame_dirty.store(
                true, std::memory_order_release);
        },
        raw);

    const char *cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command_async(ep->mpv, 0, cmd);

    ep->seek_preview.setup(m_vk, &m_seek_uploader, url);

    m_entries.push_back(std::move(ep));
    return true;
}

// ============================================================================
// GPU resource management
// ============================================================================

bool VideoPlayer::create_gpu_resources(VideoEntry &e) {
    std::println("[VideoPlayer] create_gpu_resources id={} {}x{}", e.id, e.video_w, e.video_h);
    const int w = e.video_w;
    const int h = e.video_h;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

    // -- VkImage (device-local, OPTIMAL tiling) ------------------------------
    {
        VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_vk->device, &info, m_vk->allocator, &e.image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_vk->device, e.image, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(m_vk->physical_device,
                                                 req.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &e.image_memory) != VK_SUCCESS)
            return false;

        vkBindImageMemory(m_vk->device, e.image, e.image_memory, 0);
    }

    // -- VkImageView ---------------------------------------------------------
    {
        VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = e.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_vk->device, &info, m_vk->allocator, &e.image_view);
    }

    // -- VkSampler -----------------------------------------------------------
    {
        VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        vkCreateSampler(m_vk->device, &info, m_vk->allocator, &e.sampler);
    }

    // -- ImGui descriptor (registers with ImGui_ImplVulkan) ------------------
    e.descriptor_set = ImGui_ImplVulkan_AddTexture(
        e.sampler, e.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // -- Staging buffer (HOST_VISIBLE, persistently mapped) ------------------
    {
        VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_vk->device, &info, m_vk->allocator, &e.staging_buf);

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_vk->device, e.staging_buf, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(
            m_vk->physical_device, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &e.staging_mem);
        vkBindBufferMemory(m_vk->device, e.staging_buf, e.staging_mem, 0);
        vkMapMemory(m_vk->device, e.staging_mem, 0, bytes, 0, &e.staging_mapped);
    }

    // -- Dedicated command pool (resettable) ---------------------------------
    {
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = m_vk->queue_family;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_vk->device, &info, m_vk->allocator, &e.cmd_pool);

        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = e.cmd_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_vk->device, &alloc, &e.cmd_buf);
    }

    // -- Reusable fence for upload synchronisation ---------------------------
    {
        VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(m_vk->device, &info, m_vk->allocator, &e.upload_fence);
    }

    // -- CPU pixel buffer ----------------------------------------------------
    e.pixel_buf.resize(static_cast<size_t>(w) * h * 4);

    // -- Transition image to SHADER_READ_ONLY so it can be sampled immediately
    {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(e.cmd_buf, &begin);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = e.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(e.cmd_buf,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(e.cmd_buf);

        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &e.cmd_buf;
        m_vk->queue_submit(1, &submit, e.upload_fence);
        vkWaitForFences(m_vk->device, 1, &e.upload_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_vk->device, 1, &e.upload_fence);
        vkResetCommandBuffer(e.cmd_buf, 0);
    }

    return true;
}

void VideoPlayer::destroy_gpu_resources(VideoEntry &e) {
    std::println("[VideoPlayer] destroy_gpu_resources id={}", e.id);
    if (e.descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(e.descriptor_set);
        e.descriptor_set = VK_NULL_HANDLE;
    }
    if (e.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vk->device, e.sampler, m_vk->allocator);
        e.sampler = VK_NULL_HANDLE;
    }
    if (e.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vk->device, e.image_view, m_vk->allocator);
        e.image_view = VK_NULL_HANDLE;
    }
    if (e.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_vk->device, e.image, m_vk->allocator);
        e.image = VK_NULL_HANDLE;
    }
    if (e.image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, e.image_memory, m_vk->allocator);
        e.image_memory = VK_NULL_HANDLE;
    }
    if (e.upload_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_vk->device, e.upload_fence, m_vk->allocator);
        e.upload_fence = VK_NULL_HANDLE;
    }
    // Destroying the pool also frees the command buffer
    if (e.cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_vk->device, e.cmd_pool, m_vk->allocator);
        e.cmd_pool = VK_NULL_HANDLE;
        e.cmd_buf = VK_NULL_HANDLE;
    }
    if (e.staging_buf != VK_NULL_HANDLE) {
        if (e.staging_mapped) {
            vkUnmapMemory(m_vk->device, e.staging_mem);
            e.staging_mapped = nullptr;
        }
        vkDestroyBuffer(m_vk->device, e.staging_buf, m_vk->allocator);
        e.staging_buf = VK_NULL_HANDLE;
    }
    if (e.staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, e.staging_mem, m_vk->allocator);
        e.staging_mem = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Hover thumbnail / seek thumbnail — delegated to child classes
// ============================================================================

VkDescriptorSet VideoPlayer::hover_thumbnail(const std::string &source) {
    static std::string s_last_hover_source;
    if (s_last_hover_source != source) {
        std::println("[VideoPlayer] hover_thumbnail: {}", source);
        s_last_hover_source = source;
    }
    return m_hover.thumbnail(source);
}

bool VideoPlayer::save_hover_frame(const std::filesystem::path &path) {
    std::println("[VideoPlayer] save_hover_frame: {}", path.string());
    return m_hover.save_frame(path);
}

VkDescriptorSet VideoPlayer::get_open_thumbnail(const std::string &source) const {
    static std::string s_last_hit_source;
    for (const auto &ep : m_entries) {
        if (ep->source == source && ep->descriptor_set != VK_NULL_HANDLE) {
            if (s_last_hit_source != source) {
                std::println("[VideoPlayer] get_open_thumbnail: HIT {}", source);
                s_last_hit_source = source;
            }
            return ep->descriptor_set;
        }
    }
    return VK_NULL_HANDLE;
}

// ============================================================================
// Per-frame upload
// ============================================================================

void VideoPlayer::upload_frame(VideoEntry &e) {
    if (!e.staging_mapped || e.video_w <= 0 || e.video_h <= 0)
        return;

    const int w = e.video_w;
    const int h = e.video_h;
    size_t stride = static_cast<size_t>(w) * 4;

    // Ask libmpv to render the current video frame into the CPU buffer
    int size_arr[2] = {w, h};
    const char *fmt_rgba = "rgba";
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size_arr},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(fmt_rgba)},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, e.pixel_buf.data()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_render(e.render_ctx, render_params) < 0)
        return;

    // Copy CPU buffer into the persistently-mapped staging buffer
    std::memcpy(e.staging_mapped, e.pixel_buf.data(), e.pixel_buf.size());

    // Record a one-time command buffer: barrier + copy + barrier
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(e.cmd_buf, &begin);

    // SHADER_READ → TRANSFER_DST
    VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.image = e.image;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b1.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(e.cmd_buf,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
    vkCmdCopyBufferToImage(e.cmd_buf, e.staging_buf, e.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ
    VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.image = e.image;
    b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(e.cmd_buf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(e.cmd_buf);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &e.cmd_buf;
    m_vk->queue_submit(1, &submit, e.upload_fence);
    vkWaitForFences(m_vk->device, 1, &e.upload_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(m_vk->device, 1, &e.upload_fence);
    vkResetCommandBuffer(e.cmd_buf, 0);
}

// ============================================================================
// Event polling
// ============================================================================

void VideoPlayer::poll_events(VideoEntry &e) {
    while (true) {
        mpv_event *ev = mpv_wait_event(e.mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;

        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            std::println("[VideoPlayer] event VIDEO_RECONFIG id={}", e.id);
            // Read the actual decoded video dimensions
            int64_t nw = 0, nh = 0;
            mpv_get_property(e.mpv, "dwidth", MPV_FORMAT_INT64, &nw);
            mpv_get_property(e.mpv, "dheight", MPV_FORMAT_INT64, &nh);

            if (nw > 0 && nh > 0 &&
                (static_cast<int>(nw) != e.video_w ||
                 static_cast<int>(nh) != e.video_h)) {
                vkDeviceWaitIdle(m_vk->device);
                destroy_gpu_resources(e);
                e.video_w = static_cast<int>(nw);
                e.video_h = static_cast<int>(nh);
                create_gpu_resources(e);
                e.frame_dirty.store(true, std::memory_order_release);
                std::println("[VideoPlayer] reconfigured id={} {}x{}", e.id, e.video_w, e.video_h);
            }
        }
    }
}

// ============================================================================
// update_frames — call once per frame
// ============================================================================

void VideoPlayer::update_frames() {
    if (!m_vk)
        return;

    for (auto &ep : m_entries) {
        VideoEntry &e = *ep;
        if (!e.mpv)
            continue;

        poll_events(e);

        // If the render-update callback fired, render and upload a new frame
        if (e.frame_dirty.exchange(false, std::memory_order_acq_rel) &&
            e.descriptor_set != VK_NULL_HANDLE) {
            upload_frame(e);
        }

        // Upload preview thumbnail if the jthread has a new frame ready
        e.seek_preview.update();
    }
}

// ============================================================================
// draw — called each frame inside an ImGui frame
// ============================================================================

void VideoPlayer::set_player_menu_callbacks(
    std::function<void()> on_open_image,
    std::function<void()> on_open_online,
    std::function<void(const std::string &, const std::string &)> on_open_recent,
    std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> history,
    HistoryPreview *preview)
{
    m_on_open_image    = std::move(on_open_image);
    m_on_open_online   = std::move(on_open_online);
    m_on_open_recent   = std::move(on_open_recent);
    m_history_provider = std::move(history);
    m_history_preview  = preview;
}

void VideoPlayer::set_context_menu(
    VideoContextMenu *ctx,
    std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> lookup,
    std::function<void(const std::string &)> on_erase)
{
    m_ctx_menu     = ctx;
    m_ctx_lookup   = std::move(lookup);
    m_ctx_on_erase = std::move(on_erase);
}

void VideoPlayer::draw() {
    if (!m_vk)
        return;

    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (!m_entries[i]->open)
            continue;
        draw_window(*m_entries[i], i);
    }

    // Evict closed entries (destroy GPU + mpv resources)
    bool any_closed = std::any_of(m_entries.begin(), m_entries.end(),
                                  [](const std::unique_ptr<VideoEntry> &ep) {
                                      return !ep->open;
                                  });
    if (any_closed)
        vkDeviceWaitIdle(m_vk->device);

    std::erase_if(m_entries, [this](const std::unique_ptr<VideoEntry> &ep) {
        if (ep->open)
            return false;
        // Stop threads before freeing resources they use
        ep->seek_preview.stop_thread();
        if (ep->render_ctx) {
            mpv_render_context_free(ep->render_ctx);
            ep->render_ctx = nullptr;
        }
        destroy_gpu_resources(*ep);
        if (ep->mpv) {
            mpv_terminate_destroy(ep->mpv);
            ep->mpv = nullptr;
        }
        ep->seek_preview.shutdown();
        return true;
    });
}

void VideoPlayer::draw_window(VideoEntry &e, int idx) {
    const std::string win_id = e.title + "###video_" + std::to_string(e.id);

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (e.fullscreen) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
        flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    } else {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (m_on_open_image || m_on_open_online || m_on_open_recent)
            flags |= ImGuiWindowFlags_MenuBar;
    }

    if (!ImGui::Begin(win_id.c_str(), &e.open, flags)) {
        ImGui::End();
        return;
    }

    // ---- Context menu (right-click anywhere in the window) -----------------
    if (m_ctx_menu) {
        // Build a minimal history entry; use cached history entry when available.
        WindowStateToml::ImageHistoryEntry hist_entry;
        if (m_ctx_lookup) {
            if (const auto *found = m_ctx_lookup(e.source))
                hist_entry = *found;
        }
        if (hist_entry.source.empty()) {
            hist_entry.source = e.source;
            hist_entry.kind   = e.kind;
        }

        const std::string popup_id = "##vctx_" + std::to_string(e.id);
        if (ImGui::BeginPopupContextWindow(popup_id.c_str())) {
            // --- History / save items (Remove from History, Save Video As…) ---
            const auto ctx_result = m_ctx_menu->draw_menu_items(hist_entry);
            if (ctx_result.erase && m_ctx_on_erase)
                m_ctx_on_erase(ctx_result.erase_source);

            // --- Open shortcuts -------------------------------------------
            if (m_on_open_image || m_on_open_online || m_on_open_recent) {
                ImGui::Separator();
                if (m_on_open_image && ImGui::MenuItem("Open Image...", "Ctrl+O"))
                    m_on_open_image();
                if (m_on_open_online && ImGui::MenuItem("Open Online..."))
                    m_on_open_online();
                if (m_history_provider && m_on_open_recent) {
                    const auto &hist = m_history_provider();
                    if (!hist.empty() && ImGui::BeginMenu("Recent")) {
                        constexpr int k_max = 20;
                        int shown = 0;
                        for (const auto &h : hist) {
                            if (shown++ >= k_max)
                                break;
                            std::string label;
                            if (h.kind == "file") {
                                label = "[file]  ";
                                label += std::filesystem::path(h.source).filename().string();
                            } else {
                                label = "[url]   ";
                                label += h.source.size() > 60
                                             ? h.source.substr(0, 57) + "..."
                                             : h.source;
                            }
                            if (ImGui::MenuItem(label.c_str()))
                                m_on_open_recent(h.source, h.kind);
                            if (ImGui::IsItemHovered() && m_history_preview && m_ctx_lookup) {
                                if (auto *entry = m_ctx_lookup(h.source))
                                    m_history_preview->draw_for_hover(*entry);
                            }
                        }
                        ImGui::EndMenu();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    // ---- In-window File menu bar -------------------------------------------
    if (!e.fullscreen && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (m_on_open_image && ImGui::MenuItem("Open Image...", "Ctrl+O"))
                m_on_open_image();
            if (m_on_open_online && ImGui::MenuItem("Open Online..."))
                m_on_open_online();

            if (m_history_provider && m_on_open_recent) {
                const auto &hist = m_history_provider();
                if (!hist.empty() && ImGui::BeginMenu("Recent")) {
                    constexpr int k_max = 20;
                    int shown = 0;
                    for (const auto &h : hist) {
                        if (shown++ >= k_max)
                            break;
                        std::string label;
                        if (h.kind == "file") {
                            label = "[file]  ";
                            label += std::filesystem::path(h.source).filename().string();
                        } else {
                            label = "[url]   ";
                            label += h.source.size() > 60
                                         ? h.source.substr(0, 57) + "..."
                                         : h.source;
                        }
                        if (ImGui::MenuItem(label.c_str()))
                            m_on_open_recent(h.source, h.kind);
                        if (ImGui::IsItemHovered() && m_history_preview && m_ctx_lookup) {
                            if (auto *entry = m_ctx_lookup(h.source))
                                m_history_preview->draw_for_hover(*entry);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Escape exits fullscreen
    if (e.fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape))
        e.fullscreen = false;

    // ---- Video frame image -------------------------------------------------
    constexpr float k_controls_h = 72.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    if (e.descriptor_set != VK_NULL_HANDLE && e.video_w > 0 && e.video_h > 0) {
        const float vid_ar = static_cast<float>(e.video_w) / static_cast<float>(e.video_h);
        const float canvas_h = std::max(avail.y - k_controls_h, 1.0f);
        const float canvas_w = avail.x;

        float disp_w, disp_h;
        if (vid_ar > canvas_w / canvas_h) {
            disp_w = canvas_w;
            disp_h = canvas_w / vid_ar;
        } else {
            disp_h = canvas_h;
            disp_w = canvas_h * vid_ar;
        }
        disp_w = std::max(disp_w, 1.0f);
        disp_h = std::max(disp_h, 1.0f);

        // Centre image inside the canvas area
        const ImVec2 cursor_base = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursor_base.x + (canvas_w - disp_w) * 0.5f,
                                   cursor_base.y + (canvas_h - disp_h) * 0.5f));
        ImGui::Image(std::bit_cast<ImTextureID>(e.descriptor_set), ImVec2(disp_w, disp_h));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                e.fullscreen = !e.fullscreen;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                mpv_command_string(e.mpv, "cycle pause");
        }
    } else {
        ImGui::TextDisabled("Loading...");
    }

    // ---- Transport controls ------------------------------------------------
    ImGui::Separator();

    // Fetch current playback state (cheap mpv property reads)
    double time_pos = 0.0;
    double dur = 0.0;
    int paused = 0;
    int64_t volume = 100;
    mpv_get_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);
    mpv_get_property(e.mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    mpv_get_property(e.mpv, "pause", MPV_FORMAT_FLAG, &paused);
    mpv_get_property(e.mpv, "volume", MPV_FORMAT_INT64, &volume);

    auto fmt_time = [](double t) {
        const int total = static_cast<int>(t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
        return std::string(buf);
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

    // Prev / next file — navigate across open entries
    const bool has_prev = idx > 0;
    const bool has_next = idx < static_cast<int>(m_entries.size()) - 1;
    if (!has_prev)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton("|<")) {
        const std::string &src = m_entries[idx - 1]->source;
        const char *cmd[] = {"loadfile", src.c_str(), "replace", nullptr};
        mpv_command_async(e.mpv, 0, cmd);
        e.source = src;
        e.title = m_entries[idx - 1]->title;
        e.kind = m_entries[idx - 1]->kind;
    }
    if (!has_prev)
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton("<<"))
        mpv_command_string(e.mpv, "seek -10");

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(paused ? "|>" : "||"))
        mpv_command_string(e.mpv, "cycle pause");

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(">>"))
        mpv_command_string(e.mpv, "seek 10");

    ImGui::SameLine(0.0f, 4.0f);
    if (e.loop)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("⟳")) {
        e.loop = !e.loop;
        mpv_command_string(e.mpv, e.loop ? "set loop-file inf" : "set loop-file no");
    }
    if (e.loop)
        ImGui::PopStyleColor();
    
    ImGui::SameLine(0.0f, 4.0f);
    if (!has_next)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton(">|")) {
        const std::string &src = m_entries[idx + 1]->source;
        const char *cmd[] = {"loadfile", src.c_str(), "replace", nullptr};
        mpv_command_async(e.mpv, 0, cmd);
        e.source = src;
        e.title = m_entries[idx + 1]->title;
        e.kind = m_entries[idx + 1]->kind;
    }
    if (!has_next)
        ImGui::EndDisabled();

    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(fmt_time(time_pos).c_str());
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextUnformatted(fmt_time(dur).c_str());

    // Seek bar
    if (dur > 0.0) {
        float pos_f = static_cast<float>(time_pos / dur);
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::SliderFloat("##seek", &pos_f, 0.0f, 1.0f, "")) {
            double new_pos = pos_f * dur;
            mpv_set_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &new_pos);
        }

        // Seek-preview tooltip: show thumbnail at the hovered position
        if (ImGui::IsItemHovered() && e.seek_preview.descriptor_set() != VK_NULL_HANDLE) {
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const float frac = std::clamp(
                (ImGui::GetMousePos().x - item_min.x) / (item_max.x - item_min.x),
                0.0f, 1.0f);
            e.seek_preview.seek(static_cast<double>(frac) * dur);
            ImGui::BeginTooltip();
            ImGui::Image(std::bit_cast<ImTextureID>(e.seek_preview.descriptor_set()),
                         e.seek_preview.size());
            ImGui::Text("%s", fmt_time(static_cast<double>(frac) * dur).c_str());
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
    }

    // Volume slider
    int vol_i = static_cast<int>(volume);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##vol", &vol_i, 0, 150, "Vol %d%%")) {
        int64_t new_vol = vol_i;
        mpv_set_property(e.mpv, "volume", MPV_FORMAT_INT64, &new_vol);
    }

    ImGui::End();
}

// ============================================================================
// Query
// ============================================================================

bool VideoPlayer::has_open_windows() const {
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [](const std::unique_ptr<VideoEntry> &ep) { return ep->open; });
}
