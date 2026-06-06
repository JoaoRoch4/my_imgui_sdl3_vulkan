#include <doctest.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <system_error>
#include <vector>

#include "image_buffer.hpp"
#include "image_job_system.hpp"
#include "image_ops.hpp"

using namespace img;

namespace {

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

std::filesystem::path tmp(const char *n) { return std::filesystem::temp_directory_path() / n; }

ImageJobSystem::Config test_cfg(unsigned workers) {
    ImageJobSystem::Config c;
    c.worker_count = workers;
    c.watch        = false; // no ThreadOverwatch monitor thread in unit tests
    return c;
}

} // namespace

TEST_CASE("ImageJobSystem runs encode/decode/resize on the pool") {
    ImageJobSystem js;
    js.start(test_cfg(4));
    CHECK(js.worker_count() == 4);
    CHECK(js.running());

    const auto path = tmp("ijs_roundtrip.png");
    REQUIRE(js.encode_png(make_gradient(128, 96), path).get().has_value());

    const auto dec = js.decode(path, 4).get();
    REQUIRE(dec.has_value());
    CHECK(dec->width == 128);
    CHECK(dec->height == 96);

    const auto rz = js.resize(make_gradient(512, 512), 300, 200).get(); // large -> tiled
    REQUIRE(rz.has_value());
    CHECK(rz->width == 300);
    CHECK(rz->height == 200);

    js.shutdown();
    CHECK_FALSE(js.running());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageJobSystem processes many concurrent jobs") {
    ImageJobSystem js;
    js.start(test_cfg(4));

    const auto path = tmp("ijs_many.png");
    REQUIRE(ops::encode_png(make_gradient(64, 64), path).has_value());

    std::vector<std::future<std::expected<ImageBuffer, ImageError>>> futs;
    for (int i = 0; i < 200; ++i)
        futs.push_back(js.decode(path, 4));

    int ok = 0;
    for (auto &f : futs) {
        const auto r = f.get();
        if (r.has_value() && r->width == 64)
            ++ok;
    }
    CHECK(ok == 200);

    js.shutdown();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("ImageJobSystem tiled resize equals single-shot end-to-end") {
    ImageJobSystem js;
    js.start(test_cfg(4));

    const auto single = ops::resize(make_gradient(400, 400), 333, 211); // reference
    REQUIRE(single.has_value());

    const auto multi = js.resize(make_gradient(400, 400), 333, 211).get(); // pool, tiled
    REQUIRE(multi.has_value());
    REQUIRE(multi->data.size() == single->data.size());
    CHECK(multi->data == single->data); // bit-exact through the real worker pool

    js.shutdown();
}

TEST_CASE("ImageJobSystem shuts down promptly with pending work") {
    ImageJobSystem js;
    js.start(test_cfg(2));

    const auto path = tmp("ijs_flood.png");
    REQUIRE(ops::encode_png(make_gradient(32, 32), path).has_value());
    for (int i = 0; i < 500; ++i)
        (void)js.decode(path, 4); // flood, drop the futures

    js.shutdown(); // must not hang: clears pending, joins workers
    CHECK_FALSE(js.running());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
