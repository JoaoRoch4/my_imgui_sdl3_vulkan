#include "pch.hpp"

#include "thread_overwatch.hpp"
#include "video_seek_preview.hpp"
#include "vulkan_context.hpp"
#include "vulkan_upload_context.hpp"

#ifndef VIDEO_SEEK_DEBUG
#ifdef NDEBUG
#define VIDEO_SEEK_DEBUG 0
#else
#define VIDEO_SEEK_DEBUG 1
#endif
#endif

#if VIDEO_SEEK_DEBUG
#define _SeekDebug(fmt, ...)                                                   \
  std::println("[VideoSeekPreview] " fmt, ##__VA_ARGS__)
#else
#define _SeekDebug(fmt, ...) ((void)0)
#endif

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter,
                          VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);

  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    if ((filter & (1u << i)) &&
        ((mem.memoryTypes[i].propertyFlags & props) == props))
      return i;

  return 0xFFFFFFFFu;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

VideoSeekPreview::VideoSeekPreview() = default;

VideoSeekPreview::~VideoSeekPreview() { shutdown(); }

void VideoSeekPreview::setup(vulkan_context *vk, VulkanUploadContext *uploader,
                             const std::string &source) {
  prepare(vk, uploader, source);
  ensure_active();
}

void VideoSeekPreview::prepare(vulkan_context *vk, VulkanUploadContext *uploader,
                               const std::string &source) {
  _SeekDebug("prepare source={}", source);

  // If mpv is already running (source change), tear down the thread and mpv
  // but keep GPU resources — they are source-independent fixed-size textures.
  if (m_mpv) {
    stop_thread();
    if (m_render_ctx) {
      mpv_render_context_free(m_render_ctx);
      m_render_ctx = nullptr;
    }
    mpv_terminate_destroy(m_mpv);
    m_mpv = nullptr;
  }

  m_vk       = vk;
  m_uploader = uploader;
  m_source   = source;

  // Allocate GPU resources once (idempotent — skipped if already created).
  if (m_image == VK_NULL_HANDLE)
    create_gpu_resources(vk);
}

void VideoSeekPreview::ensure_active() {
  if (m_mpv)           return; // already running
  if (!m_vk)           return; // not prepared
  if (m_source.empty()) return;

  _SeekDebug("ensure_active source={}", m_source);
  init_mpv(m_source);
  if (!m_mpv || !m_render_ctx)
    return;
  start_thread();
}

void VideoSeekPreview::stop_thread(bool unregister_watch) {
  std::lock_guard<std::mutex> lock(m_lifecycle_mutex);

  const bool has_thread = m_thread.joinable();
  const bool has_watch =
      m_thread_watch_id.load(std::memory_order_acquire) != 0;

  // Already stopped and no watch to unregister.
  if (!has_thread && (!unregister_watch || !has_watch))
    return;

  _SeekDebug("stop_thread");

  if (unregister_watch) {
    const uint64_t watch_id =
        m_thread_watch_id.exchange(0, std::memory_order_acq_rel);
    ThreadOverwatch::instance().unwatch(watch_id);
  }

  if (has_thread)
    m_thread = std::jthread{};
}

void VideoSeekPreview::shutdown() {
  const bool is_inactive = !m_thread.joinable() &&
                           m_thread_watch_id.load(std::memory_order_acquire) == 0 &&
                           m_mpv == nullptr && m_render_ctx == nullptr &&
                           m_image == VK_NULL_HANDLE &&
                           m_image_memory == VK_NULL_HANDLE &&
                           m_image_view == VK_NULL_HANDLE &&
                           m_sampler == VK_NULL_HANDLE &&
                           m_descriptor_set == VK_NULL_HANDLE &&
                           m_vk == nullptr && m_uploader == nullptr;
  if (is_inactive)
    return;

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
  m_source.clear();
}

// ============================================================================
// MPV
// ============================================================================

void VideoSeekPreview::init_mpv(const std::string &source) {
  _SeekDebug("init_mpv source={}", source);
  m_mpv = mpv_create();
  if (!m_mpv)
    return;

  mpv_set_option_string(m_mpv, "vo", "libmpv");
  mpv_set_option_string(m_mpv, "mute", "no");
  mpv_set_option_string(m_mpv, "pause", "yes");
  mpv_set_option_string(m_mpv, "hr-seek", "yes");
  mpv_set_option_string(m_mpv, "hwdec", "nvdec");
  mpv_set_option_string(m_mpv, "hwdec-codecs", "h264,hevc,av1,vp9,mpeg4,vc1");
    // yt-dlp integration — lets mpv stream YouTube, Vimeo, Twitch, etc.
   
  if (mpv_initialize(m_mpv) < 0) {
    mpv_terminate_destroy(m_mpv);
    m_mpv = nullptr;
    return;
  }

  std::any api_type_value{const_cast<char *>(MPV_RENDER_API_TYPE_SW)};
  auto *api_type_payload =
      static_cast<void *>(std::any_cast<char *>(api_type_value));

  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, api_type_payload},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };

  if (mpv_render_context_create(&m_render_ctx, m_mpv, params) < 0) {
    mpv_terminate_destroy(m_mpv);
    m_mpv = nullptr;
    m_render_ctx = nullptr;
    return;
  }

  mpv_render_context_set_update_callback(
      m_render_ctx,
      [](void *ctx) {
        auto *self = static_cast<VideoSeekPreview *>(ctx);
        self->m_frame_dirty.store(true, std::memory_order_release);
      },
      this);

  const char *cmd[] = {"loadfile", source.c_str(), nullptr};
  mpv_command_async(m_mpv, 0, cmd);
}

// ============================================================================
// GPU
// ============================================================================

bool VideoSeekPreview::create_gpu_resources(vulkan_context *vk) {
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
  alloc.memoryTypeIndex =
      find_memory_type(vk->physical_device, req.memoryTypeBits,
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
      m_sampler, m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // Transition once so first sampling is valid before any upload happens.
  {
    VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = vk->queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &pool_info, nullptr, &pool);

    VkCommandBufferAllocateInfo cmd_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cmd_info, &cmd);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = m_image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fence_info, nullptr, &fence);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk->queue_submit(1, &submit, fence);

    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, nullptr);
  }

  // CPU buffer
  m_buf.resize(m_w * m_h * 4);

  return true;
}

void VideoSeekPreview::destroy_gpu_resources(vulkan_context *vk) {
  _SeekDebug("destroy_gpu_resources");
  if (!vk)
    return;

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

void VideoSeekPreview::start_thread() {
  std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
  _SeekDebug("start_thread");

  if (!m_mpv || !m_render_ctx)
    return;

  if (m_thread.joinable())
    return;

  if (m_thread_watch_id.load(std::memory_order_acquire) == 0) {
    const auto watch_id = ThreadOverwatch::instance().watch(
        "VideoSeekPreview::thread", std::chrono::milliseconds(5000),
        [this]() { stop_thread(false); },
        [this]() {
          if (m_vk != nullptr && m_uploader != nullptr && m_mpv != nullptr &&
              m_render_ctx != nullptr)
            start_thread();
        });
    m_thread_watch_id.store(watch_id, std::memory_order_release);
  }

  m_thread = std::jthread([this](const std::stop_token& stoken) {
    while (!stoken.stop_requested()) {
      const uint64_t watch_id =
          m_thread_watch_id.load(std::memory_order_acquire);
      ThreadOverwatch::instance().heartbeat(watch_id);

      double req = m_seek_req.exchange(-1.0);

      if (req >= 0.0 && m_mpv && m_render_ctx) {

        char t[64];
        snprintf(t, sizeof(t), "%.4f", req);

        const char *cmd[] = {"seek", t, "absolute+exact", nullptr};
        mpv_command_async(m_mpv, 0, cmd);

        m_frame_dirty = false;

        for (int i = 0; i < 50; ++i) {
          mpv_wait_event(m_mpv, 0.01);
          if (m_frame_dirty)
            break;
        }

        if (m_frame_dirty) {

          int size[2] = {m_w, m_h};
          size_t stride = m_w * 4;
          std::any sw_format_value{const_cast<char *>("rgba")};
          void *sw_format_payload =
              static_cast<void *>(std::any_cast<char *>(sw_format_value));

          std::lock_guard lock(m_buf_mutex);

          mpv_render_param params[] = {
              {MPV_RENDER_PARAM_SW_SIZE, size},
              {MPV_RENDER_PARAM_SW_FORMAT, sw_format_payload},
              {MPV_RENDER_PARAM_SW_STRIDE, &stride},
              {MPV_RENDER_PARAM_SW_POINTER, m_buf.data()},
              {MPV_RENDER_PARAM_INVALID, nullptr},
          };

          if (mpv_render_context_render(m_render_ctx, params) >= 0)
            m_buf_ready = true;
        }
      }

      mpv_wait_event(m_mpv, 0.01);
      ThreadOverwatch::instance().heartbeat(watch_id);
    }
  });

  ThreadOverwatch::instance().heartbeat(
      m_thread_watch_id.load(std::memory_order_acquire));
}

// ============================================================================
// UPDATE (IMPORTANT PART)
// ============================================================================

void VideoSeekPreview::update() {
  if (!m_buf_ready.exchange(false))
    return;

  if (!m_uploader)
    return;

  std::lock_guard lock(m_buf_mutex);
  m_uploader->upload_to_image(m_buf.data(), m_image, (uint32_t)m_w,
                              (uint32_t)m_h);
}

// ============================================================================

void VideoSeekPreview::seek(double t) { m_seek_req.store(t); }