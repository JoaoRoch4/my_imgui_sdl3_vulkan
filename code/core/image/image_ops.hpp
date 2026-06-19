#pragma once

#include "image_buffer.hpp"
#include "image_types.hpp"
#include <expected>
#include <filesystem>
#include <functional>

// Synchronous, thread-free image operations wrapping stb + libwebp. These are pure
// functions with no scheduling, no Vulkan and no project-PCH dependency, so they can
// be unit-tested directly. ImageJobSystem builds the parallel/async layer on top.

namespace img::ops {

// Decode the first frame of any file FFmpeg can open (PNG/JPEG/WebP/MP4/MKV/...)
// into an interleaved 8-bit buffer with `desired_channels` channels (4 = RGBA).
// For *video* thumbnails prefer decode_video_thumbnail(): this grabs an early
// keyframe, which is frequently black/blank.
[[nodiscard]] std::expected<ImageBuffer, ImageError>
decode_file(const std::filesystem::path &file, int desired_channels = 4);

// Decode a representative *video* thumbnail via ffmpegthumbnailer, which picks a
// high-variance (non-black) frame instead of the first keyframe decode_file() lands
// on. Output is an interleaved 8-bit buffer with `desired_channels` (3 = RGB,
// 4 = RGBA with opaque alpha). `thumbnail_size` is the longest-edge target in pixels
// (<= 0 = native frame size). The frame-selection policy lives in the .cpp.
[[nodiscard]] std::expected<ImageBuffer, ImageError>
decode_video_thumbnail(const std::filesystem::path &file, int thumbnail_size,
                       int desired_channels = 4);

// Single-threaded resize (stb_image_resize2, linear). The multithreaded split-based
// resize lives in ImageJobSystem and is validated bit-exact against this one.
[[nodiscard]] std::expected<ImageBuffer, ImageError>
resize(const ImageBuffer &src, int dst_w, int dst_h);

// Executor contract: invoke `run(i)` for every i in [0, total_splits); may run them
// concurrently but MUST return only once all have completed.
using SplitExecutor =
    std::function<void(int total_splits, const std::function<void(int)> &run)>;

// Multithread-capable resize using stb's split API. Splitting into up to `max_splits`
// pieces and running them through `exec` produces byte-for-byte identical output to
// resize() regardless of the split count. ImageJobSystem injects a pool-backed
// executor; tests inject a sequential one.
[[nodiscard]] std::expected<ImageBuffer, ImageError>
resize_parallel(const ImageBuffer &src, int dst_w, int dst_h, int max_splits,
                const SplitExecutor &exec);

// Encode an RGBA/RGB buffer to a PNG file (stb_image_write).
[[nodiscard]] std::expected<bool, ImageError>
encode_png(const ImageBuffer &src, const std::filesystem::path &out);

} // namespace img::ops
