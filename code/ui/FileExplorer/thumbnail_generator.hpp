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

		// Full-quality IMAGE path: decode the source at its NATIVE resolution and aspect
		// (stb), with NO letterbox and NO disk persistence. Only downscales when the long
		// edge exceeds max_edge (preserving aspect), as a VRAM guard — otherwise the buffer
		// is the original pixels. This makes a masonry/grid thumbnail look identical to the
		// full-res hover preview (both decode the original); the fixed-cell views fit it at
		// display time. Images only — videos keep the letterboxed generate()/generate_bc1().
		[[nodiscard]] static Result generate_image_full(std::filesystem::path const &source, int max_edge);

		using Bc1Result = std::expected<std::vector<std::byte>, img::ImageError>;

		// BC1 backend cache-miss engine: decode + letterbox to w x h exactly like
		// generate(), but encode the result to BC1/DXT1 blocks (img::ops::encode_bc1)
		// and return them instead of persisting a PNG. The blob cache owns persistence.
		// Letterbox padding becomes opaque black (BC1 has no alpha). Runs on the pool.
		[[nodiscard]] static Bc1Result generate_bc1(std::filesystem::path const &source, bool is_video,
			int w, int h);
};
