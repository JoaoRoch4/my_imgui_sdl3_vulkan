#include <doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <vector>

#include "thumbnail_blob_cache.hpp"

namespace {
std::vector<std::byte> blob(std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v)
        b.push_back(static_cast<std::byte>(x));
    return b;
}
std::filesystem::path tmp_dir(const char *name) {
    auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
} // namespace

TEST_CASE("store then lookup returns byte-exact payload and dims") {
    ThumbnailBlobCache c;
    REQUIRE(c.open(tmp_dir("tbc_roundtrip"), 1u << 20));
    const auto payload = blob({1, 2, 3, 4, 5, 6, 7, 8});
    REQUIRE(c.store(42, payload, 4, 4));
    const auto got = c.lookup(42);
    REQUIRE(got.has_value());
    CHECK(got->w == 4);
    CHECK(got->h == 4);
    REQUIRE(got->blocks.size() == payload.size());
    CHECK(std::equal(payload.begin(), payload.end(), got->blocks.begin()));
    CHECK_FALSE(c.lookup(99).has_value());
    c.close();
}

TEST_CASE("index persists across close/open") {
    const auto dir = tmp_dir("tbc_persist");
    {
        ThumbnailBlobCache c;
        REQUIRE(c.open(dir, 1u << 20));
        REQUIRE(c.store(7, blob({9, 9, 9, 9, 9, 9, 9, 9}), 4, 4));
        c.close();
    }
    ThumbnailBlobCache c2;
    REQUIRE(c2.open(dir, 1u << 20));
    REQUIRE(c2.lookup(7).has_value());
    c2.close();
}

TEST_CASE("LRU cap evicts the least-recently-used entry") {
    ThumbnailBlobCache c;
    REQUIRE(c.open(tmp_dir("tbc_lru"), 16)); // room for two 8-byte payloads
    REQUIRE(c.store(1, blob({1, 1, 1, 1, 1, 1, 1, 1}), 4, 4));
    REQUIRE(c.store(2, blob({2, 2, 2, 2, 2, 2, 2, 2}), 4, 4));
    (void)c.lookup(1);                                       // 1 is now most-recently-used
    REQUIRE(c.store(3, blob({3, 3, 3, 3, 3, 3, 3, 3}), 4, 4)); // over cap -> evict LRU (key 2)
    CHECK(c.lookup(1).has_value());
    CHECK_FALSE(c.lookup(2).has_value());
    CHECK(c.lookup(3).has_value());
    c.close();
}

TEST_CASE("corrupt index header -> clean empty rebuild, no crash") {
    const auto dir = tmp_dir("tbc_corrupt");
    {
        ThumbnailBlobCache c;
        REQUIRE(c.open(dir, 1u << 20));
        REQUIRE(c.store(5, blob({5, 5, 5, 5, 5, 5, 5, 5}), 4, 4));
        c.close();
    }
    { std::ofstream f(dir / "thumbs.index", std::ios::binary | std::ios::trunc); f << "garbage"; }
    ThumbnailBlobCache c2;
    REQUIRE(c2.open(dir, 1u << 20));
    CHECK_FALSE(c2.lookup(5).has_value()); // discarded, starts empty
    c2.close();
}
