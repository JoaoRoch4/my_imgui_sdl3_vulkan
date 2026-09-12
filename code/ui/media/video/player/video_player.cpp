#include "pch.hpp" // NOLINT

#include "VideoEntry.hpp"
#include "core/log/debug_log.hpp"
#include "history_preview.hpp"
#include "video_context_menu.hpp"
#include "video_downloader.hpp"
#include "video_hover_preview.hpp"
#include "video_osd_overlay.hpp"
#include "video_player.hpp"
#include "video_seek_preview.hpp"
#include "video_ui_window.hpp"
#include "window_state_toml.hpp"


#include "vulkan_context.hpp"
#include "vulkan_upload_context.hpp"




// ============================================================================
// Extension sets
// ============================================================================

using std::move;

/**
 * @brief Reads the GPU model name from the NVIDIA kernel driver's sysfs tree
 *        or, as a fallback, from the DRM subsystem's device name file.
 *
 * Primary path — NVIDIA proprietary driver:
 *   /proc/driver/nvidia/gpus/<PCI-address>/information
 *   Looks for the line beginning with "Model:" and returns its value.
 *
 * Fallback path — DRM (open-source drivers, Mesa, etc.):
 *   /sys/class/drm/card0/device/product_name
 *   /sys/class/drm/card1/device/product_name  … (first non-empty file wins)
 *
 * @return A string such as "NVIDIA GeForce RTX 3060", or "Unknown GPU" if
 *         neither path produces a usable result.
 */
[[nodiscard]] static std::string read_gpu_name_linux() {
	namespace fs = std::filesystem;
	std::error_code ec;

	// -- Primary: NVIDIA proprietary sysfs tree --------------------------------
	fs::path const nvidia_base("/proc/driver/nvidia/gpus");
	if (fs::exists(nvidia_base, ec)) {
		for (auto const &gpu_dir : fs::directory_iterator(nvidia_base, ec)) {
			// Each subdirectory is a PCI address; the "information" file inside
			// contains human-readable key=value pairs about the device.
			fs::path const info_file = gpu_dir.path() / "information";
			std::ifstream  f(info_file);
			if (!f.is_open())
				continue; // directory without the file

			std::string line;
			while (std::getline(f, line)) {
				if (!line.starts_with("Model:"))
					continue;

				std::size_t const colon = line.find(':');
				if (colon == std::string::npos)
					continue;

				// Skip ": " after the colon.
				std::string name = line.substr(colon + 2);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
					name.pop_back();

				return name; // e.g. "NVIDIA GeForce RTX 3060"
			}
		}
	}

	// -- Fallback: DRM product_name (Mesa / open-source / Intel / AMD) ---------
	for (int card_idx = 0; card_idx < 4; ++card_idx) {
		// Probe up to card3; most systems have only one or two GPU nodes.
		fs::path const product_file
			= fs::path("/sys/class/drm") / ("card" + std::to_string(card_idx)) / "device" / "product_name";

		std::ifstream f(product_file);
		if (!f.is_open())
			continue; // card node absent

		std::string name;
		std::getline(f, name); // single-line file

		// Trim trailing whitespace and newlines.
		while (!name.empty() && (name.back() == ' ' || name.back() == '\n' || name.back() == '\r'))
			name.pop_back();

		if (!name.empty())
			return name; // first non-empty name wins
	}

	return "Unknown GPU"; // neither path succeeded
}
// ============================================================================
// Linux system-info helpers (OSD)
// ============================================================================

/**
 * @brief Reads the CPU model name from the Linux kernel's /proc/cpuinfo.
 *
 * Iterates lines looking for the "model name" key.  Only the first occurrence
 * (physical socket 0, logical core 0) is returned; for multi-socket machines
 * the first socket's label is representative enough for an OSD string.
 *
 * If the file cannot be opened, or the key is absent (unlikely on any x86-64
 * Linux), the function returns the literal string "Unknown CPU" so callers
 * never receive an empty string.
 *
 * @return A trimmed string such as "Intel(R) Core(TM) i7-7700 CPU @ 3.60GHz".
 */
[[nodiscard]] static std::string read_cpu_name_linux() {
	// Open the virtual file exposed by the kernel for CPU topology.
	std::ifstream f("/proc/cpuinfo");
	if (!f.is_open())
		return "Unknown CPU"; // file not available

	std::string line;
	while (std::getline(f, line)) {
		// The "model name" key appears once per logical core; grab the first hit.
		if (!line.starts_with("model name"))
			continue;

		std::size_t const colon = line.find(':'); // find the separator
		if (colon == std::string::npos)
			continue; // malformed line — skip

		// Skip ": " (colon + space) to get the bare model string.
		std::string name = line.substr(colon + 2);

		// Trim any trailing whitespace or carriage-return left by the kernel.
		while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
			name.pop_back();

		return name; // first occurrence is enough
	}
	return "Unknown CPU"; // key not found
}

namespace {




std::unordered_set<std::string> const k_video_exts = {
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

std::unordered_set<std::string> const k_audio_exts = {
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
std::unordered_set<std::string> const k_image_exts = {
	".jpg",
	".jpeg",
	".png",
	".bmp",
	".tga",
	".webp",
};

// Hostnames whose URLs mpv/yt-dlp can stream directly.
std::unordered_set<std::string> const k_streaming_hosts = {
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
// VideoPlayer constructor / destructor
// ============================================================================

VideoPlayer::VideoPlayer()
	: m_hover {std::make_unique<VideoHoverPreview>()}
	, m_seek_uploader {std::make_unique<VulkanUploadContext>()}
	, m_ui_window {std::make_unique<VideoUiWindow>()}
	, m_vk {nullptr}
	, m_entries {}
	, m_next_id {0}
	, m_resume_persist_min_duration_seconds {WindowStateToml {}.video_resume_persist_min_duration_seconds}
	, m_ctx_menu {nullptr}
	, m_ctx_lookup {}
	, m_ctx_on_erase {}
	, m_on_open_image {}
	, m_on_open_online {}
	, m_on_open_recent {}
	, m_history_provider {}
	, m_history_preview {nullptr}
	, m_on_fix_videos {}
	, m_is_startup_videos_fixed {}
	, m_on_get_app_fullscreen {}
	, m_on_set_app_fullscreen {}
	, m_downloader {nullptr} { }

VideoPlayer::~VideoPlayer() {
	if (m_vk)
		shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

void VideoPlayer::bind_context(vulkan_context *vk) {
	m_vk             = vk;
	m_main_thread_id = std::this_thread::get_id();
}

void VideoPlayer::setup(vulkan_context *vk) {
	APP_DEBUG_LOG("[VideoPlayer] setup");
	m_main_thread_id                                = std::this_thread::get_id();
	m_vk                                            = vk;
	constexpr VkDeviceSize k_max_seek_preview_bytes = static_cast<VkDeviceSize>(1920) * 1920 * 4;
	m_seek_uploader->init(m_vk, static_cast<size_t>(k_max_seek_preview_bytes));
	m_hover->setup(vk);
	m_initialized       = true;
	m_hover_initialized = true;
}

void VideoPlayer::shutdown() {
	APP_DEBUG_LOG("[VideoPlayer] shutdown begin");
	if (!m_vk)
		return;

	// Stop all background threads before vkDeviceWaitIdle
	m_hover->stop_thread();
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

	m_hover->shutdown();
	m_seek_uploader->shutdown();

	m_vk = nullptr;
	APP_DEBUG_LOG("[VideoPlayer] shutdown done");
}

// ============================================================================
// Static helpers
// ============================================================================

bool VideoPlayer::is_video_path(std::filesystem::path const &path) {
	auto const ext = path.extension().string();
	return k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0;
}

bool VideoPlayer::is_video_url(std::string const &url) {
	bool const has_scheme = url.find("://") != std::string::npos;

	// 1. Known streaming hostnames — always route to mpv regardless of extension
	auto const host_start = url.find("://");
	if (host_start != std::string::npos) {
		auto const        path_start = url.find('/', host_start + 3);
		std::string const host       = url.substr(host_start + 3,
            path_start == std::string::npos ? std::string::npos : path_start - (host_start + 3));
		if (k_streaming_hosts.count(host) > 0)
			return true;
	}

	// 2. Check extension (strip query string first)
	auto const clean = url.substr(0, url.find('?'));
	auto const ext   = std::filesystem::path(clean).extension().string();
	if (has_scheme && (k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0))
		return true;

	// 3. Any http/https URL that doesn't look like a still image → let mpv try
	bool const is_http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
	if (is_http && k_image_exts.count(ext) == 0 && ext.empty())
		return true;

	return false;
}

uint32_t VideoPlayer::find_memory_type(VkPhysicalDevice phys, uint32_t filter, VkMemoryPropertyFlags props) {
	VkPhysicalDeviceMemoryProperties mem {};
	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
		if ((filter & (1u << i)) && ((mem.memoryTypes[i].propertyFlags & props) == props))
			return i;
	return 0xFFFFFFFFu;
}

// ============================================================================
// Add entries
// ============================================================================

bool VideoPlayer::add_from_path(std::filesystem::path const &path) {
	return add_from_path(path, {}, {}, m_global_hwdec_enabled, 0, {});
}

bool VideoPlayer::add_from_path(std::filesystem::path const &path, std::string const &title) {
	return add_from_path(path, title, {}, m_global_hwdec_enabled, 0, {});
}

bool VideoPlayer::add_from_path(std::filesystem::path const &path, std::string const &title,
	std::string const &logical_source) {
	return add_from_path(path, title, logical_source, m_global_hwdec_enabled, 0, {});
}

bool VideoPlayer::add_from_path(std::filesystem::path const &path, std::string const &title,
	std::string const &logical_source, bool hwdec_enabled, int resume_position_seconds,
	std::string const &initial_osd_message) {
	APP_DEBUG_LOG("[VideoPlayer] add_from_path: {}", path.string());
	auto ep                     = std::make_unique<VideoEntry>();
	ep->title                   = title.empty() ? path.filename().string() : title;
	ep->source                  = logical_source.empty() ? path.string() : logical_source;
	ep->playback_source         = path.string();
	ep->kind                    = (path.extension().string() == ".gif") ? "gif" : "video";
	ep->id                      = m_next_id++;
	ep->hwdec_enabled           = hwdec_enabled;
	ep->resume_position_seconds = std::max(resume_position_seconds, 0);
	ep->resume_seek_pending     = ep->resume_position_seconds > 0;

	ep->mpv = mpv_create();
	if (!ep->mpv)
		return false;

	mpv_set_option_string(ep->mpv, "hwdec", ep->hwdec_enabled ? "nvdec" : "no");
	if (ep->hwdec_enabled)
		mpv_set_option_string(ep->mpv, "hwdec-codecs", "h264,hevc,av1,vp9,mpeg4,vc1");
	mpv_set_option_string(ep->mpv, "vo", "libmpv");
	if (ep->kind == "gif" || m_global_loop_enabled) {
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
		[](void *ctx) { static_cast<VideoEntry *>(ctx)->frame_dirty.store(true, std::memory_order_release); }, raw);

	char const *cmd[] = {"loadfile", ep->playback_source.c_str(), nullptr};
	mpv_command_async(ep->mpv, 0, cmd);
	{
		int start_paused = 1;
		mpv_set_property(ep->mpv, "pause", MPV_FORMAT_FLAG, &start_paused);
	}
	{
		int start_paused = 1;
		mpv_set_property(ep->mpv, "pause", MPV_FORMAT_FLAG, &start_paused);
	}

	ep->seek_preview.prepare(m_vk, m_seek_uploader.get(), ep->playback_source);
	if (!initial_osd_message.empty())
		ep->osd.show(initial_osd_message);

	m_entries.push_back(std::move(ep));
	return true;
}

bool VideoPlayer::add_from_url(std::string const &url, std::string const &title) {
	return add_from_url(url, title, m_global_hwdec_enabled, 0, {});
}

bool VideoPlayer::add_from_url(std::string const &url, std::string const &title, bool hwdec_enabled,
	int resume_position_seconds, std::string const &initial_osd_message) {
	APP_DEBUG_LOG("[VideoPlayer] add_from_url: {} (title='{}')", url, title);
	auto ep                     = std::make_unique<VideoEntry>();
	ep->title                   = title;
	ep->source                  = url;
	ep->playback_source         = url;
	ep->kind                    = "video";
	ep->id                      = m_next_id++;
	ep->hwdec_enabled           = hwdec_enabled;
	ep->resume_position_seconds = std::max(resume_position_seconds, 0);
	ep->resume_seek_pending     = ep->resume_position_seconds > 0;

	ep->mpv = mpv_create();
	if (!ep->mpv)
		return false;
	mpv_set_option_string(ep->mpv, "vo", "libmpv");

	mpv_set_option_string(ep->mpv, "hwdec", ep->hwdec_enabled ? "nvdec" : "no");
	if (ep->hwdec_enabled){
		mpv_set_option_string(ep->mpv, "hwdec-codecs", "h264,hevc,av1,vp9,mpeg4,vc1");
	mpv_set_option_string(ep->mpv, "gpu-api", "vulkan");
    }
	mpv_set_option_string(ep->mpv, "vo", "libmpv");
	// yt-dlp integration — lets mpv stream YouTube, Vimeo, Twitch, etc.
	mpv_set_option_string(ep->mpv, "ytdl", "yes");
	mpv_set_option_string(ep->mpv, "ytdl-format", "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best");
	// Network buffering
	mpv_set_option_string(ep->mpv, "cache", "yes");
	mpv_set_option_string(ep->mpv, "demuxer-max-bytes", "150MiB");
	mpv_set_option_string(ep->mpv, "demuxer-max-back-bytes", "50MiB");
	mpv_set_option_string(ep->mpv, "demuxer-readahead-secs", "30");
	mpv_set_option_string(ep->mpv, "stream-buffer-size", "4MiB");
	if (m_global_loop_enabled) {
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
		[](void *ctx) { static_cast<VideoEntry *>(ctx)->frame_dirty.store(true, std::memory_order_release); }, raw);

	char const *cmd[] = {"loadfile", url.c_str(), nullptr};
	mpv_command_async(ep->mpv, 0, cmd);

	// Seek preview requires a locally cached file; skip for remote URLs.
	if (!initial_osd_message.empty())
		ep->osd.show(initial_osd_message);

	m_entries.push_back(std::move(ep));
	return true;
}

// ============================================================================
// create_gpu_resources
// ============================================================================

/**
 * @brief Allocates all Vulkan resources needed to display one video entry.
 *
 * Called once after VIDEO_RECONFIG delivers the decoded frame dimensions.
 * If the dimensions change later the resources are destroyed and recreated.
 *
 * The staging pipeline uses two independent slots (double buffering) so the
 * GPU can finish reading slot N while slot N+1 is already being filled by mpv.
 * A single shared VkCommandPool covers both slots; per-slot VkCommandBuffers and
 * VkFences track each upload independently.
 *
 * After allocation the image is immediately transitioned from UNDEFINED to
 * SHADER_READ_ONLY_OPTIMAL so ImGui can sample it before the first real frame
 * arrives.  Slot 0's command buffer and fence are borrowed for this one-time
 * bootstrap submit and reset before the first real upload.
 *
 * @param e  The video entry to initialise.
 * @return true on success; false if any critical Vulkan call fails.
 */
bool VideoPlayer::create_gpu_resources(VideoEntry &e) {
	APP_DEBUG_LOG("[VideoPlayer] create_gpu_resources id={} {}x{}", e.id, e.video_w, e.video_h);

	int const w = e.video_w; // frame width in pixels
	int const h = e.video_h; // frame height in pixels

	// Byte size of one complete RGBA frame: width × height × 4 channels.
	VkDeviceSize const bytes = static_cast<VkDeviceSize>(w) * h * 4;

	// -------------------------------------------------------------------------
	// VkImage — device-local, OPTIMAL tiling, sampled + transfer-dst usage.
	// -------------------------------------------------------------------------
	{
		VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.imageType         = VK_IMAGE_TYPE_2D;
		info.format            = VK_FORMAT_R8G8B8A8_UNORM; // RGBA 8-bit
		info.extent            = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u}; // 2-D single-depth
		info.mipLevels         = 1; // no mip chain
		info.arrayLayers       = 1; // single layer
		info.samples           = VK_SAMPLE_COUNT_1_BIT; // no MSAA
		info.tiling            = VK_IMAGE_TILING_OPTIMAL; // GPU-optimal layout
		info.usage             = VK_IMAGE_USAGE_SAMPLED_BIT // ImGui samples it
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT; // copy target
		info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE; // one queue family
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // transitioned below

		if (vkCreateImage(m_vk->device, &info, m_vk->allocator, &e.image) != VK_SUCCESS)
			return false; // hard failure

		VkMemoryRequirements req {};
		vkGetImageMemoryRequirements(m_vk->device, e.image, &req); // query size/alignment

		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize       = req.size;
		alloc.memoryTypeIndex      = find_memory_type(m_vk->physical_device, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); // GPU-only

		if (vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &e.image_memory) != VK_SUCCESS)
			return false; // hard failure

		vkBindImageMemory(m_vk->device, e.image, e.image_memory, 0); // bind at offset 0
	}

	// -------------------------------------------------------------------------
	// VkImageView — covers the full colour subresource.
	// -------------------------------------------------------------------------
	{
		VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		info.image                 = e.image;
		info.viewType              = VK_IMAGE_VIEW_TYPE_2D;
		info.format                = VK_FORMAT_R8G8B8A8_UNORM;
		info.subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; // mip 0, layer 0
		vkCreateImageView(m_vk->device, &info, m_vk->allocator, &e.image_view);
	}

	// -------------------------------------------------------------------------
	// VkSampler — linear filtering, clamp-to-border (black).
	// -------------------------------------------------------------------------
	{
		VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		info.magFilter           = VK_FILTER_LINEAR; // smooth upscale
		info.minFilter           = VK_FILTER_LINEAR; // smooth downscale
		info.addressModeU        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; // no wrap on X
		info.addressModeV        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; // no wrap on Y
		info.borderColor         = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; // black letterbox
		vkCreateSampler(m_vk->device, &info, m_vk->allocator, &e.sampler);
	}

	// -------------------------------------------------------------------------
	// ImGui descriptor — registers the sampler + view as a draw-able texture.
	// -------------------------------------------------------------------------
	e.descriptor_set = ImGui_ImplVulkan_AddTexture(e.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// -------------------------------------------------------------------------
	// Command pool — shared by both staging slots, reset per command buffer.
	// -------------------------------------------------------------------------
	{
		VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		info.queueFamilyIndex        = m_vk->queue_family;
		info.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // individual resets
		vkCreateCommandPool(m_vk->device, &info, m_vk->allocator, &e.cmd_pool);
	}

	// -------------------------------------------------------------------------
	// Double-buffered staging slots.
	//
	// Iterating with index rather than range-for so we can log the slot index.
	// Each slot owns: a host-visible staging buffer (persistently mapped so mpv
	// can render straight into it), one command buffer, and one fence.
	// -------------------------------------------------------------------------
	for (std::size_t i = 0; i < VideoEntry::k_staging_count; ++i) {
		VideoEntry::StagingSlot &slot = e.staging_slots[i]; // reference to slot i

		// -- Staging buffer: HOST_VISIBLE | HOST_COHERENT ----------------------
		VkBufferCreateInfo buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buf_info.size               = bytes; // one full RGBA frame
		buf_info.usage              = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // source for image copy
		vkCreateBuffer(m_vk->device, &buf_info, m_vk->allocator, &slot.buf);

		VkMemoryRequirements req {};
		vkGetBufferMemoryRequirements(m_vk->device, slot.buf, &req);

		VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.allocationSize       = req.size;
		alloc.memoryTypeIndex      = find_memory_type(m_vk->physical_device, req.memoryTypeBits,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | // CPU can write
					 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); // no flush required

		vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &slot.mem);
		vkBindBufferMemory(m_vk->device, slot.buf, slot.mem, 0);

		// Persistent map: mpv will render pixel data directly into this pointer
		// every frame, removing the intermediate pixel_buf → staging memcpy.
		vkMapMemory(m_vk->device, slot.mem, 0, bytes, 0, &slot.mapped);

		// -- Command buffer (one per slot, independent of the other) -----------
		VkCommandBufferAllocateInfo cmd_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cmd_alloc.commandPool                 = e.cmd_pool; // from the shared pool
		cmd_alloc.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd_alloc.commandBufferCount          = 1;
		vkAllocateCommandBuffers(m_vk->device, &cmd_alloc, &slot.cmd);

		// -- Fence (unsignaled; signaled by GPU after upload completes) --------
		VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; // no SIGNALED flag
		vkCreateFence(m_vk->device, &fence_info, m_vk->allocator, &slot.fence);

		slot.in_flight = false; // slot is free initially
	}

	// Start writing to slot 0 on the first upload.
	e.staging_write_idx = 0;

	// -------------------------------------------------------------------------
	// Initial layout transition: UNDEFINED → SHADER_READ_ONLY_OPTIMAL.
	//
	// ImGui will try to sample the texture before the first video frame arrives.
	// We must put the image in SHADER_READ_ONLY_OPTIMAL immediately so sampling
	// a not-yet-uploaded texture does not trigger a validation error.
	//
	// Slot 0's command buffer and fence are borrowed for this one-time submit
	// and then reset so the slot is ready for its first real upload.
	// -------------------------------------------------------------------------
	{
		VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // record once, submit once
		vkBeginCommandBuffer(e.staging_slots[0].cmd, &begin);

		VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED; // freshly allocated
		barrier.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.image                = e.image;
		barrier.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barrier.srcAccessMask        = 0; // no prior access to flush
		barrier.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT; // fragment shader will read

		vkCmdPipelineBarrier(e.staging_slots[0].cmd,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // earliest stage
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // wait before sampling
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		vkEndCommandBuffer(e.staging_slots[0].cmd);

		VkSubmitInfo submit       = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &e.staging_slots[0].cmd;

		// Submit with the per-slot fence and wait synchronously.
		// Resource creation is infrequent so a stall here is acceptable.
		m_vk->queue_submit(1, &submit, e.staging_slots[0].fence);
		vkWaitForFences(m_vk->device, 1, &e.staging_slots[0].fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_vk->device, 1, &e.staging_slots[0].fence); // ready for first upload
		vkResetCommandBuffer(e.staging_slots[0].cmd, 0); // ready to record again
	}

	return true;
}


// ============================================================================
// destroy_gpu_resources
// ============================================================================

/**
 * @brief Releases all Vulkan resources owned by a VideoEntry.
 *
 * Must only be called after vkDeviceWaitIdle() has confirmed no GPU work
 * references these resources — the callers in shutdown() and draw() guarantee
 * this.  Each handle is nulled after destruction so the function is safe to
 * call more than once (no double-free).
 *
 * Destroying the command pool implicitly frees every command buffer allocated
 * from it, so we null the per-slot cmd handles rather than calling
 * vkFreeCommandBuffers individually.
 *
 * @param e  The video entry whose Vulkan resources are to be freed.
 */
void VideoPlayer::destroy_gpu_resources(VideoEntry &e) {
	APP_DEBUG_LOG("[VideoPlayer] destroy_gpu_resources id={}", e.id);

	// Reset the upload-tracking state to a clean baseline.
	e.staging_write_idx = 0;
	e.last_upload_time  = {}; // epoch — no last upload

	// -- ImGui descriptor (unregisters the texture from the ImGui draw lists) -
	if (e.descriptor_set != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(e.descriptor_set); // deregister first
		e.descriptor_set = VK_NULL_HANDLE;
	}

	// -- Sampler ---------------------------------------------------------------
	if (e.sampler != VK_NULL_HANDLE) {
		vkDestroySampler(m_vk->device, e.sampler, m_vk->allocator);
		e.sampler = VK_NULL_HANDLE;
	}

	// -- Image view ------------------------------------------------------------
	if (e.image_view != VK_NULL_HANDLE) {
		vkDestroyImageView(m_vk->device, e.image_view, m_vk->allocator);
		e.image_view = VK_NULL_HANDLE;
	}

	// -- Device-local image ----------------------------------------------------
	if (e.image != VK_NULL_HANDLE) {
		vkDestroyImage(m_vk->device, e.image, m_vk->allocator);
		e.image = VK_NULL_HANDLE;
	}

	// -- Image backing memory --------------------------------------------------
	if (e.image_memory != VK_NULL_HANDLE) {
		vkFreeMemory(m_vk->device, e.image_memory, m_vk->allocator);
		e.image_memory = VK_NULL_HANDLE;
	}

	// -- Command pool (implicitly frees all command buffers from both slots) ---
	// Destroying the pool first avoids vkFreeCommandBuffers on each slot, which
	// would be redundant and potentially racy if a buffer was still allocated.
	if (e.cmd_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(m_vk->device, e.cmd_pool, m_vk->allocator);
		e.cmd_pool = VK_NULL_HANDLE; // pool is gone

		// Null out per-slot cmd handles — the backing objects no longer exist.
		for (auto &slot : e.staging_slots)
			slot.cmd = VK_NULL_HANDLE;
	}

	// -- Double-buffered staging slots (buffer / memory / fence per slot) -----
	for (auto &slot : e.staging_slots) {
		if (slot.buf != VK_NULL_HANDLE) {
			if (slot.mapped != nullptr) {
				// The Vulkan spec requires memory to be unmapped before freeing.
				vkUnmapMemory(m_vk->device, slot.mem); // release persistent CPU mapping
				slot.mapped = nullptr;
			}
			vkDestroyBuffer(m_vk->device, slot.buf, m_vk->allocator);
			slot.buf = VK_NULL_HANDLE;
		}
		if (slot.mem != VK_NULL_HANDLE) {
			vkFreeMemory(m_vk->device, slot.mem, m_vk->allocator);
			slot.mem = VK_NULL_HANDLE;
		}
		if (slot.fence != VK_NULL_HANDLE) {
			vkDestroyFence(m_vk->device, slot.fence, m_vk->allocator);
			slot.fence = VK_NULL_HANDLE;
		}
		slot.in_flight = false; // slot can be reinitialised
	}
}


// ============================================================================
// Hover thumbnail / seek thumbnail — delegated to child classes
// ============================================================================

VkDescriptorSet VideoPlayer::hover_thumbnail(std::string const &source) {
	static std::string s_last_hover_source;
	if (s_last_hover_source != source) {
		APP_DEBUG_LOG("[VideoPlayer] hover_thumbnail: {}", source);
		s_last_hover_source = source;
	}
	return m_hover->thumbnail(source);
}

bool VideoPlayer::save_hover_frame(std::filesystem::path const &path) {
	APP_DEBUG_LOG("[VideoPlayer] save_hover_frame: {}", path.string());
	return m_hover->save_frame(path);
}

VkDescriptorSet VideoPlayer::get_open_thumbnail(std::string const &source) const {
	static std::string s_last_hit_source;
	for (auto const &ep : m_entries) {
		if (ep->source == source && ep->descriptor_set != VK_NULL_HANDLE) {
			if (s_last_hit_source != source) {
				APP_DEBUG_LOG("[VideoPlayer] get_open_thumbnail: HIT {}", source);
				s_last_hit_source = source;
			}
			return ep->descriptor_set;
		}
		if (ep->playback_source == source && ep->descriptor_set != VK_NULL_HANDLE)
			return ep->descriptor_set;
	}
	return VK_NULL_HANDLE;
}

// ============================================================================
// Per-frame upload
// ============================================================================

// ============================================================================
// upload_frame
// ============================================================================

/**
 * @brief Renders the current mpv video frame to a staging buffer and issues a
 *        GPU copy into the device-local texture.
 *
 * Two staging slots are maintained in a ping-pong fashion.  Before rendering,
 * the function selects the preferred slot (e.staging_write_idx).  If that slot
 * is still in flight (GPU has not yet finished the previous copy) the other
 * slot is tried.  If both slots are in flight the frame is skipped and the
 * caller must retry on the next tick — this path is extremely rare in practice
 * because vkCmdCopyBufferToImage typically completes in under one frame period.
 *
 * mpv renders pixel data directly into the persistently-mapped host pointer
 * (slot.mapped), eliminating the intermediate pixel_buf → staging memcpy that
 * was present in the previous version.
 *
 * @param e  The video entry to upload a frame for.
 * @return true if a frame was successfully submitted to the GPU; false if no
 *         available slot was found or mpv had no new frame ready.
 */
bool VideoPlayer::upload_frame(VideoEntry &e) {
	// Guard against calling before GPU resources are created.
	if (e.video_w <= 0 || e.video_h <= 0)
		return false;

	// -------------------------------------------------------------------------
	// Step 1 — select a staging slot that is not currently being read by the GPU.
	//
	// Prefer the designated write slot; fall back to the other if it is busy.
	// -------------------------------------------------------------------------
	std::size_t              slot_idx = e.staging_write_idx; // preferred slot
	VideoEntry::StagingSlot *slot     = &e.staging_slots[slot_idx];

	if (slot->in_flight) {
		// Poll the GPU fence — VK_SUCCESS means the upload finished.
		VkResult const preferred_status = vkGetFenceStatus(m_vk->device, slot->fence);
		if (preferred_status == VK_SUCCESS) {
			// GPU is done — reset resources and reuse the slot.
			vkResetFences(m_vk->device, 1, &slot->fence);
			vkResetCommandBuffer(slot->cmd, 0);
			slot->in_flight = false;
		} else {
			// Preferred slot is still busy.  Try the alternate slot.
			std::size_t const        alt_idx = (slot_idx + 1) % VideoEntry::k_staging_count;
			VideoEntry::StagingSlot *alt     = &e.staging_slots[alt_idx];

			if (alt->in_flight) {
				// Both slots in flight — the GPU is behind the render thread.
				// Skip this frame; frame_dirty stays true and we retry next tick.
				VkResult const alt_status = vkGetFenceStatus(m_vk->device, alt->fence);
				if (alt_status != VK_SUCCESS)
					return false; // no free slot available

				// Alt slot just finished — reset it.
				vkResetFences(m_vk->device, 1, &alt->fence);
				vkResetCommandBuffer(alt->cmd, 0);
				alt->in_flight = false;
			}

			// Switch to the alternate slot for this upload.
			slot_idx = alt_idx;
			slot     = alt;
		}
	}

	// -------------------------------------------------------------------------
	// Step 2 — render the current video frame directly into the staging buffer.
	//
	// MPV_RENDER_PARAM_SW_POINTER is set to slot->mapped, which is the
	// persistently-mapped host pointer for this slot's VkDeviceMemory.  libmpv
	// writes decoded and colour-converted pixel data straight into GPU-visible
	// memory, removing the CPU→CPU memcpy from the previous version.
	//
	// std::array is used for size_arr to avoid a C-style array.  .data() yields
	// the int* pointer that the mpv C API expects.
	// -------------------------------------------------------------------------
	int const w = e.video_w;
	int const h = e.video_h;

	// Row stride in bytes: each pixel is 4 bytes (RGBA).
	std::size_t stride = static_cast<std::size_t>(w) * 4;

	// Dimensions array required by MPV_RENDER_PARAM_SW_SIZE {width, height}.
	std::array<int, 2> size_arr = {w, h};

	// Pixel format string required by MPV_RENDER_PARAM_SW_FORMAT.
	// constexpr so the string literal is baked in at compile time.
	static constexpr char const *k_fmt_rgba = "rgba";

	// Build the mpv render parameter list as a std::array.
	// The INVALID entry acts as a sentinel terminator for the mpv C API.
	std::array<mpv_render_param, 5> render_params = {{
		{MPV_RENDER_PARAM_SW_SIZE, size_arr.data()}, // frame dimensions (int*)
		{MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(k_fmt_rgba)}, // pixel format (char*)
		{MPV_RENDER_PARAM_SW_STRIDE, &stride}, // bytes per row (size_t*)
		{MPV_RENDER_PARAM_SW_POINTER, slot->mapped}, // destination — staging mem
		{MPV_RENDER_PARAM_INVALID, nullptr}, // list terminator
	}};

	// Ask libmpv to decode and blit the current frame.
	// A negative return means no new frame was available — bail without a GPU submit.
	if (mpv_render_context_render(e.render_ctx, render_params.data()) < 0)
		return false;

	// -------------------------------------------------------------------------
	// Step 3 — record a one-time command buffer: barrier → copy → barrier.
	// -------------------------------------------------------------------------
	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // record once, submit once
	vkBeginCommandBuffer(slot->cmd, &begin);

	// Barrier: SHADER_READ_ONLY → TRANSFER_DST so the copy can write to the image.
	VkImageMemoryBarrier b_to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b_to_dst.oldLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // was being sampled
	b_to_dst.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // will be written
	b_to_dst.image                = e.image;
	b_to_dst.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; // mip 0, layer 0
	b_to_dst.srcAccessMask        = VK_ACCESS_SHADER_READ_BIT; // flush shader reads
	b_to_dst.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT; // before transfer write

	vkCmdPipelineBarrier(slot->cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // after fragment reads
		VK_PIPELINE_STAGE_TRANSFER_BIT, // before the copy
		0, 0, nullptr, 0, nullptr, 1, &b_to_dst);

	// Copy from the staging buffer to the device-local image.
	VkBufferImageCopy region = {};
	region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; // mip 0, layer 0
	region.imageExtent       = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u}; // full frame extent

	vkCmdCopyBufferToImage(slot->cmd, slot->buf, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// Barrier: TRANSFER_DST → SHADER_READ_ONLY so the fragment shader can sample the new frame.
	VkImageMemoryBarrier b_to_read = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b_to_read.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // was being written
	b_to_read.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // will be sampled
	b_to_read.image                = e.image;
	b_to_read.subresourceRange     = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b_to_read.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT; // flush the copy
	b_to_read.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT; // before sampling

	vkCmdPipelineBarrier(slot->cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT, // after the copy finishes
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // before the shader reads
		0, 0, nullptr, 0, nullptr, 1, &b_to_read);

	vkEndCommandBuffer(slot->cmd);

	// -------------------------------------------------------------------------
	// Step 4 — submit and mark the slot in-flight.
	// -------------------------------------------------------------------------
	VkSubmitInfo submit       = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &slot->cmd;

	// The per-slot fence is signaled by the GPU when the copy completes.
	m_vk->queue_submit(1, &submit, slot->fence);
	slot->in_flight = true; // GPU is now reading this slot

	// Advance the write index to the other slot so the next frame has maximum
	// GPU time to finish before we come back around to this slot.
	e.staging_write_idx = (slot_idx + 1) % VideoEntry::k_staging_count;

	return true;
}


// ============================================================================
// poll_events
// ============================================================================

/**
 * @brief Drains the mpv event queue for one video entry and reacts to each event.
 *
 * Called every frame from update_frames() on the main thread.  Passes 0.0 as
 * the timeout so mpv_wait_event() returns immediately if no event is pending.
 *
 * Key changes vs the original:
 *  - On VIDEO_RECONFIG the video frame rate is queried via the "container-fps"
 *    property and stored in e.container_fps.  e.frame_interval is then computed
 *    as 90 % of one frame period so the upload gate is slightly faster than the
 *    nominal rate, preventing throttle drift from accumulating over time.
 *
 * @param e  The video entry to poll.
 */
void VideoPlayer::poll_events(VideoEntry &e) {
	while (true) {
		mpv_event *ev = mpv_wait_event(e.mpv, 0.0); // non-blocking poll
		if (!ev || ev->event_id == MPV_EVENT_NONE)
			break; // queue is empty

		// -- End-of-file / load failure ----------------------------------------
		if (ev->event_id == MPV_EVENT_END_FILE) {
			auto const *edata = static_cast<mpv_event_end_file const *>(ev->data);
			if (e.intentional_stop_pending) {
				// "stop" was issued by our code (e.g. switching active playback).
				// Suppress the load_failed path.
				e.intentional_stop_pending = false;
			} else if (edata->reason == MPV_END_FILE_REASON_EOF) {
				// Natural end of file — reset resume position so next open starts at 00:00.
				e.finished_at_eof = true;
			} else {
				APP_DEBUG_LOG("[VideoPlayer] load failed id={} reason={}", e.id, static_cast<int>(edata->reason));
				e.load_failed = true;
			}
		}

		// -- File loaded (seek restore + state reset) --------------------------
		if (ev->event_id == MPV_EVENT_FILE_LOADED && e.resume_seek_pending && e.resume_position_seconds > 0) {
			// Restore the saved playback position after a reload.
			double resume_position = static_cast<double>(e.resume_position_seconds);
			mpv_set_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &resume_position);
			e.resume_seek_pending = false;
			e.media_unloaded      = false;
			e.load_failed         = false;
			e.finished_at_eof     = false;
		} else if (ev->event_id == MPV_EVENT_FILE_LOADED) {
			// No resume seek — just clear the error flags.
			e.media_unloaded  = false;
			e.load_failed     = false;
			e.finished_at_eof = false;
		}

		// -- Video reconfiguration (dimensions or stream change) ---------------
		// -- Video reconfiguration (dimensions or stream change) ---------------
		if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
			APP_DEBUG_LOG("[VideoPlayer] event VIDEO_RECONFIG id={}", e.id);

			// Read the actual decoded frame dimensions from mpv.
			int64_t nw = 0, nh = 0;
			mpv_get_property(e.mpv, "dwidth", MPV_FORMAT_INT64, &nw);
			mpv_get_property(e.mpv, "dheight", MPV_FORMAT_INT64, &nh);

			if (nw > 0 && nh > 0 && (static_cast<int>(nw) != e.video_w || static_cast<int>(nh) != e.video_h)) {
				// Dimensions changed — tear down and rebuild GPU resources.
				vkDeviceWaitIdle(m_vk->device); // finish in-flight work first
				destroy_gpu_resources(e);
				e.video_w = static_cast<int>(nw);
				e.video_h = static_cast<int>(nh);
				create_gpu_resources(e);
				e.frame_dirty.store(true, std::memory_order_release); // request immediate upload
				APP_DEBUG_LOG("[VideoPlayer] reconfigured id={} {}x{}", e.id, e.video_w, e.video_h);
			}

			// -----------------------------------------------------------------
			// Update per-entry upload throttle from the container frame rate.
			// "container-fps" is declared in the file header and is available
			// immediately after VIDEO_RECONFIG.  Falls back to 30 fps for
			// audio-only streams or containers that omit the field.
			// The interval is 90 % of one frame period to prevent edge-slip
			// when the OS timer granularity is coarser than the frame period.
			// -----------------------------------------------------------------
			double fps = 0.0;
			if (mpv_get_property(e.mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps) < 0 || fps <= 1.0) {
				fps = 30.0; // safe fallback for audio or missing field
			}
			e.container_fps = fps;

			// 90 % of one frame period cast to steady_clock's native duration.
			double const interval_s = (1.0 / fps) * 0.9;
			e.frame_interval        = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(interval_s));

			APP_DEBUG_LOG("[VideoPlayer] fps={:.2f} frame_interval={}ms", fps,
				std::chrono::duration_cast<std::chrono::milliseconds>(e.frame_interval).count());

			// -----------------------------------------------------------------
			// OSD: show decoder mode + system hardware on every reconfiguration.
			//
			// "hwdec-current" returns the *active* hwdec backend chosen by mpv
			// for this stream ("nvdec", "vaapi", "vdpau", "dxva2", "no", …).
			// "no" means pure software decoding via libavcodec on the CPU.
			//
			// CPU and GPU names are read once from the Linux sysfs/procfs tree
			// and cached in function-local statics so the file I/O happens only
			// on the first reconfiguration event during the process lifetime.
			// -----------------------------------------------------------------

			// Lazy-initialised system info — read from the OS exactly once.
			static std::string const s_cpu_name = read_cpu_name_linux(); // e.g. "Intel(R) Core(TM) i7-7700 @ 3.60GHz"
			static std::string const s_gpu_name = read_gpu_name_linux(); // e.g. "NVIDIA GeForce RTX 3060"

			// Query the decoder mpv actually selected for this stream.
			// "no" = software (libavcodec on the CPU); anything else = hardware.
			char *hwdec_raw = nullptr;
			mpv_get_property(e.mpv, "hwdec-current", MPV_FORMAT_STRING, &hwdec_raw);

			// Wrap in a std::string so we own the copy; free the mpv allocation.
			std::string const hwdec_current = (hwdec_raw && hwdec_raw[0] != '\0') ? std::string(hwdec_raw) : "no";
			if (hwdec_raw)
				mpv_free(hwdec_raw); // always free mpv-allocated strings

			// Build the human-readable decoder label:
			//   "SW (libavcodec)"  — when mpv fell back to CPU decoding
			//   "HW (nvdec)"       — when a hardware backend is active
			bool const        hw_active = (hwdec_current != "no");
			std::string const dec_mode  = hw_active
				 ? ("HW (" + hwdec_current + ")") // e.g. "HW (nvdec)"
				 : "SW (libavcodec)"; // pure CPU path

			// Compose the final OSD string. std::format (C++23) produces a
			// single allocation and is evaluated at compile time for the format
			// string itself, keeping runtime overhead minimal.
			std::string const osd_msg
				= std::format("Decoder : {}\nCPU     : {}\nGPU     : {}", dec_mode, s_cpu_name, s_gpu_name);

			e.osd.show(osd_msg); // display for the configured duration

			APP_DEBUG_LOG("[VideoPlayer] OSD decoder info id={} hwdec-current='{}'", e.id, hwdec_current);
		}
	}
}


// ============================================================================
// update_frames
// ============================================================================

/**
 * @brief Uploads new video frames to the GPU for every open entry.
 *
 * Must be called exactly once per application frame on the main thread, before
 * draw().  Polls each entry's mpv event queue, and when the render-update
 * callback has flagged a new frame, attempts an upload respecting the
 * per-entry frame_interval throttle.
 *
 * Key change: the throttle interval now comes from e.frame_interval (derived
 * from the video's container frame rate in poll_events) instead of the global
 * 50 ms constant that hard-capped all content at 20 fps regardless of source.
 *
 * @note Asserts that it is called from the thread recorded in m_main_thread_id.
 */
void VideoPlayer::update_frames() {
	if (!m_initialized)
		return;

	// Thread safety: this function writes to Vulkan resources and to VideoEntry
	// fields.  It must always run on the same thread as draw().
	assert(std::this_thread::get_id() == m_main_thread_id
		&& "VideoPlayer::update_frames() must be called from the main thread");

	if (m_hover_initialized) {
		m_hover->tick_idle();          // keep hover preview alive
		update_hover_space_hold_speed(); // Space pause / hold-to-fast-forward
	}

	for (auto &ep : m_entries) {
		VideoEntry &e = *ep;
		if (!e.mpv)
			continue; // entry being torn down

		poll_events(e); // drain mpv event queue

		if (e.media_unloaded) {
			// Media was explicitly stopped (e.g. single-active enforcement).
			// Clear the dirty flag so we do not upload a spurious black frame
			// that mpv may emit after "stop".
			e.frame_dirty.store(false, std::memory_order_release);
		} else if (e.frame_dirty.exchange(false, std::memory_order_acq_rel) && e.descriptor_set != VK_NULL_HANDLE) {
			// The mpv render callback fired — a new decoded frame is available.
			auto const now = std::chrono::steady_clock::now();

			// Throttle uploads to the video's native frame rate (stored in
			// e.frame_interval, computed from container-fps in poll_events).
			// This replaces the former hard-coded 50 ms / 20 fps ceiling.
			if ((now - e.last_upload_time) >= e.frame_interval) {
				if (upload_frame(e)) {
					e.last_upload_time = now; // record successful upload time
				} else {
					// No staging slot was free or mpv had nothing to render.
					// Preserve the dirty flag so we retry on the very next tick.
					e.frame_dirty.store(true, std::memory_order_release);
				}
			} else {
				// Not enough time has elapsed since the last upload.
				// Preserve the dirty flag — do not silently discard the frame.
				e.frame_dirty.store(true, std::memory_order_release);
			}
		}

		// Flush any seek-preview thumbnail the background jthread produced.
		e.seek_preview.update();
	}

	enforce_single_active_playback(); // ensure at most one entry plays
}



// ============================================================================
// draw — called each frame inside an ImGui frame
// ============================================================================

void VideoPlayer::set_player_menu_callbacks(std::function<void()> on_open_image, std::function<void()> on_open_online,
	std::function<void(std::string const &, std::string const &)>            on_open_recent,
	std::function<std::vector<WindowStateToml::ImageHistoryEntry> const &()> history, HistoryPreview *preview,
	std::function<void(std::string const &)> on_fix_videos,
	std::function<bool(std::string const &)> is_startup_videos_fixed, std::function<bool()> on_get_app_fullscreen,
	std::function<void(bool)> on_set_app_fullscreen) {
	m_on_open_image           = std::move(on_open_image);
	m_on_open_online          = std::move(on_open_online);
	m_on_open_recent          = std::move(on_open_recent);
	m_history_provider        = std::move(history);
	m_history_preview         = preview;
	m_on_fix_videos           = std::move(on_fix_videos);
	m_is_startup_videos_fixed = std::move(is_startup_videos_fixed);
	m_on_get_app_fullscreen   = std::move(on_get_app_fullscreen);
	m_on_set_app_fullscreen   = std::move(on_set_app_fullscreen);
}

void VideoPlayer::set_context_menu(VideoContextMenu                         *ctx,
	std::function<WindowStateToml::ImageHistoryEntry *(std::string const &)> lookup,
	std::function<void(std::string const &)>                                 on_erase) {
	m_ctx_menu     = ctx;
	m_ctx_lookup   = std::move(lookup);
	m_ctx_on_erase = std::move(on_erase);
}

void VideoPlayer::set_downloader(VideoDownloader *d) { m_downloader = d; }

void VideoPlayer::restart_hover_preview() {
	APP_DEBUG_LOG("[VideoPlayer] restart_hover_preview");
	m_hover->stop_thread();
	m_hover->start_thread();
}

bool VideoPlayer::consume_hover_popup_reopen_request() { return m_hover->consume_popup_reopen_request(); }

bool VideoPlayer::is_hover_dwell_pending(std::string const &source) const {
	return m_hover->is_hover_dwell_pending(source);
}

void VideoPlayer::notify_hover(std::string const &source) { m_hover->notify_hover(source); }

bool VideoPlayer::is_hover_previewing() const { return m_hover && m_hover->is_previewing(); }

void VideoPlayer::seek_hover_preview(double seconds) {
	if (m_hover)
		m_hover->seek_relative(seconds);
}

void VideoPlayer::adjust_hover_volume(int delta) {
	if (m_hover)
		m_hover->adjust_volume(delta);
}

bool VideoPlayer::can_toggle_hwdec(std::string const &source) const {
	return std::any_of(m_entries.begin(), m_entries.end(), [&source](std::unique_ptr<VideoEntry> const &entry) {
		return entry->open && entry->source == source;
	});
}

bool VideoPlayer::is_hwdec_enabled(std::string const &source) const {
	for (auto const &entry : m_entries) {
		if (entry->open && entry->source == source)
			return entry->hwdec_enabled;
	}
	return false;
}

int VideoPlayer::current_position_seconds(std::string const &source) const {
	for (auto const &entry : m_entries) {
		if (!entry->open || entry->source != source)
			continue;

		return current_position_seconds(*entry);
	}

	return 0;
}

int VideoPlayer::persisted_position_seconds(std::string const &source) const {
	for (auto const &entry : m_entries) {
		if (!entry->open || entry->source != source)
			continue;

		return persisted_position_seconds(*entry);
	}

	return 0;
}

int VideoPlayer::current_position_seconds(VideoEntry const &entry) const {
	double time_pos = 0.0;
	if (mpv_get_property(entry.mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos) >= 0)
		return std::max(static_cast<int>(time_pos), 0);
	return entry.resume_position_seconds;
}

int VideoPlayer::persisted_position_seconds(VideoEntry const &entry) const {
	// Video finished naturally — reset resume timestamp to 00:00
	if (entry.finished_at_eof)
		return 0;

	double duration = 0.0;
	if (mpv_get_property(entry.mpv, "duration", MPV_FORMAT_DOUBLE, &duration) < 0)
		return current_position_seconds(entry);

	if (duration < static_cast<double>(m_resume_persist_min_duration_seconds))
		return 0;

	return current_position_seconds(entry);
}

void VideoPlayer::set_resume_persist_min_duration_seconds(int seconds) {
	m_resume_persist_min_duration_seconds = std::max(seconds, 0);
}

int VideoPlayer::resume_persist_min_duration_seconds() const { return m_resume_persist_min_duration_seconds; }

void VideoPlayer::toggle_hwdec(std::string const &source) {
	for (auto &entry : m_entries) {
		if (!entry->open || entry->source != source)
			continue;

		entry->hwdec_enabled           = !entry->hwdec_enabled;
		entry->resume_position_seconds = current_position_seconds(source);
		entry->resume_seek_pending     = entry->resume_position_seconds > 0;
		entry->reload_osd_message
			= entry->hwdec_enabled ? "HW decode requested (NVDEC)" : "SW decode requested (libavcodec)";
		entry->reload_requested = true;
		return;
	}
}

void VideoPlayer::set_all_hwdec(bool enabled) {
	m_global_hwdec_enabled = enabled;
	for (auto &entry : m_entries) {
		if (!entry->open || entry->hwdec_enabled == enabled)
			continue;
		entry->hwdec_enabled           = enabled;
		entry->resume_position_seconds = current_position_seconds(entry->source);
		entry->resume_seek_pending     = entry->resume_position_seconds > 0;
		// After the reload, poll_events/VIDEO_RECONFIG will show the full
		// decoder+system OSD automatically.  This brief message confirms the
		// toggle intent before the reload completes.
		entry->reload_osd_message      = entry->hwdec_enabled
				 ? "HW decode requested (NVDEC)" // will activate after reload
				 : "SW decode requested (libavcodec)"; // CPU path after reload
		entry->reload_requested        = true;
	}
}

void VideoPlayer::set_all_loop(bool enabled) {
	m_global_loop_enabled = enabled;
	for (auto &entry : m_entries) {
		if (!entry->open)
			continue;
		entry->loop = enabled;
		if (entry->mpv) {
			char const *val = enabled ? "inf" : "no";
			mpv_set_property_string(entry->mpv, "loop-file", val);
		}
	}
}

VideoEntry *VideoPlayer::active_or_first_entry() {
	// Prefer the active entry (the one currently playing / most-recently-active).
	for (auto &ep : m_entries) {
		if (ep->open && ep->mpv && ep->id == m_active_video_id)
			return ep.get();
	}
	// Fall back to the first open entry when no active id is tracked.
	for (auto &ep : m_entries) {
		if (ep->open && ep->mpv)
			return ep.get();
	}
	return nullptr;
}

void VideoPlayer::handle_media_key(SDL_Keycode key) {
	VideoEntry *target = active_or_first_entry();
	if (!target)
		return;

	switch (key) {
	case SDLK_MEDIA_PLAY:
	case SDLK_MEDIA_PLAY_PAUSE: {
		int paused = 1;
		mpv_get_property(target->mpv, "pause", MPV_FORMAT_FLAG, &paused);
		set_playback_state(target->id, paused != 0);
		break;
	}
	case SDLK_MEDIA_PAUSE: {
		set_playback_state(target->id, false);
		break;
	}
	case SDLK_MEDIA_STOP: {
		target->intentional_stop_pending = true;
		mpv_command_string(target->mpv, "stop");
		target->media_unloaded = true;
		target->frame_dirty.store(false, std::memory_order_release);
		if (m_active_video_id == target->id)
			m_active_video_id = -1;
		break;
	}
	case SDLK_MEDIA_NEXT_TRACK:
	case SDLK_MEDIA_FAST_FORWARD: {
		char const *cmd[] = {"seek", "10", "relative", nullptr};
		mpv_command_async(target->mpv, 0, cmd);
		break;
	}
	case SDLK_MEDIA_PREVIOUS_TRACK:
	case SDLK_MEDIA_REWIND: {
		char const *cmd[] = {"seek", "-10", "relative", nullptr};
		mpv_command_async(target->mpv, 0, cmd);
		break;
	}
	case SDLK_MUTE: {
		mpv_command_string(target->mpv, "cycle mute");
		break;
	}
	default:
		break;
	}
}

void VideoPlayer::seek_active_video(double seconds) {
	VideoEntry *target = active_or_first_entry();
	if (!target)
		return;

	std::string const amount = std::to_string(seconds);
	char const       *cmd[]  = {"seek", amount.c_str(), "relative", nullptr};
	mpv_command_async(target->mpv, 0, cmd);
}

void VideoPlayer::adjust_active_volume(int delta) {
	VideoEntry *target = active_or_first_entry();
	if (!target)
		return;

	int64_t volume = 100;
	mpv_get_property(target->mpv, "volume", MPV_FORMAT_INT64, &volume);
	volume = std::clamp<int64_t>(volume + delta, 0, 130);
	mpv_set_property(target->mpv, "volume", MPV_FORMAT_INT64, &volume);

	char buf[32];
	std::snprintf(buf, sizeof(buf), "Volume %d%%", static_cast<int>(volume));
	target->osd.show(buf);
}

void VideoPlayer::toggle_active_loop() {
	VideoEntry *target = active_or_first_entry();
	if (!target)
		return;

	target->loop = !target->loop;
	mpv_command_string(target->mpv, target->loop ? "set loop-file inf" : "set loop-file no");
	target->osd.show(target->loop ? "Loop On" : "Loop Off");
}

void VideoPlayer::update_space_hold_speed() {
	// Mirror the left-mouse hold FSM (VideoUiWindow) for the Space key:
	//   * quick tap     -> toggle play/pause
	//   * hold (>180ms) -> play at hold_speed_multiplier, restore on release
	// Read the live key state so the threshold is frame-accurate (independent of
	// the OS key-repeat delay). WantTextInput keeps Space usable in text fields.
	// The hover preview owns Space while its popup is up — "you control what you
	// are looking at". Bail so the active window does not also react.
	if (m_hover && m_hover->is_previewing())
		return;

	ImGuiIO const &io               = ImGui::GetIO();
	bool const     space_down       = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
	auto const     now              = std::chrono::steady_clock::now();
	constexpr auto k_hold_threshold = std::chrono::milliseconds(180);

	VideoEntry *target = active_or_first_entry();

	if (space_down && !m_space_active) {
		m_space_active       = true;
		m_space_accelerating = false;
		m_space_press_time   = now;
	}

	if (m_space_active && space_down && !m_space_accelerating && target
		&& (now - m_space_press_time) >= k_hold_threshold) {
		m_space_accelerating = true;
		m_space_saved_speed  = 1.0;
		mpv_get_property(target->mpv, "speed", MPV_FORMAT_DOUBLE, &m_space_saved_speed);
		auto boosted = static_cast<double>(VideoUiWindow::hold_speed_multiplier);
		mpv_set_property(target->mpv, "speed", MPV_FORMAT_DOUBLE, &boosted);
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.2gx", boosted);
		target->osd.show(buf);
	}

	if (m_space_active && !space_down) {
		if (m_space_accelerating) {
			if (target) {
				mpv_set_property(target->mpv, "speed", MPV_FORMAT_DOUBLE, &m_space_saved_speed);
				target->osd.show("1x");
			}
		} else if ((now - m_space_press_time) < k_hold_threshold && target) {
			int paused = 1;
			mpv_get_property(target->mpv, "pause", MPV_FORMAT_FLAG, &paused);
			set_playback_state(target->id, paused != 0);
		}
		m_space_active       = false;
		m_space_accelerating = false;
	}
}

void VideoPlayer::update_hover_space_hold_speed() {
	// Mirrors update_space_hold_speed() but targets the hover preview. Runs only
	// while the popup is actively playing; otherwise it resets and gets out of
	// the way so the active-window FSM keeps Space.
	if (!m_hover || !m_hover->is_previewing()) {
		m_hover_space_active       = false;
		m_hover_space_accelerating = false;
		return;
	}

	ImGuiIO const &io               = ImGui::GetIO();
	bool const     space_down       = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
	auto const     now              = std::chrono::steady_clock::now();
	constexpr auto k_hold_threshold = std::chrono::milliseconds(180);

	if (space_down && !m_hover_space_active) {
		m_hover_space_active       = true;
		m_hover_space_accelerating = false;
		m_hover_space_press_time   = now;
	}

	if (m_hover_space_active && space_down && !m_hover_space_accelerating
		&& (now - m_hover_space_press_time) >= k_hold_threshold) {
		m_hover_space_accelerating = true;
		m_hover_space_saved_speed  = m_hover->speed();
		m_hover->set_speed(static_cast<double>(VideoUiWindow::hold_speed_multiplier));
	}

	if (m_hover_space_active && !space_down) {
		if (m_hover_space_accelerating) {
			m_hover->set_speed(m_hover_space_saved_speed);
		} else if ((now - m_hover_space_press_time) < k_hold_threshold) {
			m_hover->toggle_pause();
		}
		m_hover_space_active       = false;
		m_hover_space_accelerating = false;
	}
}

void VideoPlayer::restart_all_threads() {
	restart_hover_preview();
	for (auto &entry : m_entries) {
		if (!entry->open)
			continue;
		entry->resume_position_seconds = current_position_seconds(entry->source);
		entry->resume_seek_pending     = entry->resume_position_seconds > 0;
		entry->reload_osd_message      = "Restarted";
		entry->reload_requested        = true;
	}
}

void VideoPlayer::sync_history_state(std::vector<WindowStateToml::ImageHistoryEntry> &history) const {
	for (auto &history_entry : history) {
		for (auto const &entry : m_entries) {
			if (!entry->open || entry->source != history_entry.source)
				continue;

			history_entry.hwdec_enabled           = entry->hwdec_enabled;
			history_entry.resume_position_seconds = persisted_position_seconds(*entry);
			break;
		}
	}
}

void VideoPlayer::notify_download_complete(std::string const &url, std::filesystem::path const &cached_path) {
	for (auto &ep : m_entries) {
		if (ep->source == url && ep->open) {
			APP_DEBUG_LOG("[VideoPlayer] download complete for id={} -> {}", ep->id, cached_path.string());
			ep->seek_preview.prepare(m_vk, m_seek_uploader.get(), cached_path.string());
			ep->playback_source = cached_path.string();
			break;
		}
	}
}

void VideoPlayer::replace_source_with_saved_file(std::string const &source, std::filesystem::path const &saved_path) {
	std::string const saved_source = saved_path.string();

	for (auto &ep : m_entries) {
		if (ep->source != source || !ep->open)
			continue;

		ep->source          = saved_source;
		ep->playback_source = saved_source;
		ep->title           = saved_path.filename().string();
		ep->kind            = (saved_path.extension().string() == ".gif") ? "gif" : "video";
		break;
	}
}

void VideoPlayer::draw() {
	assert(std::this_thread::get_id() == m_main_thread_id && "VideoPlayer::draw() must be called from the main thread");
	if (!m_vk)
		return;

	update_space_hold_speed();

	struct ReloadRequest {
			std::string source;
			std::string title;
			bool        hwdec_enabled;
			bool        loop;
			int         resume_position_seconds;
			std::string initial_osd_message;
	};

	std::vector<ReloadRequest> reload_queue;

	// First process entries closed in a previous frame. This avoids destroying
	// descriptor sets that may still be referenced by current-frame ImGui draw lists.
	for (auto &ep : m_entries) {
		if (!ep->open && ep->reload_requested) {
			reload_queue.push_back({ep->source, ep->title, ep->hwdec_enabled, ep->loop, ep->resume_position_seconds,
				ep->reload_osd_message});
			ep->reload_requested = false;
			ep->load_failed      = false;
			ep->reload_osd_message.clear();
		}
	}

	bool any_closed = std::any_of(m_entries.begin(), m_entries.end(), [](std::unique_ptr<VideoEntry> const &ep) {
		return !ep->open;
	});
	if (any_closed)
		vkDeviceWaitIdle(m_vk->device);

	std::erase_if(m_entries, [this](std::unique_ptr<VideoEntry> const &ep) {
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

	// Re-open entries that requested a reload after old resources were torn down.
	for (auto const &request : reload_queue) {
		std::string const &source               = request.source;
		std::string const &title                = request.title;
		bool               history_cache_exists = false;
		std::string        history_cached_path;
		std::error_code    cache_ec;
		if (m_ctx_lookup) {
			if (auto *entry = m_ctx_lookup(source)) {
				history_cached_path = entry->cached_path;
				if (!entry->cached_path.empty()) {
					history_cache_exists = std::filesystem::exists(std::filesystem::path(entry->cached_path), cache_ec);
				}
			}
		}

		std::filesystem::path reload_path;
		bool                  reopen_as_url = false;

		if (history_cache_exists) {
			reload_path = std::filesystem::path(history_cached_path);
		} else {
			std::error_code             source_ec;
			std::filesystem::path const source_path(source);
			if (std::filesystem::exists(source_path, source_ec)) {
				reload_path = source_path;
			} else {
				reopen_as_url = is_video_url(source);
			}
		}

		APP_DEBUG_LOG("[VideoPlayer] executing reload source={} reopen_as_url={} reload_path='{}' "
					  "history_cached_path='{}' cache_exists={} cache_ec={}",
			source, reopen_as_url, reload_path.string(), history_cached_path, history_cache_exists,
			cache_ec ? cache_ec.message() : "ok");

		if (reopen_as_url)
			add_from_url(source, title, request.hwdec_enabled, request.resume_position_seconds,
				request.initial_osd_message);
		else
			add_from_path(reload_path, title, source, request.hwdec_enabled, request.resume_position_seconds,
				request.initial_osd_message);

		// Restore the loop setting that was active before the reload.
		if (request.loop && !m_entries.empty() && m_entries.back()->mpv) {
			m_entries.back()->loop = true;
			mpv_set_property_string(m_entries.back()->mpv, "loop-file", "inf");
		}
	}

	for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
		if (!m_entries[i]->open)
			continue;
		draw_window(*m_entries[i], i);
		if (!m_entries[i]->open) {
			// Closed by window X button or context menu: force teardown path,
			// never reload this entry.
			m_entries[i]->reload_requested = false;
			m_entries[i]->reload_osd_message.clear();
			m_entries[i]->seek_preview.stop_thread();
		} else if (m_entries[i]->reload_requested) {
			m_entries[i]->open = false;
		}
	}
}

bool VideoPlayer::close_window(std::string const &source) {
	for (auto &ep : m_entries) {
		if (ep->open && ep->source == source) {
			ep->open             = false;
			ep->reload_requested = false;
			ep->reload_osd_message.clear();
			ep->seek_preview.stop_thread();
			return true;
		}
	}
	return false;
}

void VideoPlayer::close_all_windows() {
	for (auto &ep : m_entries) {
		ep->open             = false;
		ep->reload_requested = false;
		ep->reload_osd_message.clear();
		ep->seek_preview.stop_thread();
	}
	m_active_video_id = -1;
}

void VideoPlayer::set_playback_state(int video_id, bool playing) {
	if (playing) {
		m_active_video_id = video_id;
		// Exactly one active player: ensure selected is loaded and unload all others.
		for (auto &entry : m_entries) {
			if (!entry->open || !entry->mpv)
				continue;

			if (entry->id == video_id) {
				if (entry->media_unloaded) {
					char const *cmd[] = {"loadfile", entry->playback_source.c_str(), "replace", nullptr};
					mpv_command_async(entry->mpv, 0, cmd);
					if (entry->resume_position_seconds > 0)
						entry->resume_seek_pending = true;
					entry->media_unloaded = false;
					entry->load_failed    = false;
				}
				int pause_flag = 0;
				mpv_set_property(entry->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
				continue;
			}

			entry->resume_position_seconds  = current_position_seconds(*entry);
			entry->resume_seek_pending      = entry->resume_position_seconds > 0;
			entry->intentional_stop_pending = true;
			mpv_command_string(entry->mpv, "stop");
			entry->media_unloaded = true;
			entry->frame_dirty.store(false, std::memory_order_release);
		}
		return;
	}

	// Pause only the selected entry.
	for (auto &entry : m_entries) {
		if (!entry->open || !entry->mpv || entry->id != video_id)
			continue;

		int pause_flag = 1;
		mpv_set_property(entry->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
		entry->frame_dirty.store(false, std::memory_order_release);
		if (m_active_video_id == video_id)
			m_active_video_id = -1;
		return;
	}
}

void VideoPlayer::enforce_single_active_playback() {
	auto is_playing = [](VideoEntry &entry) -> bool {
		int paused = 1;
		if (mpv_get_property(entry.mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0)
			return false;
		return paused == 0;
	};

	int selected_id = -1;

	if (m_active_video_id >= 0) {
		for (auto &entry : m_entries) {
			if (!entry->open || !entry->mpv || entry->id != m_active_video_id)
				continue;
			if (is_playing(*entry))
				selected_id = entry->id;
			break;
		}
	}

	if (selected_id < 0) {
		for (auto &entry : m_entries) {
			if (!entry->open || !entry->mpv)
				continue;
			if (is_playing(*entry)) {
				selected_id = entry->id;
				break;
			}
		}
	}

	if (selected_id < 0) {
		m_active_video_id = -1;
		return;
	}

	m_active_video_id = selected_id;
	for (auto &entry : m_entries) {
		if (!entry->open || !entry->mpv || entry->id != selected_id)
			continue;

		if (entry->media_unloaded) {
			char const *cmd[] = {"loadfile", entry->playback_source.c_str(), "replace", nullptr};
			mpv_command_async(entry->mpv, 0, cmd);
			if (entry->resume_position_seconds > 0)
				entry->resume_seek_pending = true;
			entry->media_unloaded = false;
			entry->load_failed    = false;
		}
		break;
	}

	for (auto &entry : m_entries) {
		if (!entry->open || !entry->mpv || entry->id == selected_id)
			continue;

		int paused = 1;
		if (mpv_get_property(entry->mpv, "pause", MPV_FORMAT_FLAG, &paused) < 0)
			continue;

		if (!paused) {
			entry->resume_position_seconds  = current_position_seconds(*entry);
			entry->resume_seek_pending      = entry->resume_position_seconds > 0;
			entry->intentional_stop_pending = true;
			mpv_command_string(entry->mpv, "stop");
			entry->media_unloaded = true;
			entry->frame_dirty.store(false, std::memory_order_release);
		}
	}
}

bool VideoPlayer::draw_window(VideoEntry &e, int idx) {
	std::string               display_title = e.title;
	VideoDownloader::Progress dl {};
	if (m_downloader)
		dl = m_downloader->progress(e.source);

	// Title suffix while the source is still downloading: percentage when the
	// total size is known, otherwise just the bytes fetched so far.
	if (dl.active) {
		constexpr double k_mb = 1024.0 * 1024.0;
		char             buf[64];
		if (dl.percent >= 0.0 && dl.total > 0)
			std::snprintf(buf, sizeof(buf), " (%.0f%% · %.1f/%.1f MB↓)", dl.percent,
				static_cast<double>(dl.bytes) / k_mb, static_cast<double>(dl.total) / k_mb);
		else if (dl.percent >= 0.0)
			std::snprintf(buf, sizeof(buf), " (%.0f%%↓)", dl.percent);
		else if (dl.bytes > 0)
			std::snprintf(buf, sizeof(buf), " (%.1f MB↓)", static_cast<double>(dl.bytes) / k_mb);
		else
			std::snprintf(buf, sizeof(buf), " (starting↓)");
		display_title += buf;
	}
	VideoUiWindow::State state {
		e.mpv,
		display_title,
		e.title,
		e.source,
		e.playback_source,
		e.kind,
		e.id,
		e.open,
		e.fullscreen,
		e.loop,
		e.reload_requested,
		e.load_failed,
		e.video_w,
		e.video_h,
		dl.bytes,
		dl.percent,
		dl.total,
		dl.active,
		e.descriptor_set,
		e.osd,
		e.seek_preview,
		idx > 0,
		idx < static_cast<int>(m_entries.size()) - 1,
		e.show_stats,
		e.hide_ui,
		e.auto_hide_ui,
	};

	if (!e.resume_seek_pending) {
		double current_time_pos = 0.0;
		if (mpv_get_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &current_time_pos) >= 0)
			e.resume_position_seconds = std::max(static_cast<int>(current_time_pos), 0);
	}

	VideoUiWindow::Callbacks callbacks {
		m_ctx_menu,
		m_ctx_lookup,
		m_ctx_on_erase,
		[this]() { restart_hover_preview(); },
		[this]() { close_all_windows(); },
		m_on_open_image,
		m_on_open_online,
		m_on_open_recent,
		m_history_provider,
		m_history_preview,
		m_on_fix_videos,
		m_is_startup_videos_fixed,
		[this, &e, idx](int direction) {
			int const          target          = idx + direction;
			std::string const &source          = m_entries[target]->source;
			std::string const &playback_source = m_entries[target]->playback_source;
			char const        *command[]       = {"loadfile", playback_source.c_str(), "replace", nullptr};
			mpv_command_async(e.mpv, 0, command);
			e.source          = source;
			e.playback_source = playback_source;
			e.title           = m_entries[target]->title;
			e.kind            = m_entries[target]->kind;
		},
		[this](int video_id, bool playing) { set_playback_state(video_id, playing); },
		nullptr, // on_seek_preview_hover — no-op
		[this]() -> bool {
			// Guard: this callback may be unset if menus were wired while the
			// other player was active.  Fall back to "not fullscreen" instead of
			// invoking an empty std::function (which throws bad_function_call).
			return m_on_get_app_fullscreen ? m_on_get_app_fullscreen() : false;
		},
		[this](bool fullscreen) {
			if (m_on_set_app_fullscreen)
				m_on_set_app_fullscreen(fullscreen);
		},
	};

	m_ui_window->draw(state, callbacks);
	return e.open;
}

// ============================================================================
// Query
// ============================================================================

bool VideoPlayer::has_open_windows() const {
	return std::any_of(m_entries.begin(), m_entries.end(), [](std::unique_ptr<VideoEntry> const &ep) {
		return ep->open;
	});
}

std::vector<std::string> VideoPlayer::open_sources() const {
	std::vector<std::string> result;
	for (auto const &ep : m_entries) {
		if (ep->open)
			result.push_back(ep->source);
	}
	return result;
}
