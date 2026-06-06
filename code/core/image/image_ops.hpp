#pragma once

#include <expected>
#include <filesystem>
#include <functional>

#include "image_buffer.hpp"
#include "image_types.hpp"

// Synchronous, thread-free image operations wrapping stb + libwebp. These are pure
// functions with no scheduling, no Vulkan and no project-PCH dependency, so they can
// be unit-tested directly. ImageJobSystem builds the parallel/async layer on top.

namespace img::ops {

// Decode an image file (PNG/JPEG/WebP/...) into an interleaved 8-bit buffer with
// `desired_channels` channels (4 = RGBA). libwebp handles .webp; stb_image the rest.
[[nodiscard]] std::expected<ImageBuffer, ImageError>
decode_file(const std::filesystem::path &file, int desired_channels = 4);

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
