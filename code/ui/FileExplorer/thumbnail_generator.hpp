#pragma once

#include "pch.hpp"

#include "image_buffer.hpp"
#include "image_types.hpp"

// ThumbnailGenerator — create a file-browser thumbnail FROM A MEDIA SOURCE the first
// time it is seen, and persist it as a PNG. This is the "cache-miss" engine:
//   * video  -> ffmpegthumbnailer (img::ops::decode_video_thumbnail, workaround_bugs);
//   * image/gif -> stb (first frame; never seeks, so no ffmpegthumbnailer seek failures).
// The decoded source is letterboxed onto a fixed w x h transparent canvas and written
// to out_png for the Reproducer to reload later. All methods are static and hold no
// shared state, so they run directly on the ImageJobSystem worker pool with no `this`
// capture and no cross-thread aliasing.
class ThumbnailGenerator {
	public:

		using Result = std::expected<img::ImageBuffer, img::ImageError>;

		// Decode `source` (video via ffmpegthumbnailer, image/gif via stb), letterbox to
		// w x h, persist as PNG at out_png (best-effort), and return the letterboxed RGBA.
		[[nodiscard]] static Result generate(std::filesystem::path const &source, bool is_video,
			std::filesystem::path const &out_png, int w, int h);
};
