#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "image_buffer.hpp"
#include "image_ops.hpp"

using namespace img;

namespace {

// Build a deterministic w*h RGBA buffer whose bytes are a ramp from `base`.
ImageBuffer make_rgba(int w, int h, std::uint8_t base) {
    ImageBuffer b;
    b.width    = w;
    b.height   = h;
    b.channels = 4;
    b.data.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < b.data.size(); ++i)
        b.data[i] = static_cast<std::uint8_t>((static_cast<std::size_t>(base) + i) & 0xFFu);
    return b;
}

std::filesystem::path temp_file(const char *name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("ImageBuffer reports stride and validity") {
    const ImageBuffer b = make_rgba(2, 2, 0);
    CHECK(b.valid());
    CHECK(b.stride() == 8); // 2 px * 4 channels
}

TEST_CASE("encode_png then decode_file round-trips pixels") {
    const auto  path = temp_file("img_roundtrip_2x2.png");
    ImageBuffer src  = make_rgba(2, 2, 10);

    const auto enc = ops::encode_png(src, path);
    REQUIRE(enc.has_value());

    const auto dec = ops::decode_file(path, 4);
    REQUIRE(dec.has_value());
    CHECK(dec->width == 2);
    CHECK(dec->height == 2);
    CHECK(dec->channels == 4);
    REQUIRE(dec->data.size() == src.data.size());
    CHECK(std::equal(src.data.begin(), src.data.end(), dec->data.begin()));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("decode_file on a missing path fails with FileNotFound") {
    const auto dec = ops::decode_file(temp_file("does_not_exist_zzz.png"), 4);
    REQUIRE_FALSE(dec.has_value());
    CHECK(dec.error() == ImageError::FileNotFound);
}

TEST_CASE("resize produces the requested dimensions and channels") {
    const ImageBuffer src = make_rgba(8, 8, 0);
    const auto        dst = ops::resize(src, 4, 2);
    REQUIRE(dst.has_value());
    CHECK(dst->width == 4);
    CHECK(dst->height == 2);
    CHECK(dst->channels == 4);
    CHECK(dst->valid());
    CHECK(dst->data.size() == static_cast<std::size_t>(4) * 2 * 4);
}

TEST_CASE("resize of a solid-colour image preserves the colour") {
    // A uniform image must stay uniform after resampling (no edge artifacts).
    ImageBuffer src;
    src.width = 16; src.height = 16; src.channels = 4;
    src.data.assign(static_cast<std::size_t>(16) * 16 * 4, 0);
    for (std::size_t i = 0; i < src.data.size(); i += 4) {
        src.data[i + 0] = 200; src.data[i + 1] = 100;
        src.data[i + 2] = 50;  src.data[i + 3] = 255;
    }
    const auto dst = ops::resize(src, 4, 4);
    REQUIRE(dst.has_value());
    for (std::size_t i = 0; i < dst->data.size(); i += 4) {
        CHECK(dst->data[i + 0] == 200);
        CHECK(dst->data[i + 1] == 100);
        CHECK(dst->data[i + 2] == 50);
        CHECK(dst->data[i + 3] == 255);
    }
}

TEST_CASE("resize rejects an invalid source") {
    const ImageBuffer empty;
    const auto        dst = ops::resize(empty, 4, 4);
    REQUIRE_FALSE(dst.has_value());
    CHECK(dst.error() == ImageError::ResizeFailed);
}

namespace {
// Gradient RGBA source — non-uniform so resampling differences would show up.
ImageBuffer make_gradient(int w, int h) {
    ImageBuffer b;
    b.width = w; b.height = h; b.channels = 4;
    b.data.resize(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
            b.data[o + 0] = static_cast<std::uint8_t>((x * 7) & 0xFF);
            b.data[o + 1] = static_cast<std::uint8_t>((y * 5) & 0xFF);
            b.data[o + 2] = static_cast<std::uint8_t>(((x + y) * 3) & 0xFF);
            b.data[o + 3] = 255;
        }
    return b;
}
// Sequential executor: runs every split on the calling thread, in order.
const ops::SplitExecutor sequential_exec =
    [](int total, const std::function<void(int)> &run) {
        for (int i = 0; i < total; ++i) run(i);
    };
} // namespace

TEST_CASE("split resize matches single-shot byte-for-byte (sequential executor)") {
    const ImageBuffer src    = make_gradient(64, 48);
    const auto        single = ops::resize(src, 23, 19); // odd dims stress split boundaries
    REQUIRE(single.has_value());

    for (const int max_splits : {1, 2, 3, 4, 8, 19, 64}) {
        CAPTURE(max_splits);
        const auto multi = ops::resize_parallel(src, 23, 19, max_splits, sequential_exec);
        REQUIRE(multi.has_value());
        REQUIRE(multi->data.size() == single->data.size());
        CHECK(multi->data == single->data); // bit-exact, regardless of split count
    }
}

TEST_CASE("resize_parallel rejects invalid args") {
    const ImageBuffer src = make_gradient(8, 8);
    CHECK_FALSE(ops::resize_parallel(src, 0, 4, 4, sequential_exec).has_value());
    CHECK_FALSE(ops::resize_parallel(src, 4, 4, 0, sequential_exec).has_value());
    const ImageBuffer empty;
    CHECK_FALSE(ops::resize_parallel(empty, 4, 4, 4, sequential_exec).has_value());
}

TEST_CASE("bc1_size follows the 4x4-block, 8-bytes-per-block law") {
    CHECK(ops::bc1_size(320, 180) == static_cast<std::size_t>(320 / 4) * (180 / 4) * 8); // 80*45*8 = 28800
    CHECK(ops::bc1_size(4, 4) == 8);
    CHECK(ops::bc1_size(5, 1) == 16); // ceil(5/4)=2, ceil(1/4)=1 -> 2*1*8 = 16
}

TEST_CASE("encode_bc1 emits exactly bc1_size bytes for a non-multiple-of-4 image") {
    const ImageBuffer src = make_rgba(7, 3, 0); // ceil(7/4)=2, ceil(3/4)=1 -> 2 blocks
    const auto        enc = ops::encode_bc1(src);
    REQUIRE(enc.has_value());
    CHECK(enc->size() == ops::bc1_size(7, 3));
    CHECK(enc->size() == 16u);
}

TEST_CASE("encode_bc1 of a solid colour decodes (RGB565 endpoint) near that colour") {
    ImageBuffer src;
    src.width = 4; src.height = 4; src.channels = 4;
    src.data.assign(static_cast<std::size_t>(4) * 4 * 4, 0);
    for (std::size_t p = 0; p < 16; ++p) { // solid red, opaque
        src.data[p * 4 + 0] = 200; src.data[p * 4 + 3] = 255;
    }
    const auto enc = ops::encode_bc1(src);
    REQUIRE(enc.has_value());
    REQUIRE(enc->size() == 8u);
    // BC1 block: bytes 0-1 = colour0 as little-endian RGB565.
    const auto     b0 = static_cast<unsigned>(std::to_integer<std::uint8_t>((*enc)[0]));
    const auto     b1 = static_cast<unsigned>(std::to_integer<std::uint8_t>((*enc)[1]));
    const unsigned c0 = b0 | (b1 << 8);
    const unsigned r5 = (c0 >> 11) & 0x1F;
    const unsigned r8 = (r5 * 255) / 31;
    CHECK(r8 > 150); // endpoint red channel is high
}

TEST_CASE("encode_bc1 rejects a non-RGBA buffer") {
    ImageBuffer rgb; rgb.width = 4; rgb.height = 4; rgb.channels = 3;
    rgb.data.assign(static_cast<std::size_t>(4) * 4 * 3, 0);
    CHECK_FALSE(ops::encode_bc1(rgb).has_value());
}

TEST_CASE("encode_bc1_parallel matches encode_bc1 byte-for-byte regardless of split count") {
    // Real thumbnail size — exercises edge-replication on the non-multiple-of-4 right edge.
    const ImageBuffer src    = make_gradient(317, 181); // odd dims stress block boundaries
    const auto        single = ops::encode_bc1(src);
    REQUIRE(single.has_value());

    for (const int max_splits : {1, 2, 3, 5, 8, 16, 46}) {
        CAPTURE(max_splits);
        const auto multi = ops::encode_bc1_parallel(src, max_splits, sequential_exec);
        REQUIRE(multi.has_value());
        REQUIRE(multi->size() == single->size());
        CHECK(*multi == *single); // bit-exact, regardless of split count
    }
}

TEST_CASE("encode_bc1_parallel rejects invalid args") {
    const ImageBuffer src = make_gradient(8, 8);
    CHECK_FALSE(ops::encode_bc1_parallel(src, 0, sequential_exec).has_value());
    const ImageBuffer empty;
    CHECK_FALSE(ops::encode_bc1_parallel(empty, 4, sequential_exec).has_value());
    ImageBuffer rgb; rgb.width = 4; rgb.height = 4; rgb.channels = 3;
    rgb.data.assign(static_cast<std::size_t>(4) * 4 * 3, 0);
    CHECK_FALSE(ops::encode_bc1_parallel(rgb, 4, sequential_exec).has_value());
}
