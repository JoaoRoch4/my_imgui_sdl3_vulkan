#pragma once

#include "SDL3/SDL_stdinc.h"
#include "pch.hpp"

#include <bit>
#include <cstdint>
#include <stb_image.h> // decoder only; STB_IMAGE_IMPLEMENTATION lives in thirdparty/stb

#include "image_buffer.hpp"
#include "image_types.hpp"

// Shared stb (stbi_load) image decoder for the file-browser thumbnail classes.
// Header-only inline so it adds no translation unit; the stb implementation is
// already linked through the thirdparty/stb target. Used by ThumbnailGenerator
// (decode an image/gif SOURCE) and ThumbnailReproducer (decode a cached PNG).
namespace fbthumb {

[[nodiscard]] inline std::expected<img::ImageBuffer, img::ImageError>
decode_stb(std::filesystem::path const &file, int desired_channels = 4) {
	if (desired_channels != 3 && desired_channels != 4)
		return std::unexpected(img::ImageError::UnsupportedFormat);
	int width    = 0;
	int height   = 0;
	int channels = 0; 


	// stbi_load sniffs the format from content (PNG/JPG/WebP/GIF-first-frame/...), not the
	// extension, and converts to `desired_channels`. For animated GIFs it returns frame 0,
	// which is exactly what a thumbnail needs — and it never seeks, so it can't hit the
	// ffmpegthumbnailer "Seeking in video failed" path.
	stbi_uc *pixels 
		= stbi_load(file.string().c_str(), &width, &height, &channels, desired_channels);	
		if (pixels == nullptr || width <= 0 || height <= 0) {
		if (pixels != nullptr)
			stbi_image_free(pixels);
		return std::unexpected(img::ImageError::DecodeFailed);
	}

	img::ImageBuffer out;
	out.width    = width;
	out.height   = height;
	out.channels = desired_channels;
	std::size_t const bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
		* static_cast<std::size_t>(desired_channels);
	out.data.assign(pixels, pixels + bytes);
	stbi_image_free(pixels);
	return out;
}

} // namespace fbthumb
