#pragma once

#include "pch.hpp"

#include "file_browser_thumbnail_context_thread.hpp"
#include "image_buffer.hpp"
#include "image_types.hpp"

// ThumbnailReproducer — reload an already-generated thumbnail for display (the
// "cache-hit" engine), plus a re-derive fallback for videos the Generator's
// ffmpegthumbnailer could not handle.
//   * reload(): decode the cached thumbnail PNG with stb (fast, no per-file libav
//     AVFormatContext). Static + thread-safe, so it runs on the ImageJobSystem pool.
//   * the owned libmpv worker pool (hwdec=nvdec-copy + software readback) re-derives a
//     frame from a video source on demand; results arrive asynchronously through the
//     on_done callback the owner wires up. Worker methods are render-thread only.
class ThumbnailReproducer {
	public:

		using Result = std::expected<img::ImageBuffer, img::ImageError>;
		using DoneFn = FileBrowserThumbnailThread::DoneFn;

		// Decode a cached thumbnail PNG into RGBA via stb.
		[[nodiscard]] static Result reload(std::filesystem::path const &png_path);

		// ---- mpv (nvdec-copy) re-derive fallback. Call from the render thread only. ----
		void start(DoneFn on_done, int thumb_w, int thumb_h); // spawn the worker pool
		void shutdown();                                       // stop + join
		void submit(std::string key, std::filesystem::path file, std::filesystem::path out_png);
		void clear_pending();

	private:

		FileBrowserThumbnailThread m_mpv; // owns the nvdec-copy render workers
};
