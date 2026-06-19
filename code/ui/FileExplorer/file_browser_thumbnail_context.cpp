#include "pch.hpp" // NOLINT

#include "file_browser_thumbnail_context.hpp"

#include "image_job_system.hpp"
#include "image_ops.hpp"
#include "vulkan_texture.hpp"



// Thumbnail-pipeline debug logging. Set to 0 to silence. Logs only on state transitions
// (first sighting, decode result, upload, video done) — NOT per-frame — so it is safe to
// // leave on while diagnosing the black-thumbnail issue.
// #define THUMB_DEBUG 1
// #if THUMB_DEBUG
// #define THUMB_LOG(fmt, ...) std::println("[Thumb] " fmt __VA_OPT__(, ) __VA_ARGS__)
// #else
// #define THUMB_LOG(fmt, ...) ((void) 0)
// #endif
#define THUMB_LOG(fmt, ...) ((void)0)



namespace {

#if THUMB_DEBUG
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
#endif

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
	return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".gif";
}

bool is_video_ext(std::filesystem::path const &p) {
	std::string const e = lower_ext(p);
	return e == ".mp4" || e == ".mkv" || e == ".webm" || e == ".mov" || e == ".avi" || e == ".m4v" || e == ".wmv"
		|| e == ".flv" || e == ".ts" || e == ".mpg" || e == ".mpeg" || e == ".m2ts" || e == ".3gp" || e == ".ogv";
}

// Scale `src` to fit inside WxH preserving its aspect ratio, then centre it on a fully
// transparent WxH RGBA canvas (letterbox). Keeps the thumbnail's fixed dimensions — so
// the cache/upload/display pipeline is unchanged — while the image is no longer stretched.
std::expected<img::ImageBuffer, img::ImageError> letterbox_fit(img::ImageBuffer const &src, int W, int H) {
	if (src.width <= 0 || src.height <= 0)
		return std::unexpected(img::ImageError::DecodeFailed);

	double const scale = std::min(static_cast<double>(W) / src.width, static_cast<double>(H) / src.height);
	int const    sw    = std::clamp(static_cast<int>(std::lround(src.width * scale)), 1, W);
	int const    sh    = std::clamp(static_cast<int>(std::lround(src.height * scale)), 1, H);

	auto scaled = img::ops::resize(src, sw, sh); // aspect-correct intermediate
	if (!scaled)
		return std::unexpected(scaled.error());

	img::ImageBuffer out;
	out.width    = W;
	out.height   = H;
	out.channels = 4;
	out.data.assign(static_cast<std::size_t>(W) * H * 4, 0); // transparent padding

	int const         ox         = (W - sw) / 2;
	int const         oy         = (H - sh) / 2;
	std::size_t const row_bytes  = static_cast<std::size_t>(sw) * 4;
	std::size_t const dst_stride = static_cast<std::size_t>(W) * 4;
	for (int y = 0; y < sh; ++y) {
		std::uint8_t const *srow = scaled->data.data() + static_cast<std::size_t>(y) * row_bytes;
		std::uint8_t       *drow = out.data.data() + static_cast<std::size_t>(oy + y) * dst_stride
			+ static_cast<std::size_t>(ox) * 4;
		std::memcpy(drow, srow, row_bytes);
	}
	return out;
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
	if (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp"|| e == ".gif") {
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

void FileBrowserThumbnailContext::setup(vulkan_context *vk, std::filesystem::path thumb_dir) {
	m_vk        = vk;
	m_thumb_dir = std::move(thumb_dir);
	m_setup     = true;
	std::error_code ec;
	std::filesystem::create_directories(m_thumb_dir, ec);
	THUMB_LOG("setup thumb_dir={} vk={}", m_thumb_dir.string(), static_cast<void const *>(vk));

	m_video.start([this](std::string const &key, std::vector<std::uint8_t> rgba, bool ok) {
		on_video_done(key, std::move(rgba), ok);
	});
}

void FileBrowserThumbnailContext::shutdown() {
	if (!m_setup)
		return;
	m_setup = false;

	m_video.shutdown(); // stop + join the video worker
	img::ImageJobSystem::instance().clear_pending(); // drop queued image jobs

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
	// One composite pool job: decode -> resize(320x180) -> encode PNG (persistence) ->
	// return the RGBA for in-memory GPU upload. encode_png takes const&, so the same
	// buffer is both written to disk and handed back (no extra copy, no re-decode).
	// A cached PNG from a prior session is just decoded (already thumbnail-sized).
	e.img_future = img::ImageJobSystem::instance().submit(
		[file, out = e.png_path, decode_as_video]() -> ImgResult {

			std::error_code ec;

			if (std::filesystem::exists(out, ec) && !ec) {

				auto cached = img::ops::decode_file(out, 4);

				THUMB_LOG("img cache-HIT {} -> {}", out.filename().string(),
					cached ? px_summary(*cached) : std::string("DECODE-FAIL"));

				return cached;
			}

			// Video sources use ffmpegthumbnailer (smart non-black frame, ~20ms CPU
			// decode) instead of spinning up a full mpv+nvdec instance per file. Both
			// paths feed the same letterbox below, so caching/delivery are identical.
			THUMB_LOG("{} GENERATE from {}", decode_as_video ? "vid" : "img", file.filename().string());
			auto dec = decode_as_video ? img::ops::decode_video_thumbnail(file, k_thumb_w, 4)
									   : img::ops::decode_file(file, 4);

			if (!dec)
				return std::unexpected(dec.error());

			// Aspect-preserving: fit within k_thumb_w x k_thumb_h and letterbox onto a
			// transparent canvas instead of stretching the source to fill the box.
			auto rz = letterbox_fit(*dec, k_thumb_w, k_thumb_h);

			if (!rz)
				return std::unexpected(rz.error());

			std::filesystem::create_directories(out.parent_path(), ec);
			
			// Persist the thumbnail to disk so the next session loads it instead of
			// regenerating. MUST NOT be `static` — that would run encode_png only once
			// per process and leave every later thumbnail unwritten.
			auto const encoded = img::ops::encode_png(*rz, out); // best-effort
			static_cast<void>(encoded);
			return rz;
		},
		img::Priority::Normal);
	e.state = State::Generating;
}

void FileBrowserThumbnailContext::on_video_done(std::string const &key, std::vector<std::uint8_t> rgba, bool ok) {
	THUMB_LOG("video_done key={} ok={} bytes={}", key, ok, rgba.size());
	ImgResult res = std::unexpected(img::ImageError::DecodeFailed);
	if (ok && rgba.size() == static_cast<std::size_t>(k_thumb_w) * k_thumb_h * 4) {
		img::ImageBuffer b;
		b.width    = k_thumb_w;
		b.height   = k_thumb_h;
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
			it->second.pixels      = std::move(*res);
			it->second.have_pixels = true;
			it->second.state       = State::PixelsReady;
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

	// Upload at most k_max_uploads_per_frame textures per frame so a fresh grid of
	// ready thumbnails can't stall a single frame.
	if (e.state == State::PixelsReady && e.have_pixels && m_uploads_this_frame < k_max_uploads_per_frame) {
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
		std::string const &stored_key = it->first; // stable key string owned by the map
		if (e.png_path.empty())
			e.png_path = png_for(stored_key);

		std::error_code             ec;
		bool const                  have_png = std::filesystem::exists(e.png_path, ec) && !ec;
		std::filesystem::path const file     = dir / name; // joined only on a (re)submit

		THUMB_LOG("{} key={} video={} have_png={} png={}", e.state == State::Cached ? "RELOAD" : "NEW", stored_key,
			is_video, have_png, e.png_path.filename().string());
		// Image and video sources BOTH decode on the ImageJobSystem pool and deliver via
		// e.img_future. A fresh video source is decoded by ffmpegthumbnailer (~20ms CPU)
		// rather than the old per-file mpv+nvdec render (seconds, and it produced the wrong
		// 640x480 size the context then rejected). is_video stays false: poll_entry only
		// polls the future when is_video == false, so it MUST be false regardless of the
		// SOURCE type, else the decoded pixels are never picked up and the thumb stays blank.
		// (The m_video mpv worker pool is now unused — safe to remove in a follow-up.)
		e.is_video = false;
		submit_image(file, e, /*decode_as_video=*/is_video && !have_png);
	}

	return poll_entry(e);
}

void FileBrowserThumbnailContext::evict(std::filesystem::path const &path) {
	if (!m_setup)
		return;
	auto it = m_entries.find(key_for(path));
	if (it == m_entries.end())
		return;
	if (it->second.texture)
		m_retire.push_back({std::move(it->second.texture), k_retire_frames});
	if (!it->second.png_path.empty()) {
		std::error_code ec;
		std::filesystem::remove(it->second.png_path, ec);
	}
	m_entries.erase(it);
}

void FileBrowserThumbnailContext::clear() {
	if (!m_setup)
		return;
	m_video.clear_pending();
	img::ImageJobSystem::instance().clear_pending();
	std::error_code ec;
	for (auto &[k, e] : m_entries) {
		if (e.texture)
			m_retire.push_back({std::move(e.texture), k_retire_frames});
		if (!e.png_path.empty())
			std::filesystem::remove(e.png_path, ec);
	}
	m_entries.clear();
}
