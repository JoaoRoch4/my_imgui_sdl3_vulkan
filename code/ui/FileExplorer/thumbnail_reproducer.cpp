#include "pch.hpp" // NOLINT

#include "thumbnail_reproducer.hpp"

#include "thumbnail_image_io.hpp"

ThumbnailReproducer::Result ThumbnailReproducer::reload(std::filesystem::path const &png_path) {
	// The cached file is an already-sized thumbnail PNG written by the Generator, so a
	// straight stb decode is all that's needed — no resize, no letterbox, no libav.
	return fbthumb::decode_stb(png_path, 4);
}

void ThumbnailReproducer::start(DoneFn on_done, int thumb_w, int thumb_h) {
	// Match the worker's output size to the context's thumbnail dimensions before the
	// pool spins up (render_video reads these per job).
	m_mpv.k_thumb_w = static_cast<unsigned int>(thumb_w);
	m_mpv.k_thumb_h = static_cast<unsigned int>(thumb_h);
	m_mpv.start(std::move(on_done));
}

void ThumbnailReproducer::shutdown() { m_mpv.shutdown(); }

void ThumbnailReproducer::submit(std::string key, std::filesystem::path file, std::filesystem::path out_png) {
	m_mpv.submit(std::move(key), std::move(file), std::move(out_png));
}

void ThumbnailReproducer::clear_pending() { m_mpv.clear_pending(); }
