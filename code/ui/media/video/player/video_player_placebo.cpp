#include "pch.hpp" // NOLINT

#include "video_player_placebo.hpp"

#include "core/log/debug_log.hpp"
#include "video_player.hpp"
#include "vulkan_context.hpp"

namespace {
// ---------------------------------------------------------------------------
// GL extension function table – loaded once via eglGetProcAddress
// ---------------------------------------------------------------------------
struct GlExtFuncs {
  PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
  PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
  PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFdEXT = nullptr;
  PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT = nullptr;
  PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT = nullptr;
  PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT = nullptr;
  PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFdEXT = nullptr;
  PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT = nullptr;
  PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT = nullptr;
  // FBO functions (GL 3.0 – load via procaddr to be safe with EGL)
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
  PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
  bool loaded = false;

  bool load() {
    if (loaded)
      return true;
#define GLOAD(T, name)                                                         \
  name = std::bit_cast<T>(eglGetProcAddress(#name));                        \
  if (!(name)) {                                                                 \
    APP_DEBUG_LOG("[GlExtFuncs] failed to load: " #name);                      \
    return false;                                                              \
  }
    GLOAD(PFNGLCREATEMEMORYOBJECTSEXTPROC, CreateMemoryObjectsEXT)
    GLOAD(PFNGLDELETEMEMORYOBJECTSEXTPROC, DeleteMemoryObjectsEXT)
    GLOAD(PFNGLIMPORTMEMORYFDEXTPROC, ImportMemoryFdEXT)
    GLOAD(PFNGLTEXSTORAGEMEM2DEXTPROC, TexStorageMem2DEXT)
    GLOAD(PFNGLGENSEMAPHORESEXTPROC, GenSemaphoresEXT)
    GLOAD(PFNGLDELETESEMAPHORESEXTPROC, DeleteSemaphoresEXT)
    GLOAD(PFNGLIMPORTSEMAPHOREFDEXTPROC, ImportSemaphoreFdEXT)
    GLOAD(PFNGLSIGNALSEMAPHOREEXTPROC, SignalSemaphoreEXT)
    GLOAD(PFNGLWAITSEMAPHOREEXTPROC, WaitSemaphoreEXT)
    GLOAD(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)
    GLOAD(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)
    GLOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)
    GLOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)
    GLOAD(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)
#undef GLOAD
    loaded = true;
    APP_DEBUG_LOG("[GlExtFuncs] all GL interop extensions loaded");
    return true;
  }
};

// Vulkan extension function pointers (loaded via vkGetDeviceProcAddr)
PFN_vkGetMemoryFdKHR s_vkGetMemoryFdKHR = nullptr;
PFN_vkGetSemaphoreFdKHR s_vkGetSemaphoreFdKHR = nullptr;
GlExtFuncs s_gl;

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter,
                          VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((filter & (1u << i)) &&
        ((mem.memoryTypes[i].propertyFlags & props) == props))
      return i;
  }
  return 0xFFFFFFFFu;
}

bool is_valid_config(const VideoPlayerPlacebo::Config &config) {
  const auto rot = pl_rotation_normalize(PL_ROTATION_0);
  (void)rot;

  // This scaffold currently supports all combinations. Keep validation in one
  // place so future backend constraints are easy to enforce.
  (void)config;
  return true;
}

const std::unordered_set<std::string> k_video_exts = {
    ".mp4",  ".mkv",  ".avi",  ".mov", ".webm", ".flv", ".wmv", ".m4v",
    ".ts",   ".m2ts", ".mpeg", ".mpg", ".ogv",  ".3gp", ".rm",  ".rmvb",
    ".divx", ".xvid", ".gif",  ".mp3", ".flac", ".ogg", ".wav", ".aac",
    ".opus", ".m4a",  ".wma",  ".ac3", ".dts",  ".tta", ".wv",
};
} // namespace

VideoPlayerPlacebo::VideoPlayerPlacebo()
    : m_hover_player(std::make_unique<VideoPlayer>()) {}

VideoPlayerPlacebo::~VideoPlayerPlacebo() { shutdown(); }

void VideoPlayerPlacebo::bind_context(vulkan_context *vk) {
  m_vk = vk;
  m_main_thread_id = std::this_thread::get_id();
  if (m_hover_player)
    m_hover_player->bind_context(vk);
}

void VideoPlayerPlacebo::setup(vulkan_context *vk) {
  setup(vk, kDefaultConfig);
}

void VideoPlayerPlacebo::setup(vulkan_context *vk, Config config) {
  if (!vk)
    return;
  if (!is_valid_config(config))
    return;

  if (m_initialized && m_vk == vk && m_config == config)
    return;

  if (m_initialized)
    shutdown();

  m_vk = vk;
  m_config = config;
  m_main_thread_id = std::this_thread::get_id();
  m_initialized = true;

  if (!init_placebo_gpu())
    APP_DEBUG_LOG("[VideoPlayerPlacebo] init_placebo_gpu failed – falling back "
                  "to SW path");

  if (m_hover_player) {
    m_hover_player->bind_context(m_vk);
    m_hover_player->setup(m_vk);
    m_hover_player->set_all_hwdec(m_config.enable_hwdec);
  }
  if (m_placeholder_descriptor_set == VK_NULL_HANDLE)
    create_placeholder_texture();
}

void VideoPlayerPlacebo::reconfigure(Config config) {
  if (!is_valid_config(config))
    return;

  if (m_config == config)
    return;

  m_config = config;

  if (!m_initialized)
    return;

  if (m_hover_player)
    m_hover_player->set_all_hwdec(m_config.enable_hwdec);

  // Placeholder path for future runtime backend reconfiguration.
  // For now, keep the class initialized and only update the cached config.
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path) {
  return add_from_path(path, {}, {}, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title) {
  (void)title;
  return add_from_path(path, title, {}, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title,
                                       const std::string &logical_source) {
  return add_from_path(path, title, logical_source, m_config.enable_hwdec, 0,
                       {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title,
                                       const std::string &logical_source,
                                       bool hwdec_enabled,
                                       int resume_position_seconds,
                                       const std::string &initial_osd_message) {
  if (!m_initialized && m_vk)
    setup(m_vk, m_config);
  if (!m_initialized)
    return false;

  // If the libplacebo GPU pipeline is ready, use the PlaceboEntry path.
  if (m_pl_renderer) {
    for (const auto &e : m_entries) {
      if (e->source == path.string() || e->playback_source == path.string())
        return true; // already open
    }
    auto entry = std::make_unique<PlaceboEntry>();
    entry->id = m_next_id++;
    entry->source = path.string();
    entry->title = title.empty() ? path.filename().string() : title;
    entry->playback_source =
        logical_source.empty() ? path.string() : logical_source;
    entry->hwdec_enabled = hwdec_enabled;
    entry->resume_position_seconds = resume_position_seconds;
    if (!initial_osd_message.empty())
      entry->osd.show(initial_osd_message);
    if (!entry_create_mpv(*entry, path.string(), hwdec_enabled)) {
      APP_DEBUG_LOG("[VideoPlayerPlacebo] entry_create_mpv failed for {}",
                    path.string());
      return false;
    }
    m_entries.push_back(std::move(entry));
    return true;
  }

  // Fallback: SW path via hover player
  if (!m_hover_player)
    return false;

  return m_hover_player->add_from_path(path, title, logical_source,
                                       hwdec_enabled, resume_position_seconds,
                                       initial_osd_message);
}

bool VideoPlayerPlacebo::add_from_url(const std::string &url,
                                      const std::string &title) {
  return add_from_url(url, title, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_url(const std::string &url,
                                      const std::string &title,
                                      bool hwdec_enabled,
                                      int resume_position_seconds,
                                      const std::string &initial_osd_message) {
  if (!m_initialized && m_vk)
    setup(m_vk, m_config);
  if (!m_initialized)
    return false;

  reconfigure(
      Config{.enable_hwdec = hwdec_enabled, .prefer_nvdec = hwdec_enabled});
  if (!m_hover_player)
    return false;

  return m_hover_player->add_from_url(
      url, title, hwdec_enabled, resume_position_seconds, initial_osd_message);
}

void VideoPlayerPlacebo::update_frames() {
  assert((m_main_thread_id == std::thread::id{} ||
          std::this_thread::get_id() == m_main_thread_id) &&
         "VideoPlayerPlacebo::update_frames() must be called from the main "
         "thread");

  if (m_hover_player)
    m_hover_player->update_frames();

  // PlaceboEntry GPU render loop
  for (auto &e : m_entries) {
    entry_poll_events(*e);
    if (e->frame_dirty.load(std::memory_order_acquire) && !e->load_failed) {
      e->frame_dirty.store(false, std::memory_order_release);
      entry_render_frame(*e);
    }
  }
  // Remove closed entries
  std::erase_if(m_entries, [](const auto &ep) { return !ep->open; });
}

bool VideoPlayerPlacebo::create_placeholder_texture() {
  if (!m_vk || !m_initialized)
    return false;
  if (m_placeholder_descriptor_set != VK_NULL_HANDLE)
    return true;

  const int w = m_placeholder_width;
  const int h = m_placeholder_height;
  const VkDeviceSize bytes =
      static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;

  std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                          static_cast<size_t>(x)) *
                         4u;
      const bool checker = (((x / 16) + (y / 16)) & 1) != 0;
      const uint8_t r = static_cast<uint8_t>(checker ? 64 : 28);
      const uint8_t g = static_cast<uint8_t>(checker ? 86 : 36);
      const uint8_t b = static_cast<uint8_t>(checker ? 118 : 48);
      pixels[idx + 0] = r;
      pixels[idx + 1] = g;
      pixels[idx + 2] = b;
      pixels[idx + 3] = 255;
    }
  }

  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(m_vk->device, &image_info, m_vk->allocator,
                    &m_placeholder_image) != VK_SUCCESS)
    return false;

  VkMemoryRequirements img_req{};
  vkGetImageMemoryRequirements(m_vk->device, m_placeholder_image, &img_req);

  VkMemoryAllocateInfo image_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  image_alloc.allocationSize = img_req.size;
  image_alloc.memoryTypeIndex =
      find_memory_type(m_vk->physical_device, img_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (image_alloc.memoryTypeIndex == 0xFFFFFFFFu)
    return false;
  if (vkAllocateMemory(m_vk->device, &image_alloc, m_vk->allocator,
                       &m_placeholder_image_memory) != VK_SUCCESS)
    return false;
  vkBindImageMemory(m_vk->device, m_placeholder_image,
                    m_placeholder_image_memory, 0);

  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = m_placeholder_image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(m_vk->device, &view_info, m_vk->allocator,
                        &m_placeholder_image_view) != VK_SUCCESS)
    return false;

  VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 1.0f;
  if (vkCreateSampler(m_vk->device, &sampler_info, m_vk->allocator,
                      &m_placeholder_sampler) != VK_SUCCESS)
    return false;

  m_placeholder_descriptor_set = ImGui_ImplVulkan_AddTexture(
      m_placeholder_image_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  VkBuffer staging_buf = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;

  VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(m_vk->device, &buffer_info, m_vk->allocator,
                     &staging_buf) != VK_SUCCESS)
    return false;

  VkMemoryRequirements buf_req{};
  vkGetBufferMemoryRequirements(m_vk->device, staging_buf, &buf_req);

  VkMemoryAllocateInfo buf_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  buf_alloc.allocationSize = buf_req.size;
  buf_alloc.memoryTypeIndex =
      find_memory_type(m_vk->physical_device, buf_req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (buf_alloc.memoryTypeIndex == 0xFFFFFFFFu)
    return false;
  if (vkAllocateMemory(m_vk->device, &buf_alloc, m_vk->allocator,
                       &staging_mem) != VK_SUCCESS)
    return false;
  vkBindBufferMemory(m_vk->device, staging_buf, staging_mem, 0);

  void *mapped = nullptr;
  vkMapMemory(m_vk->device, staging_mem, 0, bytes, 0, &mapped);
  std::memcpy(mapped, pixels.data(), static_cast<size_t>(bytes));
  vkUnmapMemory(m_vk->device, staging_mem);

  const int frame_idx = static_cast<int>(m_vk->main_window_data.FrameIndex);
  VkCommandPool cmd_pool = m_vk->main_window_data.Frames[frame_idx].CommandPool;

  VkCommandBufferAllocateInfo cmd_alloc = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = cmd_pool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(m_vk->device, &cmd_alloc, &cmd);

  VkCommandBufferBeginInfo begin = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b1.srcAccessMask = 0;
  b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b1.image = m_placeholder_image;
  b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b1);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
  vkCmdCopyBufferToImage(cmd, staging_buf, m_placeholder_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b2.image = m_placeholder_image;
  b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b2);

  vkEndCommandBuffer(cmd);

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(m_vk->device, &fence_info, m_vk->allocator, &fence);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  m_vk->queue_submit(1, &submit, fence);
  vkWaitForFences(m_vk->device, 1, &fence, VK_TRUE, UINT64_MAX);

  vkDestroyFence(m_vk->device, fence, m_vk->allocator);
  vkFreeCommandBuffers(m_vk->device, cmd_pool, 1, &cmd);
  vkDestroyBuffer(m_vk->device, staging_buf, m_vk->allocator);
  vkFreeMemory(m_vk->device, staging_mem, m_vk->allocator);

  return true;
}

void VideoPlayerPlacebo::destroy_placeholder_texture() {
  if (!m_vk)
    return;

  if (m_placeholder_descriptor_set != VK_NULL_HANDLE) {
    ImGui_ImplVulkan_RemoveTexture(m_placeholder_descriptor_set);
    m_placeholder_descriptor_set = VK_NULL_HANDLE;
  }
  if (m_placeholder_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(m_vk->device, m_placeholder_sampler, m_vk->allocator);
    m_placeholder_sampler = VK_NULL_HANDLE;
  }
  if (m_placeholder_image_view != VK_NULL_HANDLE) {
    vkDestroyImageView(m_vk->device, m_placeholder_image_view, m_vk->allocator);
    m_placeholder_image_view = VK_NULL_HANDLE;
  }
  if (m_placeholder_image != VK_NULL_HANDLE) {
    vkDestroyImage(m_vk->device, m_placeholder_image, m_vk->allocator);
    m_placeholder_image = VK_NULL_HANDLE;
  }
  if (m_placeholder_image_memory != VK_NULL_HANDLE) {
    vkFreeMemory(m_vk->device, m_placeholder_image_memory, m_vk->allocator);
    m_placeholder_image_memory = VK_NULL_HANDLE;
  }
}

void VideoPlayerPlacebo::draw() {
  if (!m_initialized)
    return;

  if (m_hover_player && m_hover_player->has_open_windows())
    m_hover_player->draw();

  if (m_placeholder_descriptor_set == VK_NULL_HANDLE)
    create_placeholder_texture();

  for (size_t i = 0; i < m_entries.size(); ++i) {
    PlaceboEntry &e = *m_entries[i];

    bool open = true;
    const std::string win_title =
        e.title.empty() ? (std::string("Video###vpp_") + std::to_string(e.id))
                        : (e.title + "###vpp_" + std::to_string(e.id));

    if (!ImGui::Begin(win_title.c_str(), &open)) {
      ImGui::End();
      if (!open)
        e.open = false;
      continue;
    }

    // Compute display size maintaining aspect ratio
    const float avail_w = std::max(ImGui::GetContentRegionAvail().x, 120.0f);
    float aspect = 16.0f / 9.0f;
    if (e.shared_w > 0 && e.shared_h > 0)
      aspect = static_cast<float>(e.shared_w) / static_cast<float>(e.shared_h);
    const float disp_w = std::min(avail_w, 1920.0f);
    const float disp_h = disp_w / aspect;
    const ImVec2 disp_size(disp_w, disp_h);

    // Use entry descriptor if available, otherwise placeholder
    VkDescriptorSet tex = e.descriptor_set != VK_NULL_HANDLE
                              ? e.descriptor_set
                              : m_placeholder_descriptor_set;

    if (tex != VK_NULL_HANDLE)
      ImGui::Image(std::bit_cast<ImTextureID>(tex), disp_size);
    else
      ImGui::Dummy(disp_size);

    if (e.load_failed)
      ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Load failed");

    ImGui::End();

    if (!open)
      e.open = false;
  }
}

void VideoPlayerPlacebo::notify_download_complete(
    const std::string &url, const std::filesystem::path &cached_path) {
  if (m_hover_player)
    m_hover_player->notify_download_complete(url, cached_path);

  const std::string cached = cached_path.string();
  for (auto &e : m_entries) {
    if (e->source == url) {
      e->source = cached;
      break;
    }
  }
}

void VideoPlayerPlacebo::replace_source_with_saved_file(
    const std::string &source, const std::filesystem::path &saved_path) {
  if (m_hover_player)
    m_hover_player->replace_source_with_saved_file(source, saved_path);

  const std::string saved = saved_path.string();
  for (auto &e : m_entries) {
    if (e->source == source) {
      e->source = saved;
      break;
    }
  }
}

bool VideoPlayerPlacebo::close_window(const std::string &source) {
  if (m_hover_player && m_hover_player->close_window(source))
    return true;

  const auto before = m_entries.size();
  std::erase_if(m_entries, [&](const auto &e) { return e->source == source; });
  return m_entries.size() != before;
}

void VideoPlayerPlacebo::close_all_windows() {
  if (m_hover_player)
    m_hover_player->close_all_windows();
  m_entries.clear();
}

bool VideoPlayerPlacebo::has_open_windows() const {
  if (m_hover_player && m_hover_player->has_open_windows())
    return true;
  return !m_entries.empty();
}

std::vector<std::string> VideoPlayerPlacebo::open_sources() const {
  if (m_hover_player && m_hover_player->has_open_windows())
    return m_hover_player->open_sources();
  std::vector<std::string> srcs;
  srcs.reserve(m_entries.size());
  for (const auto &e : m_entries)
    srcs.push_back(e->source);
  return srcs;
}

VkDescriptorSet
VideoPlayerPlacebo::get_open_thumbnail(const std::string &source) const {
  if (m_hover_player)
    return m_hover_player->get_open_thumbnail(source);
  (void)source;
  return VK_NULL_HANDLE;
}

VkDescriptorSet VideoPlayerPlacebo::hover_thumbnail(const std::string &source) {
  if (m_hover_player)
    return m_hover_player->hover_thumbnail(source);
  (void)source;
  return VK_NULL_HANDLE;
}

void VideoPlayerPlacebo::notify_hover(const std::string &source) {
  if (m_hover_player)
    m_hover_player->notify_hover(source);
}

bool VideoPlayerPlacebo::is_hover_previewing() const {
  return m_hover_player && m_hover_player->is_hover_previewing();
}

void VideoPlayerPlacebo::seek_hover_preview(double seconds) {
  if (m_hover_player)
    m_hover_player->seek_hover_preview(seconds);
}

bool VideoPlayerPlacebo::save_hover_frame(const std::filesystem::path &path) {
  if (m_hover_player)
    return m_hover_player->save_hover_frame(path);
  (void)path;
  return false;
}

bool VideoPlayerPlacebo::is_video_path(const std::filesystem::path &path) {
  return k_video_exts.count(path.extension().string()) > 0;
}

bool VideoPlayerPlacebo::is_video_url(const std::string &url) {
  const auto clean = url.substr(0, url.find('?'));
  const auto ext = std::filesystem::path(clean).extension().string();
  if (k_video_exts.count(ext) > 0)
    return true;
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

void VideoPlayerPlacebo::set_downloader(VideoDownloader *d) {
  m_downloader = d;
  if (m_hover_player)
    m_hover_player->set_downloader(d);
}

void VideoPlayerPlacebo::restart_hover_preview() {
  if (m_hover_player)
    m_hover_player->restart_hover_preview();
}

bool VideoPlayerPlacebo::consume_hover_popup_reopen_request() {
  if (m_hover_player)
    return m_hover_player->consume_hover_popup_reopen_request();
  return false;
}

bool VideoPlayerPlacebo::is_hover_dwell_pending(
    const std::string &source) const {
  if (m_hover_player)
    return m_hover_player->is_hover_dwell_pending(source);
  (void)source;
  return false;
}

bool VideoPlayerPlacebo::can_toggle_hwdec(const std::string &source) const {
  if (m_hover_player && m_hover_player->can_toggle_hwdec(source))
    return true;
  return !source.empty();
}

bool VideoPlayerPlacebo::is_hwdec_enabled(const std::string &source) const {
  if (m_hover_player)
    return m_hover_player->is_hwdec_enabled(source);
  (void)source;
  return m_config.enable_hwdec;
}

int VideoPlayerPlacebo::current_position_seconds(
    const std::string &source) const {
  if (m_hover_player)
    return m_hover_player->current_position_seconds(source);
  (void)source;
  return 0;
}

int VideoPlayerPlacebo::persisted_position_seconds(
    const std::string &source) const {
  if (m_hover_player)
    return m_hover_player->persisted_position_seconds(source);
  (void)source;
  return 0;
}

void VideoPlayerPlacebo::set_resume_persist_min_duration_seconds(int seconds) {
  m_resume_persist_min_duration_seconds = std::max(0, seconds);
}

int VideoPlayerPlacebo::resume_persist_min_duration_seconds() const {
  return m_resume_persist_min_duration_seconds;
}

void VideoPlayerPlacebo::toggle_hwdec(const std::string &source) {
  if (m_hover_player) {
    m_hover_player->toggle_hwdec(source);
    return;
  }
  (void)source;
  reconfigure(Config{
      .enable_hwdec = !m_config.enable_hwdec,
      .prefer_nvdec = !m_config.enable_hwdec,
  });
}

void VideoPlayerPlacebo::sync_history_state(
    std::vector<WindowStateToml::ImageHistoryEntry> &history) const {
  if (m_hover_player && m_hover_player->has_open_windows()) {
    m_hover_player->sync_history_state(history);
    return;
  }

  for (auto &hist_entry : history) {
    const bool found =
        std::any_of(m_entries.begin(), m_entries.end(), [&](const auto &e) {
          return e->source == hist_entry.source;
        });
    if (!found)
      continue;
    hist_entry.hwdec_enabled = m_config.enable_hwdec;
  }
}

void VideoPlayerPlacebo::set_all_hwdec(bool enabled) {
  reconfigure(Config{.enable_hwdec = enabled, .prefer_nvdec = enabled});
  if (m_hover_player)
    m_hover_player->set_all_hwdec(enabled);
}

void VideoPlayerPlacebo::set_all_loop(bool enabled) {
  m_global_loop_enabled = enabled;
  if (m_hover_player)
    m_hover_player->set_all_loop(enabled);
}

void VideoPlayerPlacebo::restart_all_threads() {
  if (m_hover_player)
    m_hover_player->restart_all_threads();
}

void VideoPlayerPlacebo::set_context_menu(
    VideoContextMenu *ctx,
    std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)>
        lookup,
    std::function<void(const std::string &)> on_erase) {
  m_ctx_menu = ctx;
  m_ctx_lookup = std::move(lookup);
  m_ctx_on_erase = std::move(on_erase);

  if (m_hover_player)
    m_hover_player->set_context_menu(m_ctx_menu, m_ctx_lookup, m_ctx_on_erase);
}

void VideoPlayerPlacebo::set_player_menu_callbacks(
    std::function<void()> on_open_image, std::function<void()> on_open_online,
    std::function<void(const std::string &, const std::string &)>
        on_open_recent,
    std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()>
        history,
    HistoryPreview *preview,
    std::function<void(const std::string &)> on_fix_videos,
    std::function<bool(const std::string &)> is_startup_videos_fixed,
    std::function<bool()> on_get_app_fullscreen,
    std::function<void(bool)> on_set_app_fullscreen) {
  m_on_open_image = std::move(on_open_image);
  m_on_open_online = std::move(on_open_online);
  m_on_open_recent = std::move(on_open_recent);
  m_history_provider = std::move(history);
  m_history_preview = preview;
  m_on_fix_videos = std::move(on_fix_videos);
  m_is_startup_videos_fixed = std::move(is_startup_videos_fixed);
  m_on_get_app_fullscreen = std::move(on_get_app_fullscreen);
  m_on_set_app_fullscreen = std::move(on_set_app_fullscreen);

  if (m_hover_player) {
    m_hover_player->set_player_menu_callbacks(
        m_on_open_image, m_on_open_online, m_on_open_recent, m_history_provider,
        m_history_preview, m_on_fix_videos, m_is_startup_videos_fixed,
        m_on_get_app_fullscreen, m_on_set_app_fullscreen);
  }
}

// ==========================================================================
// GPU pipeline – libplacebo + NVDEC no-copy
// ==========================================================================

bool VideoPlayerPlacebo::init_placebo_gpu() {
  if (!m_vk)
    return false;
  if (m_pl_renderer)
    return true; // already initialised

  // libplacebo requires the device to have been created with its required
  // features (hostQueryReset + timelineSemaphore).  vulkan_context only enables
  // them when the GPU supports them; bail to the SW path otherwise.
  if (!m_vk->placebo_features_enabled) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] device lacks libplacebo-required "
                  "Vulkan 1.2 features");
    return false;
  }

  // Load Vulkan extension function pointers
  s_vkGetMemoryFdKHR = std::bit_cast<PFN_vkGetMemoryFdKHR>(
      vkGetDeviceProcAddr(m_vk->device, "vkGetMemoryFdKHR"));
  s_vkGetSemaphoreFdKHR = std::bit_cast<PFN_vkGetSemaphoreFdKHR>(
      vkGetDeviceProcAddr(m_vk->device, "vkGetSemaphoreFdKHR"));

  if (!s_vkGetMemoryFdKHR || !s_vkGetSemaphoreFdKHR) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] VK external memory/semaphore functions "
                  "unavailable");
    return false;
  }

  // NOTE: GL interop extension functions are NOT loaded here.  On NVIDIA/Mesa
  // eglGetProcAddress() returns null for GL_EXT_memory_object / _semaphore
  // entry points until a GL context is current, and no EGL context exists yet
  // at GPU-init time (they are created lazily per entry).  s_gl.load() is
  // therefore deferred to entry_create_shared_image(), right after the entry's
  // EGL context is made current.

  // Create libplacebo log
  pl_log_params log_params{};
  log_params.log_cb = [](void *, pl_log_level lv, const char *msg) {
    if (lv <= PL_LOG_WARN)
      APP_DEBUG_LOG("[libplacebo] {}", msg);
  };
  log_params.log_level = PL_LOG_WARN;
  m_pl_log = pl_log_create(PL_API_VER, &log_params);
  if (!m_pl_log)
    return false;

  // Import our existing Vulkan device into libplacebo
  static const char *ext_names[] = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
  };
  pl_vulkan_import_params import_p{};
  import_p.instance = m_vk->instance;
  import_p.phys_device = m_vk->physical_device;
  import_p.device = m_vk->device;
  import_p.extensions = ext_names;
  import_p.num_extensions = 4;
  import_p.queue_graphics = {m_vk->queue_family, 1};
  import_p.queue_compute = {m_vk->queue_family, 1};
  import_p.queue_transfer = {m_vk->queue_family, 1};
  // Declare the features the device was created with.  vulkan_context enabled
  // exactly libplacebo's required set, so point at the canonical global.
  import_p.features = &pl_vulkan_required_features;

  m_pl_vk = pl_vulkan_import(m_pl_log, &import_p);
  if (!m_pl_vk) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] pl_vulkan_import failed");
    pl_log_destroy(&m_pl_log);
    return false;
  }

  m_pl_renderer = pl_renderer_create(m_pl_log, m_pl_vk->gpu);
  if (!m_pl_renderer) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] pl_renderer_create failed");
    pl_vulkan_destroy(&m_pl_vk);
    pl_log_destroy(&m_pl_log);
    return false;
  }

  APP_DEBUG_LOG("[VideoPlayerPlacebo] libplacebo GPU initialised");
  return true;
}

void VideoPlayerPlacebo::destroy_placebo_gpu() {
  if (m_pl_renderer) {
    pl_renderer_destroy(&m_pl_renderer);
  }
  if (m_pl_vk) {
    pl_vulkan_destroy(&m_pl_vk);
  }
  if (m_pl_log) {
    pl_log_destroy(&m_pl_log);
  }
}

// --------------------------------------------------------------------------
// Shared Vulkan ↔ GL image
// --------------------------------------------------------------------------
bool VideoPlayerPlacebo::entry_create_shared_image(PlaceboEntry &e, int w,
                                                   int h) {
  if (!m_vk || !m_pl_vk)
    return false;

  e.shared_w = w;
  e.shared_h = h;
  e.shared_format = VK_FORMAT_R8G8B8A8_UNORM;

  // 1. Create exportable VkImage
  VkExternalMemoryImageCreateInfo ext_img_ci{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext_img_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img_ci.pNext = &ext_img_ci;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = e.shared_format;
  img_ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  if (vkCreateImage(m_vk->device, &img_ci, m_vk->allocator, &e.shared_image) !=
      VK_SUCCESS) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] vkCreateImage (shared) failed");
    return false;
  }

  // 2. Allocate exportable memory
  VkMemoryRequirements mem_req{};
  vkGetImageMemoryRequirements(m_vk->device, e.shared_image, &mem_req);

  // Need a memory type that supports export
  VkPhysicalDeviceMemoryProperties mem_props{};
  vkGetPhysicalDeviceMemoryProperties(m_vk->physical_device, &mem_props);
  uint32_t mem_type = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if (!(mem_req.memoryTypeBits & (1u << i)))
      continue;
    if (mem_props.memoryTypes[i].propertyFlags &
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      mem_type = i;
      break;
    }
  }
  if (mem_type == 0xFFFFFFFFu) {
    APP_DEBUG_LOG(
        "[VideoPlayerPlacebo] no suitable memory type for shared image");
    return false;
  }

  VkExportMemoryAllocateInfo export_mem{
      VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  export_mem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkMemoryAllocateInfo mem_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mem_alloc.pNext = &export_mem;
  mem_alloc.allocationSize = mem_req.size;
  mem_alloc.memoryTypeIndex = mem_type;

  if (vkAllocateMemory(m_vk->device, &mem_alloc, m_vk->allocator,
                       &e.shared_memory) != VK_SUCCESS ||
      vkBindImageMemory(m_vk->device, e.shared_image, e.shared_memory, 0) !=
          VK_SUCCESS) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] shared image memory alloc/bind failed");
    return false;
  }

  // 3. Export FD from VkDeviceMemory
  VkMemoryGetFdInfoKHR get_fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  get_fd_info.memory = e.shared_memory;
  get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int mem_fd = -1;
  if (s_vkGetMemoryFdKHR(m_vk->device, &get_fd_info, &mem_fd) != VK_SUCCESS ||
      mem_fd < 0) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] vkGetMemoryFdKHR failed");
    return false;
  }

  // 4. GL: import memory, create texture backed by it, create FBO
  if (!e.egl.valid() && !e.egl.create()) {
    ::close(mem_fd);
    APP_DEBUG_LOG("[VideoPlayerPlacebo] EGL context create failed");
    return false;
  }
  if (!e.egl.make_current()) {
    ::close(mem_fd);
    return false;
  }

  // Load GL interop entry points now that a GL context is current.  This is
  // the earliest point eglGetProcAddress() can resolve GL_EXT_memory_object /
  // GL_EXT_semaphore functions on NVIDIA/Mesa.
  if (!s_gl.load()) {
    ::close(mem_fd);
    APP_DEBUG_LOG(
        "[VideoPlayerPlacebo] GL interop extension functions unavailable");
    return false;
  }

  s_gl.CreateMemoryObjectsEXT(1, &e.gl_mem_obj);
  s_gl.ImportMemoryFdEXT(e.gl_mem_obj, mem_req.size,
                         GL_HANDLE_TYPE_OPAQUE_FD_EXT, mem_fd);
  // FD ownership transferred to GL on success; no ::close needed

  glGenTextures(1, &e.gl_texture);
  glBindTexture(GL_TEXTURE_2D, e.gl_texture);
  s_gl.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, e.gl_mem_obj, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  s_gl.GenFramebuffers(1, &e.gl_fbo);
  s_gl.BindFramebuffer(GL_FRAMEBUFFER, e.gl_fbo);
  s_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            e.gl_texture, 0);
  const GLenum fbo_status = s_gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  s_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

  e.egl.release();

  if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] GL FBO incomplete: 0x{:x}", fbo_status);
    return false;
  }

  // 5. Wrap shared VkImage as pl_tex (held by us initially)
  {
    struct pl_vulkan_wrap_params wp{};
    wp.image = e.shared_image;
    wp.width = static_cast<int>(w);
    wp.height = static_cast<int>(h);
    wp.format = e.shared_format;
    wp.usage = img_ci.usage;
    e.pl_input_tex = pl_vulkan_wrap(m_pl_vk->gpu, &wp);
  }
  if (!e.pl_input_tex) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] pl_vulkan_wrap failed");
    return false;
  }

  // 6. Create pl_output_tex (libplacebo renders here)
  pl_fmt out_fmt = pl_find_fmt(
      m_pl_vk->gpu, PL_FMT_UNORM, 4, 8, 8,
      static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE));
  if (!out_fmt) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] pl_find_fmt RGBA8 failed");
    return false;
  }

  pl_tex_params out_tp{};
  out_tp.w = w;
  out_tp.h = h;
  out_tp.format = out_fmt;
  out_tp.renderable = true;
  out_tp.sampleable = true;
  out_tp.storable = true;
  e.pl_output_tex = pl_tex_create(m_pl_vk->gpu, &out_tp);
  if (!e.pl_output_tex) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] pl_tex_create (output) failed");
    return false;
  }

  // 7. Unwrap pl_output_tex to get VkImage, create ImGui view + descriptor
  VkFormat out_vk_fmt = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags out_vk_usage = 0;
  VkImage out_vk_img = pl_vulkan_unwrap(m_pl_vk->gpu, e.pl_output_tex,
                                        &out_vk_fmt, &out_vk_usage);

  VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_ci.image = out_vk_img;
  view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_ci.format =
      out_vk_fmt != VK_FORMAT_UNDEFINED ? out_vk_fmt : VK_FORMAT_R8G8B8A8_UNORM;
  view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(m_vk->device, &view_ci, m_vk->allocator,
                        &e.output_image_view) != VK_SUCCESS) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] vkCreateImageView (output) failed");
    return false;
  }

  VkSamplerCreateInfo samp_ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp_ci.magFilter = VK_FILTER_LINEAR;
  samp_ci.minFilter = VK_FILTER_LINEAR;
  samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp_ci.maxLod = 1.0f;
  if (vkCreateSampler(m_vk->device, &samp_ci, m_vk->allocator,
                      &e.output_sampler) != VK_SUCCESS) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] vkCreateSampler (output) failed");
    return false;
  }

  e.descriptor_set =
ImGui_ImplVulkan_AddTexture(e.output_image_view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  APP_DEBUG_LOG("[VideoPlayerPlacebo] shared image {}x{} created", w, h);
  return true;
}

void VideoPlayerPlacebo::entry_destroy_shared_image(PlaceboEntry &e) {
  if (!m_vk)
    return;

  // Destroy pl textures (must not be held)
  if (e.pl_input_tex) {
    pl_tex_destroy(m_pl_vk->gpu, &e.pl_input_tex);
  }
  if (e.pl_output_tex) {
    pl_tex_destroy(m_pl_vk->gpu, &e.pl_output_tex);
  }

  // Destroy ImGui resources
  entry_destroy_output_descriptor(e);

  // GL cleanup
  if (e.egl.valid()) {
    if (e.egl.make_current()) {
      if (e.gl_fbo) {
        s_gl.DeleteFramebuffers(1, &e.gl_fbo);
        e.gl_fbo = 0;
      }
      if (e.gl_texture) {
        glDeleteTextures(1, &e.gl_texture);
        e.gl_texture = 0;
      }
      if (e.gl_mem_obj && s_gl.loaded)
        s_gl.DeleteMemoryObjectsEXT(1, &e.gl_mem_obj);
      e.gl_mem_obj = 0;
      e.egl.release();
    }
  }

  if (e.shared_memory != VK_NULL_HANDLE) {
    vkFreeMemory(m_vk->device, e.shared_memory, m_vk->allocator);
    e.shared_memory = VK_NULL_HANDLE;
  }
  if (e.shared_image != VK_NULL_HANDLE) {
    vkDestroyImage(m_vk->device, e.shared_image, m_vk->allocator);
    e.shared_image = VK_NULL_HANDLE;
  }
  e.shared_w = e.shared_h = 0;
  e.pl_output_held = false;
}

// --------------------------------------------------------------------------
// Semaphore sync
// --------------------------------------------------------------------------
bool VideoPlayerPlacebo::entry_create_sync(PlaceboEntry &e) {
  if (!m_vk || !m_pl_vk || !s_gl.loaded)
    return false;

  // Helper: create an exportable VkSemaphore and export its FD
  auto make_exportable_sem = [&](VkSemaphore &out_vk, GLuint &out_gl) -> bool {
    VkExportSemaphoreCreateInfo exp_ci{
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exp_ci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sem_ci.pNext = &exp_ci;
    if (vkCreateSemaphore(m_vk->device, &sem_ci, m_vk->allocator, &out_vk) !=
        VK_SUCCESS)
      return false;

    VkSemaphoreGetFdInfoKHR get_fd{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    get_fd.semaphore = out_vk;
    get_fd.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (s_vkGetSemaphoreFdKHR(m_vk->device, &get_fd, &fd) != VK_SUCCESS ||
        fd < 0)
      return false;

    if (!e.egl.make_current()) {
      ::close(fd);
      return false;
    }
    s_gl.GenSemaphoresEXT(1, &out_gl);
    s_gl.ImportSemaphoreFdEXT(out_gl, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    // GL takes ownership of fd on success
    e.egl.release();
    return out_gl != 0;
  };

  if (!make_exportable_sem(e.vk_ready_sem, e.gl_ready_sem))
    return false;
  if (!make_exportable_sem(e.vk_release_sem, e.gl_release_sem))
    return false;

  // output_hold_sem: plain binary semaphore (no export needed)
  {
    struct pl_vulkan_sem_params sp{};
    sp.type = VK_SEMAPHORE_TYPE_BINARY;
    e.output_hold_sem = pl_vulkan_sem_create(m_pl_vk->gpu, &sp);
  }
  if (!e.output_hold_sem)
    return false;

  // Interop command pool + buffer + fence for CPU waiting on output_hold_sem
  VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_ci.queueFamilyIndex = m_vk->queue_family;
  pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(m_vk->device, &pool_ci, m_vk->allocator,
                          &e.interop_cmd_pool) != VK_SUCCESS)
    return false;

  VkCommandBufferAllocateInfo cmd_alloc{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmd_alloc.commandPool = e.interop_cmd_pool;
  cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmd_alloc.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(m_vk->device, &cmd_alloc, &e.interop_cmd_buf) !=
      VK_SUCCESS)
    return false;

  VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(m_vk->device, &fence_ci, m_vk->allocator,
                    &e.interop_fence) != VK_SUCCESS)
    return false;

  return true;
}

void VideoPlayerPlacebo::entry_destroy_sync(PlaceboEntry &e) {
  if (!m_vk)
    return;

  if (e.interop_fence != VK_NULL_HANDLE) {
    vkDestroyFence(m_vk->device, e.interop_fence, m_vk->allocator);
    e.interop_fence = VK_NULL_HANDLE;
  }
  if (e.interop_cmd_pool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(m_vk->device, e.interop_cmd_pool, m_vk->allocator);
    e.interop_cmd_pool = VK_NULL_HANDLE;
    e.interop_cmd_buf = VK_NULL_HANDLE;
  }
  if (e.output_hold_sem && m_pl_vk)
    pl_vulkan_sem_destroy(m_pl_vk->gpu, &e.output_hold_sem);

  // GL semaphore cleanup
  if (e.egl.valid() && s_gl.loaded && e.egl.make_current()) {
    if (e.gl_ready_sem) {
      s_gl.DeleteSemaphoresEXT(1, &e.gl_ready_sem);
      e.gl_ready_sem = 0;
    }
    if (e.gl_release_sem) {
      s_gl.DeleteSemaphoresEXT(1, &e.gl_release_sem);
      e.gl_release_sem = 0;
    }
    e.egl.release();
  }

  if (e.vk_ready_sem != VK_NULL_HANDLE) {
    vkDestroySemaphore(m_vk->device, e.vk_ready_sem, m_vk->allocator);
    e.vk_ready_sem = VK_NULL_HANDLE;
  }
  if (e.vk_release_sem != VK_NULL_HANDLE) {
    vkDestroySemaphore(m_vk->device, e.vk_release_sem, m_vk->allocator);
    e.vk_release_sem = VK_NULL_HANDLE;
  }
}

// --------------------------------------------------------------------------
// mpv OpenGL render context
// --------------------------------------------------------------------------
bool VideoPlayerPlacebo::entry_create_mpv(PlaceboEntry &e,
                                          const std::string &path, bool hwdec) {
  e.mpv = mpv_create();
  if (!e.mpv)
    return false;

  mpv_set_option_string(e.mpv, "terminal", "no");
  mpv_set_option_string(e.mpv, "vo", "libmpv");
  mpv_set_option_string(e.mpv, "vo", "gpu-next");

  mpv_set_option_string(e.mpv, "gpu-api", "opengl");
  mpv_set_option_string(e.mpv, "hwdec", hwdec ? "nvdec" : "no");
  mpv_set_option_string(e.mpv, "loop-file", "yes");
  mpv_set_option_string(e.mpv, "pause", "no");

  if (mpv_initialize(e.mpv) < 0) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] mpv_initialize failed");
    mpv_terminate_destroy(e.mpv);
    e.mpv = nullptr;
    return false;
  }

  // Create EGL context for this entry
  if (!e.egl.valid() && !e.egl.create()) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] EGL create failed");
    mpv_terminate_destroy(e.mpv);
    e.mpv = nullptr;
    return false;
  }
  if (!e.egl.make_current()) {
    mpv_terminate_destroy(e.mpv);
    e.mpv = nullptr;
    return false;
  }

  mpv_opengl_init_params gl_init{};
  gl_init.get_proc_address = PlaceboEglContext::get_proc_address;
  gl_init.get_proc_address_ctx = nullptr;

  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE,
       const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };

  if (mpv_render_context_create(&e.render_ctx, e.mpv, params) < 0) {
    APP_DEBUG_LOG("[VideoPlayerPlacebo] mpv_render_context_create failed");
    e.egl.release();
    mpv_terminate_destroy(e.mpv);
    e.mpv = nullptr;
    return false;
  }

  // Update callback – fired from mpv thread
  mpv_render_context_set_update_callback(
      e.render_ctx,
      [](void *ctx) {
        auto *entry = static_cast<PlaceboEntry *>(ctx);
        entry->frame_dirty.store(true, std::memory_order_release);
      },
      &e);

  e.egl.release();

  // Load file
  const char *cmd[] = {"loadfile", path.c_str(), nullptr};
  mpv_command(e.mpv, cmd);

  if (e.resume_position_seconds > 0)
    e.resume_seek_pending = true;

  APP_DEBUG_LOG("[VideoPlayerPlacebo] mpv entry created: {}", path);
  return true;
}

void VideoPlayerPlacebo::entry_destroy_mpv(PlaceboEntry &e) {
  if (e.render_ctx) {
    if (e.egl.make_current()) {
      mpv_render_context_free(e.render_ctx);
      e.render_ctx = nullptr;
      e.egl.release();
    }
  }
  if (e.mpv) {
    mpv_terminate_destroy(e.mpv);
    e.mpv = nullptr;
  }
  e.egl.destroy();
}

// --------------------------------------------------------------------------
// Per-entry event polling
// --------------------------------------------------------------------------
void VideoPlayerPlacebo::entry_poll_events(PlaceboEntry &e) {
  if (!e.mpv)
    return;
  mpv_event *ev;
  while ((ev = mpv_wait_event(e.mpv, 0)) && ev->event_id != MPV_EVENT_NONE) {
    if (ev->event_id == MPV_EVENT_END_FILE) {
      const auto *data = static_cast<mpv_event_end_file *>(ev->data);
      if (data->reason == MPV_END_FILE_REASON_ERROR) {
        APP_DEBUG_LOG("[VideoPlayerPlacebo] mpv end file error: {}",
                      mpv_error_string(data->error));
        e.load_failed = true;
      }
    }
    if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG && !e.load_failed) {
      // Query new dimensions – ignore for now, keep existing shared image
    }
  }
}

// --------------------------------------------------------------------------
// Per-frame render: GL → libplacebo → output tex (ImGui)
// --------------------------------------------------------------------------
void VideoPlayerPlacebo::entry_render_frame(PlaceboEntry &e) {
  if (!e.mpv || !e.render_ctx || e.load_failed)
    return;
  if (!m_pl_vk || !m_pl_renderer)
    return;

  // ── Lazy shared image creation ──────────────────────────────────────────
  if (e.shared_image == VK_NULL_HANDLE) {
    int64_t dw = 1920, dh = 1080;
    mpv_get_property(e.mpv, "dwidth", MPV_FORMAT_INT64, &dw);
    mpv_get_property(e.mpv, "dheight", MPV_FORMAT_INT64, &dh);
    if (dw <= 0)
      dw = 1920;
    if (dh <= 0)
      dh = 1080;
    if (!entry_create_shared_image(e, static_cast<int>(dw),
                                   static_cast<int>(dh))) {
      APP_DEBUG_LOG("[VideoPlayerPlacebo] entry_create_shared_image failed");
      e.load_failed = true;
      return;
    }
    if (!entry_create_sync(e)) {
      APP_DEBUG_LOG("[VideoPlayerPlacebo] entry_create_sync failed");
      e.load_failed = true;
      return;
    }
    if (e.resume_seek_pending) {
      const std::string seek_cmd = std::to_string(e.resume_position_seconds);
      const char *sc[] = {"seek", seek_cmd.c_str(), "absolute", nullptr};
      mpv_command(e.mpv, sc);
      e.resume_seek_pending = false;
    }
  }

  // ── If output was held from last frame, release it back to libplacebo ──
  if (e.pl_output_held) {
    struct pl_vulkan_release_params rp{};
    rp.tex = e.pl_output_tex;
    rp.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rp.qf = m_vk->queue_family;
    pl_vulkan_release_ex(m_pl_vk->gpu, &rp);
    e.pl_output_held = false;
  }

  // ── GL: render mpv into FBO ─────────────────────────────────────────────
  if (!e.egl.make_current())
    return;

  // For frames after the first, wait for Vulkan to release the input image
  if (!e.first_gl_render && e.gl_release_sem) {
    GLenum src_layout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    s_gl.WaitSemaphoreEXT(e.gl_release_sem, 0, nullptr, 1, &e.gl_texture,
                          &src_layout);
  }

  // Render mpv frame into gl_fbo
  int flip_y = 0;
  mpv_opengl_fbo fbo_params{static_cast<int>(e.gl_fbo), e.shared_w, e.shared_h,
                            GL_RGBA8};
  mpv_render_param render_params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_params},
      {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(e.render_ctx, render_params);

  // Signal that GL has finished writing the input image
  GLenum dst_layout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
  s_gl.SignalSemaphoreEXT(e.gl_ready_sem, 0, nullptr, 1, &e.gl_texture,
                          &dst_layout);

  // Flush GL commands so signal is submitted to GPU
  glFlush();
  e.egl.release();

  e.first_gl_render = false;

  // ── Vulkan: hand shared image to libplacebo ─────────────────────────────
  {
    struct pl_vulkan_release_params rp{};
    rp.tex = e.pl_input_tex;
    rp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rp.qf = VK_QUEUE_FAMILY_EXTERNAL;
    rp.semaphore = {e.vk_ready_sem, 0};
    pl_vulkan_release_ex(m_pl_vk->gpu, &rp);
  }

  // ── libplacebo render ───────────────────────────────────────────────────
  // Build input pl_frame
  pl_frame image{};
  image.num_planes = 1;
  image.planes[0].texture = e.pl_input_tex;
  image.planes[0].components = 4;
  image.planes[0].component_mapping[0] = PL_CHANNEL_R;
  image.planes[0].component_mapping[1] = PL_CHANNEL_G;
  image.planes[0].component_mapping[2] = PL_CHANNEL_B;
  image.planes[0].component_mapping[3] = PL_CHANNEL_A;
  image.planes[0].flipped = true; // OpenGL convention
  image.crop = {0.0f, 0.0f, static_cast<float>(e.shared_w),
                static_cast<float>(e.shared_h)};
  image.repr.sys = PL_COLOR_SYSTEM_RGB;
  image.repr.levels = PL_COLOR_LEVELS_FULL;
  image.color = pl_color_space_srgb;

  // Build output pl_frame
  pl_frame target{};
  target.num_planes = 1;
  target.planes[0].texture = e.pl_output_tex;
  target.planes[0].components = 4;
  target.planes[0].component_mapping[0] = PL_CHANNEL_R;
  target.planes[0].component_mapping[1] = PL_CHANNEL_G;
  target.planes[0].component_mapping[2] = PL_CHANNEL_B;
  target.planes[0].component_mapping[3] = PL_CHANNEL_A;
  target.crop = {0.0f, 0.0f, static_cast<float>(e.shared_w),
                 static_cast<float>(e.shared_h)};
  target.repr.sys = PL_COLOR_SYSTEM_RGB;
  target.repr.levels = PL_COLOR_LEVELS_FULL;
  target.color = pl_color_space_srgb;

  pl_render_image(m_pl_renderer, &image, &target, &pl_render_default_params);

  // Return input image to GL (signals vk_release_sem when libplacebo done)
  {
    struct pl_vulkan_hold_params hp{};
    hp.tex = e.pl_input_tex;
    hp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hp.qf = VK_QUEUE_FAMILY_EXTERNAL;
    hp.semaphore = {e.vk_release_sem, 0};
    pl_vulkan_hold_ex(m_pl_vk->gpu, &hp);
  }

  // Hold output image in SHADER_READ_ONLY so ImGui can sample it
  {
    struct pl_vulkan_hold_params hp{};
    hp.tex = e.pl_output_tex;
    hp.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hp.qf = m_vk->queue_family;
    hp.semaphore = {e.output_hold_sem, 0};
    pl_vulkan_hold_ex(m_pl_vk->gpu, &hp);
  }

  // Flush all pending libplacebo GPU commands
  pl_gpu_flush(m_pl_vk->gpu);

  // Submit an empty command that waits on output_hold_sem + signals fence
  // so we know on the CPU when the output is SHADER_READ_ONLY
  {
    vkResetCommandBuffer(e.interop_cmd_buf, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(e.interop_cmd_buf, &begin);
    vkEndCommandBuffer(e.interop_cmd_buf);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &e.output_hold_sem;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &e.interop_cmd_buf;

    vkResetFences(m_vk->device, 1, &e.interop_fence);
    m_vk->queue_submit(1, &submit, e.interop_fence);
    vkWaitForFences(m_vk->device, 1, &e.interop_fence, VK_TRUE, UINT64_MAX);
  }

  e.pl_output_held = true;
  APP_DEBUG_LOG("[VideoPlayerPlacebo] frame rendered id={}", e.id);
}

// --------------------------------------------------------------------------
// Cleanup helpers
// --------------------------------------------------------------------------
void VideoPlayerPlacebo::entry_destroy_output_descriptor(PlaceboEntry &e) {
  if (!m_vk)
    return;
  if (e.descriptor_set != VK_NULL_HANDLE) {
    ImGui_ImplVulkan_RemoveTexture(e.descriptor_set);
    e.descriptor_set = VK_NULL_HANDLE;
  }
  if (e.output_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(m_vk->device, e.output_sampler, m_vk->allocator);
    e.output_sampler = VK_NULL_HANDLE;
  }
  if (e.output_image_view != VK_NULL_HANDLE) {
    vkDestroyImageView(m_vk->device, e.output_image_view, m_vk->allocator);
    e.output_image_view = VK_NULL_HANDLE;
  }
}

void VideoPlayerPlacebo::entry_destroy_all(PlaceboEntry &e) {
  // 1. Destroy mpv first (stops decoding/rendering)
  entry_destroy_mpv(e);
  // 2. GPU idle
  if (m_pl_vk)
    pl_gpu_finish(m_pl_vk->gpu);
  // 3. Release held textures
  if (e.pl_output_held && e.pl_output_tex && m_pl_vk) {
    struct pl_vulkan_release_params rp{};
    rp.tex = e.pl_output_tex;
    rp.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rp.qf = m_vk ? m_vk->queue_family : VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(m_pl_vk->gpu, &rp);
    e.pl_output_held = false;
  }
  // pl_input_tex starts held by user; destroy via pl_tex_destroy
  // which handles this case (from pl_vulkan_wrap doc: safe to destroy if held)
  // 4. Shared image / FBO
  entry_destroy_shared_image(e);
  // 5. Semaphores / interop resources
  entry_destroy_sync(e);
}

void VideoPlayerPlacebo::shutdown() {
  if (!m_initialized && m_vk == nullptr)
    return;

  // GPU must be idle before destroying resources
  if (m_vk)
    vkDeviceWaitIdle(m_vk->device);

  for (auto &e : m_entries)
    entry_destroy_all(*e);

  if (m_hover_player)
    m_hover_player->shutdown();

  m_entries.clear();
  destroy_placeholder_texture();
  destroy_placebo_gpu();

  m_initialized = false;
  m_vk = nullptr;
}

bool VideoPlayerPlacebo::initialized() const noexcept { return m_initialized; }

const VideoPlayerPlacebo::Config &VideoPlayerPlacebo::config() const noexcept {
  return m_config;
}

void VideoPlayerPlacebo::reset_to_defaults() { reconfigure(kDefaultConfig); }
