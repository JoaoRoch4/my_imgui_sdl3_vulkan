#include "pch.hpp" // NOLINT

#include "file_browser_thumbnail_context.hpp"

#include "core/log/debug_log.hpp" // APP_DEBUG_LOG
#include "image_job_system.hpp"
#include "image_ops.hpp" // img::ops::encode_bc1 (mpv video fallback in bc1 mode)
#include "thumbnail_generator.hpp"
#include "vulkan_bc1_encoder.hpp" // GPU BC1 encoder singleton (setup/shutdown/pump)
#include "vulkan_texture.hpp"     // pulls vulkan_context.hpp (bc_textures_enabled)



#define THUMB_LOG(fmt, ...) APP_DEBUG_LOG_SPAM("[file_browser_thumbnail_context] " fmt, ##__VA_ARGS__)
#define  THUMB_DEBUG(fmt, ...)  THUMB_LOG


namespace {


// Compact pixel summary: dimensions, the brightest channel value anywhere, and the centre
// pixel. A real thumbnail has max≈255 and a non-zero centre; a black one reads max≈0.
std::string px_summary(img::ImageBuffer const &b) {
	if (b.data.empty())
		return "EMPTY";
	std::uint8_t mx = 0;
	for (std::uint8_t v : b.data)
		mx = std::max(mx, v);
	std::size_t const mid = (b.data.size() / 2) & ~static_cast<std::size_t>(3);
	return std::format("{}x{} ch{} max={} mid=({},{},{},{})", b.width, b.height, b.channels, static_cast<int>(mx),
		static_cast<int>(b.data[mid]), static_cast<int>(b.data[mid + 1]), static_cast<int>(b.data[mid + 2]),
		static_cast<int>(b.data[mid + 3]));
}

std::uint64_t fnv1a_hash(std::string const &s) {
	std::uint64_t h = 14695981039346656037ULL;
	for (unsigned char const c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

std::string lower_ext(std::filesystem::path const &p) {
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return ext;
}

bool is_image_ext(std::filesystem::path const &p) {
	std::string const e = lower_ext(p);
	// .gif is decoded as a still (first frame) via decode_file/libav, which ignores
	// seek failures — unlike ffmpegthumbnailer's seek, which fails on most GIFs.
	return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".gif" || e == ".avif"
		|| e == ".heic" || e == ".heif";
}

bool is_video_ext(std::filesystem::path const &p) {
	std::string const e = lower_ext(p);
	return e == ".mp4" || e == ".mkv" || e == ".webm" || e == ".mov" || e == ".avi" || e == ".m4v" || e == ".wmv"
		|| e == ".flv" || e == ".ts" || e == ".mpg" || e == ".mpeg" || e == ".m2ts" || e == ".3gp" || e == ".ogv";
}

} // namespace

FileBrowserThumbnailContext::FileBrowserThumbnailContext() = default;
FileBrowserThumbnailContext::~FileBrowserThumbnailContext() { shutdown(); }

bool FileBrowserThumbnailContext::is_thumbnailable(std::filesystem::path const &path) {
	return is_image_ext(path) || is_video_ext(path);
}

FileBrowserThumbnailContext::Classification FileBrowserThumbnailContext::classify(std::filesystem::path const &path) {
	// Lower the extension once and test both categories, instead of is_image_ext +
	// is_video_ext each re-allocating the lowered extension (the old per-frame cost).
	std::string const e = lower_ext(path);
	Classification    c;
	if (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".gif" || e == ".avif" || e == ".heic"
		|| e == ".heif") {
		c.thumbnailable = true;
	} else if (e == ".mp4" || e == ".mkv" || e == ".webm" || e == ".mov" || e == ".avi" || e == ".m4v" || e == ".wmv"
		|| e == ".flv" || e == ".ts" || e == ".mpg" || e == ".mpeg" || e == ".m2ts" || e == ".3gp" || e == ".ogv"
	 ) {
		c.thumbnailable = true;
		c.is_video      = true;
	}
	return c;
}

std::string FileBrowserThumbnailContext::make_key(std::filesystem::path const &path) {
	return path.lexically_normal().string();
}

// Add to: file_browser_thumbnail_context.hpp
[[nodiscard]] bool
FileBrowserThumbnailContext::GetLoadedTextureDimensions(std::string const &key, int &outW, int &outH) const noexcept {
	if (auto const it = m_entries.find(key); it != m_entries.end()) {
		if (it->second.texture) {
			outW = it->second.texture->width;
			outH = it->second.texture->height;
			return true;
		}
	}
	return false;
}

void FileBrowserThumbnailContext::setup(vulkan_context *vk, std::filesystem::path thumb_dir,
	std::string thumbnail_format, std::string image_tier, std::string video_tier) {
	m_vk        = vk;
	m_thumb_dir = std::move(thumb_dir);
	m_setup     = true;
	std::error_code ec;
	std::filesystem::create_directories(m_thumb_dir, ec);
	THUMB_LOG("setup thumb_dir={} vk={}", m_thumb_dir.string(), static_cast<void const *>(vk));

	// VIDEO quality tier -> BC1 letterbox resolution. Higher tier = sharper (more VRAM/disk).
	if (video_tier == "original")
		m_thumb_w = 1920, m_thumb_h = 1080;
	else if (video_tier == "high")
		m_thumb_w = 854, m_thumb_h = 480;
	else if (video_tier == "low")
		m_thumb_w = 426, m_thumb_h = 240;
	else // "medium" (default)
		m_thumb_w = 1280, m_thumb_h = 720;

	// Resolve the storage backend. "bc1" enables the hybrid policy: VIDEO thumbs are
	// BC1 block-compressed into a shared blob, while IMAGE thumbs decode to lossless RGBA
	// with no disk cache (BC1 bands too hard on flat-shaded art; see Entry::use_bc1). It
	// requires device BC support; otherwise everything falls back to the portable PNG path.
	const bool want_bc1 = (thumbnail_format == "bc1");
	m_backend = (want_bc1 && vk && vk->bc_textures_enabled) ? Backend::Bc1 : Backend::Png;
	if (m_backend == Backend::Bc1) {
		constexpr std::uint64_t k_cap_bytes = 256ull * 1024ull * 1024ull; // ~256 MB live cap
		// Per-resolution blob dir so changing the video tier never reads stale-sized blocks
		// (old-resolution blobs simply become unused rather than corrupting an upload).
		std::string const blob_dir = "bc1_" + std::to_string(m_thumb_w) + "x" + std::to_string(m_thumb_h);
		m_blob.open(m_thumb_dir / blob_dir, k_cap_bytes);
		APP_DEBUG_LOG("[thumbnail_context] backend=bc1 hybrid; video tier={} ({}x{})", video_tier, m_thumb_w,
			m_thumb_h);
	} else {
		APP_DEBUG_LOG("[thumbnail_context] backend=png (format='{}', bc_supported={})", thumbnail_format,
			vk ? vk->bc_textures_enabled : false);
	}

	// Auto-size the image-decode resolution cap from available VRAM: more memory -> allow
	// larger native-resolution image thumbnails before the long-edge clamp kicks in. The
	// query is best-effort (VK_EXT_memory_budget live budget, else the device-local heap
	// size); a 0 result keeps the default. Tiers, not a formula, so the value is predictable.
	if (vk) {
		VkDeviceSize const avail = vk->vram_reserve_bytes;
		double const       gib   = static_cast<double>(avail) / (1024.0 * 1024.0 * 1024.0);
		if (avail == 0)
			m_image_max_edge = 2048;
		else if (gib < 1.5)
			m_image_max_edge = 1024;
		else if (gib < 3.0)
			m_image_max_edge = 1536;
		else if (gib < 6.0)
			m_image_max_edge = 2048;
		else if (gib < 12.0)
			m_image_max_edge = 3072;
		else
			m_image_max_edge = 2048;
		// IMAGE quality tier caps the VRAM-auto value (images stay lossless RGBA — compressing
		// them is the banding we deliberately avoid; resolution is the quality/VRAM lever).
		if (image_tier == "high")
			m_image_max_edge = std::min<uint64_t>(m_image_max_edge, 2048);
		else if (image_tier == "medium")
			m_image_max_edge = std::min<uint64_t>(m_image_max_edge, 1280);
		else if (image_tier == "low")
			m_image_max_edge = std::min<uint64_t>(m_image_max_edge, 768);
		// "original" keeps the full VRAM-auto cap.
		APP_DEBUG_LOG("[thumbnail_context] image tier={} -> max_edge={} (VRAM ~{:.2f} GiB, budget_ext={})",
			image_tier, m_image_max_edge, gib, vk->memory_budget_enabled);
	}

	// Start the Reproducer's nvdec-copy mpv worker pool (sized to match our thumbnails). It
	// stays idle unless a video generate fails and poll_entry routes a re-derive to it.
	m_reproducer.start(
		[this](std::string const &key, std::vector<std::uint8_t> rgba, bool ok) {
			on_video_done(key, std::move(rgba), ok);
		},
		m_thumb_w, m_thumb_h);

	// GPU BC1 encoder: setup once (best-effort). If it fails — missing extension,
	// out-of-memory, etc. — is_ready() stays false and the worker path silently
	// falls back to CPU encode. We only need it when the storage backend is BC1;
	// firing it up under PNG would waste a few descriptor sets.
	if (vk && m_backend == Backend::Bc1) {
		if (!VulkanBc1Encoder::instance().setup(*vk))
			APP_DEBUG_LOG("[thumbnail_context] bc1 GPU encoder unavailable — CPU fallback only");
	}
}

void FileBrowserThumbnailContext::shutdown() {
	if (!m_setup)
		return;
	m_setup = false;

	m_reproducer.shutdown(); // stop + join the mpv (nvdec-copy) worker pool
	img::ImageJobSystem::instance().clear_pending(); // drop queued image jobs
	if (m_vk)
		VulkanBc1Encoder::instance().shutdown(*m_vk); // tear down GPU encoder (no-op if not setup)

	if (m_backend == Backend::Bc1)
		m_blob.close(); // flush the blob index to disk

	if (m_vk) {
		for (auto &[k, e] : m_entries)
			if (e.texture)
				e.texture->unload(*m_vk);
		for (auto &r : m_retire)
			if (r.texture)
				r.texture->unload(*m_vk);
	}
	m_entries.clear();
	m_retire.clear();
	{
		std::lock_guard lk(m_video_mutex);
		m_video_results.clear();
	}
	m_vk = nullptr;
}

std::string FileBrowserThumbnailContext::key_for(std::filesystem::path const &path) const {
	// currentDirectory_ is already absolute, so a lexical normalize is enough — no
	// weakly_canonical() syscall on the render thread (the old cache's per-frame stall).
	return make_key(path);
}

std::filesystem::path FileBrowserThumbnailContext::png_for(std::string const &key) const {
	char hex[17];
	std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(fnv1a_hash(key)));
	return m_thumb_dir / (std::string(hex) + ".png");
}

void FileBrowserThumbnailContext::submit_image(std::filesystem::path const &file, Entry &e, bool decode_as_video) {
	// One composite pool job that dispatches to the two thumbnail engines:
	//   cache HIT  -> ThumbnailReproducer::reload  (stb decode of the already-sized PNG);
	//   cache MISS -> ThumbnailGenerator::generate (video: ffmpegthumbnailer, image/gif: stb,
	//                 then letterbox -> persisted PNG).
	// Both entry points are static and thread-safe, so the job captures no `this`. A fresh
	// VIDEO generate that fails is retried on the Reproducer's mpv (nvdec-copy) worker by
	// poll_entry (gated by Entry::source_is_video / tried_mpv).

	// BC1 route (video only): no PNG. The pool job decodes + letterboxes + encodes to BC1
	// blocks; the blob (checked in get()) is the persistence layer, so no reload branch here.
	// Copy the (member) video resolution into locals so the worker lambdas capture by value
	// and never touch `this` off-thread.
	Uint64 const tw = m_thumb_w;
	Uint64 const th = m_thumb_h;
	if (e.use_bc1) {
		e.bc1_future = img::ImageJobSystem::instance().submit(
			[file, decode_as_video, tw, th]() -> Bc1Result {
				return ThumbnailGenerator::generate_bc1(file, decode_as_video, tw, th);
			},
			img::Priority::Normal);
		e.state = State::Generating;
		return;
	}

	// RGBA route, two sub-cases distinguished by png_path:
	//   * IMAGE (png_path empty, no cache) -> decode at NATIVE resolution, no letterbox, no
	//     persist. Full quality, identical to the hover preview.
	//   * VIDEO no-BC fallback (png_path set) -> reload the cached PNG, else letterbox-generate
	//     into m_thumb_w x m_thumb_h and persist, exactly as before.
	bool const full_image = e.png_path.empty();
	Uint64 const  max_edge   = m_image_max_edge;
	e.img_future          = img::ImageJobSystem::instance().submit(
        [file, out = e.png_path, decode_as_video, full_image, max_edge, tw, th]() -> ImgResult {
            if (full_image)
                return ThumbnailGenerator::generate_image_full(file, max_edge);
            std::error_code ec;
            if (!out.empty() && std::filesystem::exists(out, ec) && !ec)
                return ThumbnailReproducer::reload(out);
            return ThumbnailGenerator::generate(file, decode_as_video, out, tw, th);
        },
        img::Priority::Normal);
	e.state = State::Generating;
}

void FileBrowserThumbnailContext::on_video_done(std::string const &key, std::vector<std::uint8_t> rgba, bool ok) {
	THUMB_LOG("video_done key={} ok={} bytes={}", key, ok, rgba.size());
	ImgResult res = std::unexpected(img::ImageError::DecodeFailed);
	if (ok && rgba.size() == static_cast<std::size_t>(m_thumb_w) * m_thumb_h * 4) {
		img::ImageBuffer b;
		b.width    = m_thumb_w;
		b.height   = m_thumb_h;
		b.channels = 4;
		b.data     = std::move(rgba);
		res        = std::move(b);
	}
	std::lock_guard lk(m_video_mutex);
	m_video_results.emplace_back(key, std::move(res));
}

void FileBrowserThumbnailContext::enforce_texture_cap() {
	int live = 0;
	for (auto &[k, e] : m_entries)
		if (e.texture)
			++live;
	if (live <= k_max_live_textures)
		return;

	// Retire the least-recently-used live textures down to the cap. Visible thumbnails
	// were touched this/last frame so their last_used is newest -> never evicted here.
	std::vector<decltype(m_entries)::iterator> live_its;
	live_its.reserve(static_cast<std::size_t>(live));
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
		if (it->second.texture)
			live_its.push_back(it);
	std::sort(live_its.begin(), live_its.end(), [](auto const &a, auto const &b) {
		return a->second.last_used < b->second.last_used;
	});

	int const to_evict = live - k_max_live_textures;
	for (int i = 0; i < to_evict; ++i) {
		Entry &e = live_its[static_cast<std::size_t>(i)]->second;
		m_retire.push_back({std::move(e.texture), k_retire_frames});
		e.texture.reset(); // moved-from already, but make the null explicit
		// Keep the entry (and its png_path) alive in Cached state: the expensive source
		// generation already ran and persisted to disk, so re-display is a cheap PNG
		// decode. Erasing here is what made big folders re-run mpv/decode forever.
		e.state       = State::Cached;
		e.have_pixels = false;
		e.pixels      = img::ImageBuffer {};
	}
}

void FileBrowserThumbnailContext::begin_frame() {
	if (!m_setup)
		return;
	++m_frame;
	m_uploads_this_frame = 0;

	// GPU BC1 encoder: drain completed dispatches from prior frames and start a
	// new batch of any pending submissions. Cheap when the encoder is idle.
	if (m_vk)
		VulkanBc1Encoder::instance().pump_once(*m_vk);

	// Tick the retire queue; free textures that have aged out (GPU no longer sampling).
	for (auto it = m_retire.begin(); it != m_retire.end();) {
		if (--it->frames_left <= 0) {
			if (it->texture && m_vk)
				it->texture->unload(*m_vk);
			it = m_retire.erase(it);
		} else {
			++it;
		}
	}

	// Drain results produced by the video worker thread.
	std::vector<std::pair<std::string, ImgResult>> results;
	{
		std::lock_guard lk(m_video_mutex);
		results.swap(m_video_results);
	}
	for (auto &[key, res] : results) {
		auto it = m_entries.find(key);
		if (it == m_entries.end())
			continue;
		if (res && res->valid()) {
			if (it->second.use_bc1) {
				// mpv fallback delivered RGBA; encode to BC1 here (rare path) so the
				// upload + blob store go through the same upload_bc1 chokepoint.
				if (auto enc = img::ops::encode_bc1(*res); enc && !enc->empty()) {
					it->second.bc1_blocks = std::move(*enc);
					it->second.have_bc1   = true;
					it->second.from_blob  = false;
					it->second.state      = State::PixelsReady;
				} else {
					it->second.state = State::Failed;
				}
			} else {
				it->second.pixels      = std::move(*res);
				it->second.have_pixels = true;
				it->second.state       = State::PixelsReady;
			}
		} else {
			it->second.state = State::Failed;
		}
	}

	enforce_texture_cap(); // bound live textures (LRU) so huge folders can't exhaust the GPU
}

ImTextureID FileBrowserThumbnailContext::poll_entry(Entry &e) {
	// Image jobs deliver via a future; poll without blocking. Video jobs deliver via
	// begin_frame draining the worker's results into PixelsReady.
	if (e.state == State::Generating && !e.is_video && e.img_future.valid()) {
		if (e.img_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			ImgResult res = e.img_future.get();
			if (res && res->valid()) {
				e.pixels      = std::move(*res);
				e.have_pixels = true;
				e.state       = State::PixelsReady;
				THUMB_LOG("img decoded -> PixelsReady [{}]", px_summary(e.pixels));
			} else {
				e.state = State::Failed;
				THUMB_LOG("img decode FAILED");
			}
		}
	}

	// BC1 backend: the pool job delivers blocks (not RGBA) via bc1_future.
	if (e.state == State::Generating && !e.is_video && e.bc1_future.valid()) {
		if (e.bc1_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			Bc1Result res = e.bc1_future.get();
			if (res && !res->empty()) {
				e.bc1_blocks = std::move(*res);
				e.have_bc1   = true;
				e.from_blob  = false;
				e.state      = State::PixelsReady;
			} else {
				e.state = State::Failed;
			}
		}
	}

	// Upload at most k_max_uploads_per_frame textures per frame so a fresh grid of
	// ready thumbnails can't stall a single frame.
	if (e.state == State::PixelsReady && e.use_bc1 && e.have_bc1
		&& m_uploads_this_frame < k_max_uploads_per_frame) {
		auto tex = std::make_unique<VulkanTexture>();
		if (tex->upload_bc1(e.bc1_blocks, m_thumb_w, m_thumb_h, *m_vk)) {
			// Persist to the blob on first upload (a blob hit is already stored).
			if (!e.from_blob)
				m_blob.store(e.blob_key, e.bc1_blocks, m_thumb_w, m_thumb_h);
			e.texture = std::move(tex);
			e.state   = State::Ready;
			++m_uploads_this_frame;
		} else {
			e.state = State::Failed;
		}
		e.bc1_blocks.clear();
		e.bc1_blocks.shrink_to_fit();
		e.have_bc1 = false;
	} else if (e.state == State::PixelsReady && !e.use_bc1 && e.have_pixels
		&& m_uploads_this_frame < k_max_uploads_per_frame) {
		THUMB_LOG("upload pixels [{}]", px_summary(e.pixels));
		auto tex = std::make_unique<VulkanTexture>();
		if (tex->upload(e.pixels, *m_vk)) {
			e.texture = std::move(tex);
			e.state   = State::Ready;
			++m_uploads_this_frame;
			THUMB_LOG("upload OK  imgui_id={:#x}", static_cast<std::uint64_t>(e.texture->imgui_id()));
		} else {
			e.state = State::Failed;
			THUMB_LOG("upload FAIL");
		}
		e.pixels      = img::ImageBuffer {}; // release CPU pixels either way
		e.have_pixels = false;
	}

	if (e.state == State::Ready && e.texture)
		return e.texture->imgui_id();
	return 0;
}

ImTextureID FileBrowserThumbnailContext::get(std::filesystem::path const &path) {
	if (!m_setup || !is_thumbnailable(path))
		return 0; // skip non-image/-video files (replaces app_coordinator's is_thumb_path)
	return get(key_for(path), path.parent_path(), path.filename(), is_video_ext(path));
}

ImTextureID FileBrowserThumbnailContext::get(std::string_view key, std::filesystem::path const &dir,
	std::filesystem::path const &name,
	bool                         is_video) { // super hot — render thread, per file, per frame
	if (!m_setup)
		return 0;

	// Heterogeneous find: the common case (entry already exists) costs one hash of the
	// precomputed key with NO temporary std::string. Only a genuine cache miss — the
	// first time a file is seen — pays for key materialization and the joined path.
	auto it = m_entries.find(key);
	if (it == m_entries.end())
		it = m_entries.try_emplace(std::string(key)).first;
	Entry &e    = it->second;
	e.last_used = m_frame; // mark touched this frame (LRU)

	// Two transitions kick off a job:
	//   Queued -> first ever sighting: generate from source (mpv for video, decode for
	//             image) UNLESS a PNG from a prior pass/session is already on disk.
	//   Cached -> texture was evicted to honour the VRAM cap, but the PNG persists: reload
	//             it. This is always a cheap PNG decode — NEVER a source regeneration, so
	//             a big folder converges to "fully generated" instead of thrashing mpv.
	if (e.state == State::Queued || e.state == State::Cached) {
		std::filesystem::path const file = dir / name; // joined only on a (re)submit

		// Per-entry storage route: BC1 (blob) only for VIDEO when the device supports BC;
		// images always take the lossless RGBA route (no banding, no disk cache).
		e.use_bc1 = (m_backend == Backend::Bc1) && is_video;

		if (e.use_bc1) {
			// Blob backend: a hit (incl. Cached re-display) is a cheap blob read -> upload_bc1,
			// NEVER a source regeneration. A miss decodes + encodes on the pool (submit_image).
			if (e.blob_key == 0)
				e.blob_key = ThumbnailBlobCache::make_key(file);
			if (auto stored = m_blob.lookup(e.blob_key)) {
				e.bc1_blocks = std::move(stored->blocks);
				e.have_bc1   = true;
				e.from_blob  = true;
				e.state      = State::PixelsReady;
			} else {
				e.is_video        = false;
				e.source_is_video = is_video; // fresh video may fall back to mpv on decode failure
				e.from_blob       = false;
				submit_image(file, e, is_video);
			}
		} else {
			// RGBA route. Disk PNG cache is reserved for VIDEO in the no-BC fallback (decode is
			// expensive); IMAGES are never cached — png_path stays empty so submit_image decodes
			// from source and generate() skips the PNG write.
			bool const cache_to_disk = is_video; // images: no thumbnail cache
			bool       have_png      = false;
			if (cache_to_disk) {
				std::string const &stored_key = it->first; // stable key string owned by the map
				if (e.png_path.empty())
					e.png_path = png_for(stored_key);
				std::error_code ec;
				have_png = std::filesystem::exists(e.png_path, ec) && !ec;
			} else {
				e.png_path.clear();
			}

			THUMB_LOG("{} key={} video={} have_png={} png={}", e.state == State::Cached ? "RELOAD" : "NEW",
				it->first, is_video, have_png, e.png_path.filename().string());
			// Both source kinds decode on the ImageJobSystem pool and deliver via e.img_future, so
			// e.is_video stays false (poll_entry only polls the future when is_video == false, else
			// the decoded pixels are never picked up and the thumb stays blank). e.source_is_video
			// records the REAL source kind so a failed video generate can fall back to mpv below.
			e.is_video                   = false;
			bool const generate_as_video = is_video && !have_png;
			e.source_is_video            = generate_as_video;
			submit_image(file, e, generate_as_video);
		}
	}

	ImTextureID const id = poll_entry(e);

	// mpv re-derive fallback: a fresh video whose Generator (ffmpegthumbnailer) decode failed
	// is retried EXACTLY ONCE on the Reproducer's nvdec-copy worker. Its result returns
	// asynchronously via on_video_done -> m_video_results -> begin_frame, which sets the entry
	// to PixelsReady/Failed for this key. tried_mpv guards against an infinite re-derive loop.
	if (e.state == State::Failed && e.source_is_video && !e.tried_mpv) {
		e.tried_mpv = true;
		m_reproducer.submit(std::string(key), dir / name, e.png_path);
		e.state = State::Generating; // await the async mpv result
	}

	return id;
}

void FileBrowserThumbnailContext::evict(std::filesystem::path const &path) {
	if (!m_setup)
		return;
	auto it = m_entries.find(key_for(path));
	if (it == m_entries.end())
		return;
	if (it->second.texture)
		m_retire.push_back({std::move(it->second.texture), k_retire_frames});
	if (it->second.use_bc1) {
		m_blob.evict(it->second.blob_key ? it->second.blob_key : ThumbnailBlobCache::make_key(path));
	} else if (!it->second.png_path.empty()) {
		std::error_code ec;
		std::filesystem::remove(it->second.png_path, ec);
	}
	// Images keep neither a blob nor a PNG (no cache), so there is nothing else to remove.
	m_entries.erase(it);
}

void FileBrowserThumbnailContext::clear() {
	if (!m_setup)
		return;
	m_reproducer.clear_pending();
	img::ImageJobSystem::instance().clear_pending();
	std::error_code ec;
	for (auto &[k, e] : m_entries) {
		if (e.texture)
			m_retire.push_back({std::move(e.texture), k_retire_frames});
		// png_path is set only for cached entries (no-BC video fallback); empty for images.
		if (!e.png_path.empty())
			std::filesystem::remove(e.png_path, ec);
	}
	m_entries.clear();

	// BC1 video blob: wipe the whole blob + index in one shot (no-op when never opened).
	if (m_backend == Backend::Bc1)
		m_blob.clear();
}

void FileBrowserThumbnailContext::release_textures() {
	if (!m_setup)
		return;
	// Directory switch: free the previous folder's GPU textures so VRAM doesn't accumulate
	// across navigation. Full-res RGBA image thumbnails are large, so a folder of them left
	// live until the LRU cap could pin hundreds of MB. Unlike clear(), this keeps the on-disk
	// caches (video BC1 blob / PNG) — only the in-memory entries + live textures are dropped,
	// so re-entering a folder re-decodes images (cheap) and reloads video thumbs from the blob.
	m_reproducer.clear_pending();                    // abandon in-flight decodes for the old dir
	img::ImageJobSystem::instance().clear_pending();
	for (auto &[k, e] : m_entries)
		if (e.texture)
			m_retire.push_back({std::move(e.texture), k_retire_frames}); // free after GPU drains
	m_entries.clear();
}
