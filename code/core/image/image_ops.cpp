#include "image_ops.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <fstream>
#include <vector>

#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#include <webp/decode.h>

// stb implementations live in the standalone `stb` library (see CMakeLists.txt);
// these headers are included declaration-only.

namespace img::ops {

namespace {

std::string lower_ext(const std::filesystem::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

stbir_pixel_layout layout_for(int channels) {
    switch (channels) {
    case 1:  return STBIR_1CHANNEL;
    case 2:  return STBIR_2CHANNEL;
    case 3:  return STBIR_RGB;
    default: return STBIR_RGBA;
    }
}

std::expected<ImageBuffer, ImageError>
decode_webp(const std::filesystem::path &file, int desired_channels) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return std::unexpected(ImageError::DecodeFailed);

    const auto size = static_cast<std::streamsize>(f.tellg());
    if (size <= 0)
        return std::unexpected(ImageError::DecodeFailed);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(std::bit_cast<char *>(bytes.data()), size);

    int           w  = 0;
    int           h  = 0;
    std::uint8_t *px = nullptr;
    if (desired_channels == 4)
        px = WebPDecodeRGBA(bytes.data(), bytes.size(), &w, &h);
    else if (desired_channels == 3)
        px = WebPDecodeRGB(bytes.data(), bytes.size(), &w, &h);
    else
        return std::unexpected(ImageError::UnsupportedFormat);

    if (px == nullptr)
        return std::unexpected(ImageError::DecodeFailed);

    ImageBuffer out;
    out.width    = w;
    out.height   = h;
    out.channels = desired_channels;
    out.data.assign(px, px + static_cast<std::size_t>(w) * h * desired_channels);
    WebPFree(px);
    return out;
}

} // namespace

std::expected<ImageBuffer, ImageError>
decode_file(const std::filesystem::path &file, int desired_channels) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
        return std::unexpected(ImageError::FileNotFound);

    if (lower_ext(file) == ".webp")
        return decode_webp(file, desired_channels);

    int           w  = 0;
    int           h  = 0;
    int           ch = 0;
    std::uint8_t *px = stbi_load(file.string().c_str(), &w, &h, &ch, desired_channels);
    if (px == nullptr)
        return std::unexpected(ImageError::DecodeFailed);

    ImageBuffer out;
    out.width    = w;
    out.height   = h;
    out.channels = desired_channels;
    out.data.assign(px, px + static_cast<std::size_t>(w) * h * desired_channels);
    stbi_image_free(px);
    return out;
}

std::expected<ImageBuffer, ImageError> resize(const ImageBuffer &src, int dst_w, int dst_h) {
    if (!src.valid() || dst_w <= 0 || dst_h <= 0)
        return std::unexpected(ImageError::ResizeFailed);

    ImageBuffer dst;
    dst.width    = dst_w;
    dst.height   = dst_h;
    dst.channels = src.channels;
    dst.data.resize(static_cast<std::size_t>(dst_w) * dst_h * src.channels);

    const unsigned char *res = stbir_resize_uint8_linear(
        src.data.data(), src.width, src.height, 0,
        dst.data.data(), dst_w, dst_h, 0, layout_for(src.channels));

    if (res == nullptr)
        return std::unexpected(ImageError::ResizeFailed);
    return dst;
}

std::expected<ImageBuffer, ImageError>
resize_parallel(const ImageBuffer &src, int dst_w, int dst_h, int max_splits,
                const SplitExecutor &exec) {
    if (!src.valid() || dst_w <= 0 || dst_h <= 0 || max_splits < 1)
        return std::unexpected(ImageError::ResizeFailed);

    ImageBuffer dst;
    dst.width    = dst_w;
    dst.height   = dst_h;
    dst.channels = src.channels;
    dst.data.resize(static_cast<std::size_t>(dst_w) * dst_h * src.channels);

    // Mirror stbir_resize_uint8_linear's setup (UINT8 + CLAMP + default filter), then
    // swap the single-shot resize for the split API so the work can be parallelized.
    // Output is bit-identical to the single-shot path regardless of the split count.
    STBIR_RESIZE r;
    stbir_resize_init(&r,
                      src.data.data(), src.width, src.height, 0,
                      dst.data.data(), dst_w, dst_h, 0,
                      layout_for(src.channels), STBIR_TYPE_UINT8);
    r.horizontal_edge   = STBIR_EDGE_CLAMP;
    r.vertical_edge     = STBIR_EDGE_CLAMP;
    r.horizontal_filter = STBIR_FILTER_DEFAULT;
    r.vertical_filter   = STBIR_FILTER_DEFAULT;

    const int splits = stbir_build_samplers_with_splits(&r, max_splits);
    if (splits <= 0)
        return std::unexpected(ImageError::ResizeFailed);

    std::atomic<int> failures{0};
    exec(splits, [&](int i) {
        if (stbir_resize_extended_split(&r, i, 1) == 0)
            failures.fetch_add(1, std::memory_order_relaxed);
    });

    stbir_free_samplers(&r);

    if (failures.load(std::memory_order_relaxed) != 0)
        return std::unexpected(ImageError::ResizeFailed);
    return dst;
}

std::expected<bool, ImageError>
encode_png(const ImageBuffer &src, const std::filesystem::path &out) {
    if (!src.valid())
        return std::unexpected(ImageError::EncodeFailed);

    const int rc = stbi_write_png(out.string().c_str(), src.width, src.height, src.channels,
                                  src.data.data(), static_cast<int>(src.stride()));
    if (rc == 0)
        return std::unexpected(ImageError::EncodeFailed);
    return true;
}

} // namespace img::ops
