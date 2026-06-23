#include "pch.hpp" // NOLINT

#include "thumbnail_generator.hpp"

#include <cstring> // std::memcpy

#include "debug_log.hpp"          // APP_DEBUG_LOG — fallback notice
#include "image_job_system.hpp"   // ImageJobSystem::encode_bc1 (pool-fanned CPU)
#include "image_ops.hpp"
#include "thumbnail_image_io.hpp"
#include "vulkan_bc1_encoder.hpp" // VulkanBc1Encoder::instance().submit() (GPU, batched)

namespace {

// Scale `src` to fit inside WxH preserving aspect ratio, then centre it on a fully
// transparent WxH RGBA canvas (letterbox). Keeps the thumbnail's fixed dimensions so the
// cache/upload/display pipeline is uniform, while the source is never stretched.
// (Moved verbatim from FileBrowserThumbnailContext's anon namespace — the Generator now
// owns the full source -> sized-PNG pipeline.)
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

ThumbnailGenerator::Result ThumbnailGenerator::generate(std::filesystem::path const &source, bool is_video,
	std::filesystem::path const &out_png, int w, int h) {
	// Source decode: video -> ffmpegthumbnailer (smart non-black frame, workaround_bugs for
	// odd/seek-resistant streams); image/gif -> stb (first frame, no seek).
	auto dec = is_video ? img::ops::decode_video_thumbnail(source, w, 4) : fbthumb::decode_stb(source, 4);
	if (!dec)
		return std::unexpected(dec.error());

	auto rz = letterbox_fit(*dec, w, h);
	if (!rz)
		return std::unexpected(rz.error());

	// Persist the sized thumbnail so the Reproducer reloads it next time instead of
	// re-running the source decode. Best-effort: a failed write just means regeneration later.
	std::error_code ec;
	std::filesystem::create_directories(out_png.parent_path(), ec);
	auto const encoded = img::ops::encode_png(*rz, out_png);
	static_cast<void>(encoded);

	return rz;
}

ThumbnailGenerator::Bc1Result ThumbnailGenerator::generate_bc1(std::filesystem::path const &source, bool is_video,
	int w, int h) {
	// Same decode + letterbox as generate(), but produce BC1 blocks (no PNG). The
	// transparent letterbox padding encodes as opaque black since BC1 carries no alpha.
	auto dec = is_video ? img::ops::decode_video_thumbnail(source, w, 4) : fbthumb::decode_stb(source, 4);
	if (!dec)
		return std::unexpected(dec.error());

	auto rz = letterbox_fit(*dec, w, h);
	if (!rz)
		return std::unexpected(rz.error());

	// Backend policy is fixed by file type:
	//   * VIDEO thumbnails go to the GPU encoder.  Per-thumb decode (ffmpegthumbnailer
	//     seek + frame) costs 40-200 ms, so the GPU's ~1 ms round-trip is in the noise
	//     and batched dispatch amortizes well across a folder of videos.
	//   * IMAGE thumbnails go to the pool-fanned CPU encoder.  stb decode is fast
	//     enough that the GPU round-trip would dominate; CPU + JobQueue::parallel_for
	//     wins on per-thumb wall-clock.
	// GPU is best-effort: if not ready at scan time, video falls back to CPU here too.
	if (is_video && VulkanBc1Encoder::instance().is_ready()) {
		// Move into submit; the encoder owns the RGBA from this point. On submission
		// failure (in-flight cap, OOM) the future already holds the error; the
		// thumbnail engine marks the entry Failed and will retry on next eviction.
		auto fut = VulkanBc1Encoder::instance().submit(std::move(*rz));
		return fut.get();
	}
	// Pool-fanned CPU: we're already on an ImageJobSystem worker; parallel_for's
	// calling-thread-helps protocol keeps the nested fan-out deadlock-free.
	return img::ImageJobSystem::instance().encode_bc1(*rz);
}
