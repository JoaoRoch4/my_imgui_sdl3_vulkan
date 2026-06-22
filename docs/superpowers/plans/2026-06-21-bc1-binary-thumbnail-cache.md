# BC1 Binary Thumbnail Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional BC1 (block-compressed) thumbnail backend that stores all file-browser thumbnails in a single binary blob and uploads them into the GPU with no decode step, ~8:1 smaller than the current per-file PNG cache.

**Architecture:** A pure encoder (`img::ops::encode_bc1`, over header-only `stb_dxt`) produces BC1 block data from a letterboxed RGBA buffer. A Vulkan-free `ThumbnailBlobCache` stores those blocks in one append-only blob keyed by `hash(path+mtime+size)`, with an in-memory index, an LRU byte cap, and compaction. `VulkanTexture::upload_bc1` stages the blocks straight into a `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` image. `FileThumbnailCache` picks the backend (bc1 vs png) at setup from config ∧ device support, with png as the automatic fallback.

**Tech Stack:** C++23, `stb_dxt.h` (vendored, header-only), Vulkan (BC compressed textures), doctest, CMake/Ninja, clang/LLD.

## Global Constraints

- Toolchain: clang/clang++ + LLD for every config (Debug included). (`llvm-toolchain-preference`)
- Style: modern C++23 — `static_cast`/`std::bit_cast` only (no C casts), smart pointers, `std::span`/`string_view`, `constexpr`, `std::println` for logs, attached braces. (`cpp-code-style`)
- `code/core/image/image_ops.{hpp,cpp}` MUST stay PCH-free (no `#include "pch.hpp"`); the `image_tests` target has no code/pch include path. Use explicit std headers only. (`image-core-pch-free`)
- Thumbnail generators run on worker threads and MUST NOT touch Vulkan; only the render thread uploads. (existing `FileThumbnailCache` invariant)
- On-disk thumbnail dimensions stay `k_thumb_w = 320`, `k_thumb_h = 180`.
- BC1 block layout: 8 bytes per 4×4 texel block; payload length for a W×H image is `ceil(W/4) * ceil(H/4) * 8` bytes.

---

### Task 1: `encode_bc1` + vendored `stb_dxt`

**Files:**
- Create: `thirdparty/stb/stb_dxt.h` (vendored header-only encoder; download from the nothings/stb repo, unmodified)
- Modify: `code/core/image/image_ops.hpp` (add declaration after `encode_png`, line ~51)
- Modify: `code/core/image/image_ops.cpp` (add implementation + a single `#define STB_DXT_IMPLEMENTATION` include)
- Test: `tests/image_ops_test.cpp` (append test cases)

**Interfaces:**
- Consumes: `img::ImageBuffer` (`image_buffer.hpp`), `img::ImageError` (`image_types.hpp`).
- Produces:
  ```cpp
  // Encode an RGBA8 buffer (channels == 4) to BC1/DXT1 block data (8 bytes per
  // 4x4 block, no alpha). Output length == bc1_size(src.width, src.height).
  // Alpha is ignored by BC1: callers wanting opaque letterbox must bake the
  // padding colour before calling.
  [[nodiscard]] std::expected<std::vector<std::byte>, ImageError>
  img::ops::encode_bc1(const ImageBuffer &src);

  // Byte length of BC1 data for a w*h image. constexpr, header-inline.
  [[nodiscard]] constexpr std::size_t img::ops::bc1_size(int w, int h);
  ```

- [ ] **Step 1: Vendor `stb_dxt.h`**

Place the upstream `stb_dxt.h` (header-only public-domain BC1/BC3 encoder from github.com/nothings/stb) at `thirdparty/stb/stb_dxt.h`, byte-for-byte unmodified. Confirm it declares:
```c
void stb_compress_dxt_block(unsigned char *dest, const unsigned char *src_rgba_four_bytes_per_pixel, int alpha, int mode);
#define STB_DXT_NORMAL   0
#define STB_DXT_HIGHQUAL 2
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/image_ops_test.cpp`:
```cpp
#include "image_ops.hpp" // already included; encode_bc1 lives here

TEST_CASE("bc1_size follows the 4x4-block, 8-bytes-per-block law") {
    CHECK(ops::bc1_size(320, 180) == (320 / 4) * (180 / 4) * 8); // 80*45*8 = 28800
    CHECK(ops::bc1_size(4, 4) == 8);
    CHECK(ops::bc1_size(5, 1) == 16); // ceil(5/4)=2, ceil(1/4)=1 -> 2*1*8 = 16
}

TEST_CASE("encode_bc1 emits exactly bc1_size bytes for a non-multiple-of-4 image") {
    ImageBuffer src = make_rgba(7, 3, 0); // ceil(7/4)=2, ceil(3/4)=1 -> 2 blocks
    const auto enc = ops::encode_bc1(src);
    REQUIRE(enc.has_value());
    CHECK(enc->size() == ops::bc1_size(7, 3));
    CHECK(enc->size() == 16u);
}

TEST_CASE("encode_bc1 of a solid colour decodes (RGB565 endpoint) near that colour") {
    ImageBuffer src;
    src.width = 4; src.height = 4; src.channels = 4;
    src.data.assign(4 * 4 * 4, 0);
    for (std::size_t p = 0; p < 16; ++p) {           // solid red, opaque
        src.data[p * 4 + 0] = 200; src.data[p * 4 + 3] = 255;
    }
    const auto enc = ops::encode_bc1(src);
    REQUIRE(enc.has_value());
    REQUIRE(enc->size() == 8u);
    // BC1 block: bytes 0-1 = colour0 as little-endian RGB565.
    const auto b0 = static_cast<unsigned>(std::to_integer<std::uint8_t>((*enc)[0]));
    const auto b1 = static_cast<unsigned>(std::to_integer<std::uint8_t>((*enc)[1]));
    const unsigned c0 = b0 | (b1 << 8);
    const unsigned r5 = (c0 >> 11) & 0x1F;
    const unsigned r8 = (r5 * 255) / 31;
    CHECK(r8 > 150); // endpoint red channel is high
}

TEST_CASE("encode_bc1 rejects a non-RGBA buffer") {
    ImageBuffer rgb; rgb.width = 4; rgb.height = 4; rgb.channels = 3;
    rgb.data.assign(4 * 4 * 3, 0);
    CHECK_FALSE(ops::encode_bc1(rgb).has_value());
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build build/all --config Debug --target image_tests && ./build/all/Debug/image_tests`
Expected: FAIL — `encode_bc1` / `bc1_size` are not declared (compile error).

- [ ] **Step 4: Declare the interface in `image_ops.hpp`**

After the `encode_png` declaration (line ~51), inside `namespace img::ops`, add:
```cpp
#include <cstddef>
#include <vector>
// ... (these includes belong at the top of the file with the others)

// Byte length of BC1/DXT1 data for a w*h image: 8 bytes per 4x4 block.
[[nodiscard]] constexpr std::size_t bc1_size(int w, int h) {
    const std::size_t bx = static_cast<std::size_t>((w + 3) / 4);
    const std::size_t by = static_cast<std::size_t>((h + 3) / 4);
    return bx * by * 8u;
}

// Encode an RGBA8 buffer (channels == 4) to BC1/DXT1 blocks (8 bytes/block, no
// alpha). Returns exactly bc1_size(width, height) bytes. Alpha is ignored.
[[nodiscard]] std::expected<std::vector<std::byte>, ImageError>
encode_bc1(const ImageBuffer &src);
```

- [ ] **Step 5: Implement `encode_bc1` in `image_ops.cpp`**

At the very top of `image_ops.cpp` (after the existing includes, before any function), add the single implementation point and required headers:
```cpp
#include <cstddef>
#include <cstring>
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
```
Then add the function (anywhere in the `img::ops` namespace block):
```cpp
std::expected<std::vector<std::byte>, ImageError> encode_bc1(ImageBuffer const &src) {
    if (!src.valid() || src.channels != 4)
        return std::unexpected(ImageError::EncodeFailed);

    const int W = src.width;
    const int H = src.height;
    const int bx = (W + 3) / 4;
    const int by = (H + 3) / 4;

    std::vector<std::byte> out(bc1_size(W, H));
    std::size_t out_off = 0;

    std::array<unsigned char, 64> block{}; // 4x4 RGBA, edge-replicated
    for (int byi = 0; byi < by; ++byi) {
        for (int bxi = 0; bxi < bx; ++bxi) {
            for (int ry = 0; ry < 4; ++ry) {
                const int sy = std::min(byi * 4 + ry, H - 1); // clamp/replicate edge
                for (int rx = 0; rx < 4; ++rx) {
                    const int sx = std::min(bxi * 4 + rx, W - 1);
                    const std::size_t src_i =
                        (static_cast<std::size_t>(sy) * W + sx) * 4u;
                    const std::size_t dst_i = (static_cast<std::size_t>(ry) * 4 + rx) * 4u;
                    block[dst_i + 0] = src.data[src_i + 0];
                    block[dst_i + 1] = src.data[src_i + 1];
                    block[dst_i + 2] = src.data[src_i + 2];
                    block[dst_i + 3] = 255; // BC1 ignores alpha; keep opaque
                }
            }
            stb_compress_dxt_block(std::bit_cast<unsigned char *>(out.data() + out_off),
                                   block.data(), 0 /*no alpha -> DXT1, 8 bytes*/,
                                   STB_DXT_HIGHQUAL);
            out_off += 8;
        }
    }
    return out;
}
```
Add `#include <array>` and `#include <algorithm>` to the file's includes if not already present.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build/all --config Debug --target image_tests && ./build/all/Debug/image_tests`
Expected: PASS — all `encode_bc1` / `bc1_size` cases green; existing tests still pass.

- [ ] **Step 7: Commit**

```bash
git add thirdparty/stb/stb_dxt.h code/core/image/image_ops.hpp code/core/image/image_ops.cpp tests/image_ops_test.cpp
git commit -m "feat(image): add encode_bc1 (BC1/DXT1) over vendored stb_dxt

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `ThumbnailBlobCache` — single-blob store with index, LRU cap, compaction

**Files:**
- Create: `code/ui/FileExplorer/thumbnail_blob_cache.hpp`
- Create: `code/ui/FileExplorer/thumbnail_blob_cache.cpp`
- Modify: `CMakeLists.txt` (add the new .cpp to the app target sources and, if it has its own test, the test target)
- Test: `tests/thumbnail_blob_cache_test.cpp` (new doctest file) + CMake wiring for a `thumbnail_blob_tests` target mirroring `image_tests`

**Interfaces:**
- Consumes: `img::ops::bc1_size` is NOT needed here (cache is codec-agnostic; it stores opaque byte payloads). Standard library only — this unit is Vulkan-free and pch-free is not required (it lives under `code/ui`), but keep it dependency-light.
- Produces:
  ```cpp
  class ThumbnailBlobCache {
  public:
      struct Stored { std::vector<std::byte> blocks; int w; int h; };

      // Open/create blob + index under `dir`. cap_bytes bounds live payload bytes.
      bool open(const std::filesystem::path &dir, std::uint64_t cap_bytes);
      void close();                       // flushes index to disk

      [[nodiscard]] std::optional<Stored> lookup(std::uint64_t key);  // bumps LRU
      bool store(std::uint64_t key, std::span<const std::byte> blocks, int w, int h);
      void evict(std::uint64_t key);
      void clear();                       // wipe blob + index

      // key = fnv1a(canonical path) ^ mtime ^ size  -> invalidates on file change
      [[nodiscard]] static std::uint64_t make_key(const std::filesystem::path &file);
  };
  ```

- [ ] **Step 1: Write the failing tests**

Create `tests/thumbnail_blob_cache_test.cpp`:
```cpp
#include <doctest.h>
#include <array>
#include <filesystem>
#include "thumbnail_blob_cache.hpp"

namespace {
std::vector<std::byte> blob(std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
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
    REQUIRE(c.store(1, blob({1,1,1,1,1,1,1,1}), 4, 4));
    REQUIRE(c.store(2, blob({2,2,2,2,2,2,2,2}), 4, 4));
    (void)c.lookup(1);                        // 1 is now most-recently-used
    REQUIRE(c.store(3, blob({3,3,3,3,3,3,3,3}), 4, 4)); // over cap -> evict LRU (key 2)
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
        REQUIRE(c.store(5, blob({5,5,5,5,5,5,5,5}), 4, 4));
        c.close();
    }
    std::ofstream(dir / "thumbs.index", std::ios::binary | std::ios::trunc) << "garbage";
    ThumbnailBlobCache c2;
    REQUIRE(c2.open(dir, 1u << 20));
    CHECK_FALSE(c2.lookup(5).has_value()); // discarded, starts empty
    c2.close();
}
```

- [ ] **Step 2: Wire a `thumbnail_blob_tests` CMake target**

In `CMakeLists.txt`, mirror the `image_tests` target block (the doctest-based, no-Vulkan test). Add a target that compiles `tests/thumbnail_blob_cache_test.cpp` + `code/ui/FileExplorer/thumbnail_blob_cache.cpp` with the doctest include and the `code/ui/FileExplorer` + `code/core/image` include dirs. No FFmpeg/Vulkan link needed (the cache is pure std).

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build build/all --config Debug --target thumbnail_blob_tests && ./build/all/Debug/thumbnail_blob_tests`
Expected: FAIL — `thumbnail_blob_cache.hpp` not found / class undefined.

- [ ] **Step 4: Write the header `thumbnail_blob_cache.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Single-blob, indexed thumbnail store. Codec-agnostic: stores opaque byte
// payloads (BC1 blocks) keyed by a 64-bit key. One append-only blob file plus a
// rewritten-on-close index. LRU byte cap with compaction. No Vulkan, no PCH.
class ThumbnailBlobCache {
public:
    struct Stored { std::vector<std::byte> blocks; int w; int h; };

    bool open(const std::filesystem::path &dir, std::uint64_t cap_bytes);
    void close();

    [[nodiscard]] std::optional<Stored> lookup(std::uint64_t key);
    bool store(std::uint64_t key, std::span<const std::byte> blocks, int w, int h);
    void evict(std::uint64_t key);
    void clear();

    [[nodiscard]] static std::uint64_t make_key(const std::filesystem::path &file);

private:
    struct Record {
        std::uint64_t offset = 0;
        std::uint32_t length = 0;
        std::uint16_t w = 0;
        std::uint16_t h = 0;
        std::uint64_t last_access = 0; // monotonic tick
        bool          dead = false;
    };

    static constexpr char         k_magic[4] = {'T', 'B', 'C', '1'};
    static constexpr std::uint32_t k_version = 1;

    std::filesystem::path blob_path() const { return m_dir / "thumbs.bc1blob"; }
    std::filesystem::path index_path() const { return m_dir / "thumbs.index"; }

    bool load_index();      // false -> caller wipes + starts empty
    void save_index() const;
    void enforce_cap();
    void compact();

    std::filesystem::path                         m_dir;
    std::uint64_t                                 m_cap_bytes = 0;
    std::uint64_t                                 m_live_bytes = 0;
    std::uint64_t                                 m_dead_bytes = 0;
    std::uint64_t                                 m_tick = 0;
    std::unordered_map<std::uint64_t, Record>     m_index;
    bool                                          m_open = false;
};
```

- [ ] **Step 5: Implement `thumbnail_blob_cache.cpp`**

```cpp
#include "thumbnail_blob_cache.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace {
std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
} // namespace

std::uint64_t ThumbnailBlobCache::make_key(const std::filesystem::path &file) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(file, ec).string();
    std::uint64_t key = fnv1a(canonical);
    const auto sz = std::filesystem::file_size(file, ec);
    if (!ec) key ^= sz * 1099511628211ull;
    const auto wt = std::filesystem::last_write_time(file, ec);
    if (!ec) key ^= static_cast<std::uint64_t>(wt.time_since_epoch().count());
    return key;
}

bool ThumbnailBlobCache::open(const std::filesystem::path &dir, std::uint64_t cap_bytes) {
    m_dir = dir;
    m_cap_bytes = cap_bytes;
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    m_index.clear();
    m_live_bytes = m_dead_bytes = m_tick = 0;
    if (!load_index()) {            // corrupt/missing -> wipe and start empty
        std::filesystem::remove(blob_path(), ec);
        std::filesystem::remove(index_path(), ec);
        m_index.clear();
        m_live_bytes = m_dead_bytes = 0;
    }
    m_open = true;
    return true;
}

void ThumbnailBlobCache::close() {
    if (!m_open) return;
    save_index();
    m_open = false;
}

std::optional<ThumbnailBlobCache::Stored> ThumbnailBlobCache::lookup(std::uint64_t key) {
    auto it = m_index.find(key);
    if (it == m_index.end() || it->second.dead) return std::nullopt;
    Record &r = it->second;
    std::ifstream f(blob_path(), std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    f.seekg(static_cast<std::streamoff>(r.offset));
    Stored out;
    out.blocks.resize(r.length);
    f.read(std::bit_cast<char *>(out.blocks.data()), r.length);
    if (f.gcount() != static_cast<std::streamsize>(r.length)) return std::nullopt;
    out.w = r.w; out.h = r.h;
    r.last_access = ++m_tick;
    return out;
}

bool ThumbnailBlobCache::store(std::uint64_t key, std::span<const std::byte> blocks,
                               int w, int h) {
    if (blocks.empty()) return false;
    std::ofstream f(blob_path(), std::ios::binary | std::ios::app);
    if (!f.is_open()) return false;
    const std::uint64_t offset = static_cast<std::uint64_t>(f.tellp());
    f.write(std::bit_cast<const char *>(blocks.data()),
            static_cast<std::streamsize>(blocks.size()));
    if (!f.good()) return false;

    Record r;
    r.offset = offset;
    r.length = static_cast<std::uint32_t>(blocks.size());
    r.w = static_cast<std::uint16_t>(w);
    r.h = static_cast<std::uint16_t>(h);
    r.last_access = ++m_tick;
    m_index[key] = r;
    m_live_bytes += r.length;
    enforce_cap();
    return true;
}

void ThumbnailBlobCache::evict(std::uint64_t key) {
    auto it = m_index.find(key);
    if (it == m_index.end() || it->second.dead) return;
    it->second.dead = true;
    m_live_bytes -= it->second.length;
    m_dead_bytes += it->second.length;
}

void ThumbnailBlobCache::clear() {
    std::error_code ec;
    std::filesystem::remove(blob_path(), ec);
    std::filesystem::remove(index_path(), ec);
    m_index.clear();
    m_live_bytes = m_dead_bytes = m_tick = 0;
}

void ThumbnailBlobCache::enforce_cap() {
    while (m_live_bytes > m_cap_bytes) {
        // find live entry with smallest last_access (LRU)
        auto lru = m_index.end();
        for (auto it = m_index.begin(); it != m_index.end(); ++it) {
            if (it->second.dead) continue;
            if (lru == m_index.end() || it->second.last_access < lru->second.last_access)
                lru = it;
        }
        if (lru == m_index.end()) break;
        evict(lru->first);
    }
    if (m_dead_bytes > m_live_bytes / 4 + 1024 * 1024) compact();
}

void ThumbnailBlobCache::compact() {
    const auto tmp = m_dir / "thumbs.bc1blob.tmp";
    std::ifstream in(blob_path(), std::ios::binary);
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!in.is_open() || !out.is_open()) return;
    std::uint64_t new_off = 0;
    std::vector<char> buf;
    for (auto it = m_index.begin(); it != m_index.end();) {
        if (it->second.dead) { it = m_index.erase(it); continue; }
        Record &r = it->second;
        buf.resize(r.length);
        in.seekg(static_cast<std::streamoff>(r.offset));
        in.read(buf.data(), r.length);
        out.write(buf.data(), r.length);
        r.offset = new_off;
        new_off += r.length;
        ++it;
    }
    in.close();
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmp, blob_path(), ec);
    m_dead_bytes = 0;
    save_index();
}

bool ThumbnailBlobCache::load_index() {
    std::ifstream f(index_path(), std::ios::binary);
    if (!f.is_open()) return true; // no index yet = empty, valid
    char magic[4];
    std::uint32_t version = 0, count = 0;
    f.read(magic, 4);
    f.read(std::bit_cast<char *>(&version), sizeof version);
    f.read(std::bit_cast<char *>(&count), sizeof count);
    if (!f.good() || std::memcmp(magic, k_magic, 4) != 0 || version != k_version)
        return false; // corrupt -> caller wipes
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t key = 0;
        Record r;
        f.read(std::bit_cast<char *>(&key), sizeof key);
        f.read(std::bit_cast<char *>(&r.offset), sizeof r.offset);
        f.read(std::bit_cast<char *>(&r.length), sizeof r.length);
        f.read(std::bit_cast<char *>(&r.w), sizeof r.w);
        f.read(std::bit_cast<char *>(&r.h), sizeof r.h);
        f.read(std::bit_cast<char *>(&r.last_access), sizeof r.last_access);
        if (!f.good()) return false;
        m_index[key] = r;
        m_live_bytes += r.length;
        m_tick = std::max(m_tick, r.last_access);
    }
    return true;
}

void ThumbnailBlobCache::save_index() const {
    std::ofstream f(index_path(), std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return;
    std::uint32_t count = 0;
    for (const auto &[k, r] : m_index) if (!r.dead) ++count;
    f.write(k_magic, 4);
    f.write(std::bit_cast<const char *>(&k_version), sizeof k_version);
    f.write(std::bit_cast<const char *>(&count), sizeof count);
    for (const auto &[key, r] : m_index) {
        if (r.dead) continue;
        f.write(std::bit_cast<const char *>(&key), sizeof key);
        f.write(std::bit_cast<const char *>(&r.offset), sizeof r.offset);
        f.write(std::bit_cast<const char *>(&r.length), sizeof r.length);
        f.write(std::bit_cast<const char *>(&r.w), sizeof r.w);
        f.write(std::bit_cast<const char *>(&r.h), sizeof r.h);
        f.write(std::bit_cast<const char *>(&r.last_access), sizeof r.last_access);
    }
}
```
Add `#include <string_view>` and `#include <bit>` to the .cpp includes.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build/all --config Debug --target thumbnail_blob_tests && ./build/all/Debug/thumbnail_blob_tests`
Expected: PASS — round-trip, persistence, LRU eviction, corrupt-rebuild all green.

- [ ] **Step 7: Commit**

```bash
git add code/ui/FileExplorer/thumbnail_blob_cache.hpp code/ui/FileExplorer/thumbnail_blob_cache.cpp tests/thumbnail_blob_cache_test.cpp CMakeLists.txt
git commit -m "feat(filebrowser): add ThumbnailBlobCache (single-blob BC1 store, LRU, compaction)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Enable `textureCompressionBC` + `VulkanTexture::upload_bc1`

**Files:**
- Modify: `code/rendering/vulkan/vulkan_context.hpp` (add `bool bc_textures_enabled = false;` public field near `placebo_features_enabled`, line ~30)
- Modify: `code/rendering/vulkan/vulkan_context.cpp` (query + enable `textureCompressionBC` in the device-create block, line ~218-240)
- Modify: `code/rendering/vulkan/vulkan_texture.hpp` (declare `upload_bc1`)
- Modify: `code/rendering/vulkan/vulkan_texture.cpp` (implement `upload_bc1`)

**Interfaces:**
- Consumes: `vulkan_context` (`device`, `physical_device`, `allocator`, `queue_submit`, `main_window_data`), the existing `find_memory_type`, the staging/transfer pattern from `upload_pixels`.
- Produces:
  ```cpp
  // Upload BC1/DXT1 block data into a VK_FORMAT_BC1_RGBA_UNORM_BLOCK image and
  // register it with ImGui. `blocks.size()` must equal ceil(w/4)*ceil(h/4)*8.
  bool VulkanTexture::upload_bc1(std::span<const std::byte> blocks, int w, int h,
                                 vulkan_context &vk);
  // vulkan_context::bc_textures_enabled — true iff the device exposes textureCompressionBC.
  ```

- [ ] **Step 1: Enable the device feature**

In `vulkan_context.cpp`, in the device-create block (around line 218), extend the existing `VkPhysicalDeviceFeatures2` query to also read base features, and enable BC if supported. Replace the query/enable region with:
```cpp
VkPhysicalDeviceVulkan12Features vk12_supported = {};
vk12_supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
VkPhysicalDeviceFeatures2 features_query = {};
features_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
features_query.pNext = &vk12_supported;
vkGetPhysicalDeviceFeatures2(physical_device, &features_query);

bc_textures_enabled = features_query.features.textureCompressionBC == VK_TRUE;

// Base features to enable (BC textures). Enabled via VkPhysicalDeviceFeatures2
// chained into pNext; pEnabledFeatures stays null.
static VkPhysicalDeviceFeatures2 features_enable = {};
features_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
if (bc_textures_enabled) {
    features_enable.features.textureCompressionBC = VK_TRUE;
    APP_DEBUG_LOG("[vulkan_context] textureCompressionBC enabled");
} else {
    APP_DEBUG_LOG("[vulkan_context] textureCompressionBC unavailable — BC1 thumbnails disabled");
}

VkPhysicalDeviceVulkan12Features vk12_enable = {};
vk12_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
if (vk12_supported.hostQueryReset && vk12_supported.timelineSemaphore) {
    vk12_enable.hostQueryReset = VK_TRUE;
    vk12_enable.timelineSemaphore = VK_TRUE;
    placebo_features_enabled = true;
    APP_DEBUG_LOG("[vulkan_context] libplacebo features enabled (hostQueryReset + timelineSemaphore)");
} else {
    APP_DEBUG_LOG("[vulkan_context] libplacebo features unavailable — zero-copy video path disabled");
}
features_enable.pNext = &vk12_enable;          // chain vk12 features after base
create_info.pNext = &features_enable;          // pEnabledFeatures MUST stay null
```
Add `bool bc_textures_enabled = false;` to `vulkan_context.hpp` public members near line 30.

- [ ] **Step 2: Declare `upload_bc1` in `vulkan_texture.hpp`**

After the `upload(...)` declaration (line ~32):
```cpp
// Upload already-compressed BC1/DXT1 blocks (no decode). blocks.size() must be
// ceil(w/4)*ceil(h/4)*8. Returns false on size mismatch or any Vulkan failure.
bool upload_bc1(std::span<const std::byte> blocks, int w, int h, vulkan_context &vk);
```
Add `#include <span>` and `#include <cstddef>` to the header includes.

- [ ] **Step 3: Implement `upload_bc1` in `vulkan_texture.cpp`**

Add (modeled on `upload_pixels`, but compressed format + block-sized staging, no per-texel size):
```cpp
bool VulkanTexture::upload_bc1(std::span<const std::byte> blocks, int w, int h,
                               vulkan_context &vk) {
    if (w <= 0 || h <= 0) return false;
    const std::size_t bx = static_cast<std::size_t>((w + 3) / 4);
    const std::size_t by = static_cast<std::size_t>((h + 3) / 4);
    if (blocks.size() != bx * by * 8u) return false;

    width = w;
    height = h;
    const VkDeviceSize image_size = static_cast<VkDeviceSize>(blocks.size());

    // 1. Compressed image (extent is texel dims; driver rounds to 4x4 blocks).
    {
        VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vk.device, &info, vk.allocator, &m_image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, m_image, &req);
        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == 0xFFFFFFFFu) return false;
        vulkan_context::check_result(vkAllocateMemory(vk.device, &alloc, vk.allocator, &m_image_memory));
        vkBindImageMemory(vk.device, m_image, m_image_memory, 0);
    }

    // 2. View (format must match) + ImGui registration.
    {
        VkImageViewCreateInfo v = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = m_image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &v, vk.allocator, &m_image_view);
    }
    m_descriptor_set = ImGui_ImplVulkan_AddTexture(m_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 3. Staging buffer holds the raw blocks.
    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    {
        VkBufferCreateInfo b = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b.size = image_size;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(vk.device, &b, vk.allocator, &staging_buf);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.device, staging_buf, &req);
        VkMemoryAllocateInfo a = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(vk.device, &a, vk.allocator, &staging_mem);
        vkBindBufferMemory(vk.device, staging_buf, staging_mem, 0);
        void *map_ptr;
        vkMapMemory(vk.device, staging_mem, 0, image_size, 0, &map_ptr);
        std::memcpy(map_ptr, blocks.data(), static_cast<size_t>(image_size));
        vkUnmapMemory(vk.device, staging_mem);
    }

    // 4. Transfer (identical barriers to upload_pixels; copy extent is texel dims).
    {
        VkCommandBufferAllocateInfo c = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        c.commandPool = vk.main_window_data.Frames[static_cast<int>(vk.main_window_data.FrameIndex)].CommandPool;
        c.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vk.device, &c, &cmd);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = m_image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(cmd, staging_buf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(cmd);

        VkFence fence;
        VkFenceCreateInfo f = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(vk.device, &f, vk.allocator, &fence);
        VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1;
        s.pCommandBuffers = &cmd;
        vk.queue_submit(1, &s, fence);
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk.device, fence, vk.allocator);
        vkFreeCommandBuffers(vk.device, c.commandPool, 1, &cmd);
    }

    vkDestroyBuffer(vk.device, staging_buf, vk.allocator);
    vkFreeMemory(vk.device, staging_mem, vk.allocator);
    return true;
}
```

- [ ] **Step 4: Build the app to verify it compiles and runs**

Run: `cmake --build build/all --config Debug --target <app-target>`
Expected: links clean. Launch the app; the debug log prints `textureCompressionBC enabled` on an NVIDIA/desktop GPU. (No behavioural change yet — `upload_bc1` is not called until Task 4.)

- [ ] **Step 5: Commit**

```bash
git add code/rendering/vulkan/vulkan_context.hpp code/rendering/vulkan/vulkan_context.cpp code/rendering/vulkan/vulkan_texture.hpp code/rendering/vulkan/vulkan_texture.cpp
git commit -m "feat(vulkan): enable textureCompressionBC + VulkanTexture::upload_bc1

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Wire the BC1 backend into `FileThumbnailCache` (+ config toggle, fallback)

**Files:**
- Modify: `code/ui/FileExplorer/file_thumbnail_cache.hpp` (backend mode, blob cache member, key helper)
- Modify: `code/ui/FileExplorer/file_thumbnail_cache.cpp` (setup chooses backend; get()/generate paths; evict/clear delegate)
- Modify: `code/ui/FileExplorer/thumbnail_generator.{hpp,cpp}` (BC1 generation: bake opaque letterbox, encode BC1, return blocks+dims)
- Modify: `code/ui/config/config_runtime.{hpp,cpp}` (read `thumbnails.format` + `thumbnails.cache_cap_mb`)

**Interfaces:**
- Consumes: `ThumbnailBlobCache` (Task 2), `VulkanTexture::upload_bc1` + `vulkan_context::bc_textures_enabled` (Task 3), `img::ops::encode_bc1` (Task 1).
- Produces: an internal `enum class Backend { Png, Bc1 }` selected at `setup()`; no new public API on `FileThumbnailCache` (callers keep using `get()`).

- [ ] **Step 1: Add config knobs**

In `config_runtime.{hpp,cpp}`, add two settings read from the existing config source (mirror an existing string + int setting):
- `std::string thumbnails_format = "bc1";` (values `"bc1"` | `"png"`)
- `int thumbnails_cache_cap_mb = 256;`
Expose getters consistent with the file's existing accessor style.

- [ ] **Step 2: Extend `FileThumbnailCache` state**

In `file_thumbnail_cache.hpp`:
```cpp
#include "thumbnail_blob_cache.hpp"
// ...
enum class Backend { Png, Bc1 };
// in the Entry struct add:
std::vector<std::byte> bc1_blocks; // produced by generator (Bc1 backend), consumed on render thread
int bc1_w = 0, bc1_h = 0;
// in the class private section add:
Backend            m_backend = Backend::Png;
ThumbnailBlobCache m_blob;
```
Add `setup()` parameters or read config inside `setup()` (match how the class currently gets its dir). Choose the backend:
```cpp
// in setup(), after m_vk is stored:
const bool want_bc1 = (config.thumbnails_format() == "bc1");
m_backend = (want_bc1 && vk->bc_textures_enabled) ? Backend::Bc1 : Backend::Png;
if (m_backend == Backend::Bc1)
    m_blob.open(thumb_dir / "bc1", static_cast<std::uint64_t>(config.thumbnails_cache_cap_mb()) * 1024ull * 1024ull);
```
(If `bc_textures_enabled` is false, this silently stays on Png — the automatic fallback.)

- [ ] **Step 3: BC1 generation path in `ThumbnailGenerator`**

Add a sibling to `generate()` that returns BC1 blocks instead of writing a PNG:
```cpp
// thumbnail_generator.hpp
struct Bc1Result { std::vector<std::byte> blocks; int w; int h; };
[[nodiscard]] static std::expected<Bc1Result, img::ImageError>
generate_bc1(std::filesystem::path const &source, bool is_video, int w, int h);
```
```cpp
// thumbnail_generator.cpp — reuse the existing decode + letterbox_fit, then:
// 1. After letterbox_fit returns RGBA with TRANSPARENT padding, bake the padding
//    to opaque black so BC1 (which has no alpha) shows solid bars, not garbage:
for (std::size_t p = 0; p + 3 < rz->data.size(); p += 4) {
    if (rz->data[p + 3] == 0) { rz->data[p+0] = rz->data[p+1] = rz->data[p+2] = 0; }
    rz->data[p + 3] = 255;
}
// 2. Encode:
auto enc = img::ops::encode_bc1(*rz);
if (!enc) return std::unexpected(enc.error());
return Bc1Result{std::move(*enc), rz->width, rz->height};
```

- [ ] **Step 4: Route `get()` / generator completion / evict / clear through the backend**

In `file_thumbnail_cache.cpp`, in `Backend::Bc1` mode:
- `get()` cache check: instead of `png_for`/`exists`, compute `const std::uint64_t key = ThumbnailBlobCache::make_key(path);` and `auto stored = m_blob.lookup(key);`. If present → `entry.bc1_blocks = std::move(stored->blocks); entry.bc1_w = stored->w; entry.bc1_h = stored->h; entry.state = DiskReady;`.
- Generator thread (`generate_thumbnail`): call `ThumbnailGenerator::generate_bc1(...)`; on success, stash `bc1_blocks`/`bc1_w`/`bc1_h` into the entry and set state to `DiskReady` (it does NOT write to the blob — the render thread does, preserving single-writer). On failure set `Failed`.
- Render-thread upload (the `DiskReady` branch): replace `texture->load(png)` with:
  ```cpp
  entry.texture = std::make_unique<VulkanTexture>();
  if (entry.texture->upload_bc1(entry.bc1_blocks, entry.bc1_w, entry.bc1_h, *m_vk)) {
      m_blob.store(ThumbnailBlobCache::make_key(path), entry.bc1_blocks, entry.bc1_w, entry.bc1_h);
      entry.bc1_blocks.clear();
      entry.state = State::Ready;
  } else { entry.state = State::Failed; entry.texture.reset(); }
  ```
  (When the blocks came from `m_blob.lookup`, calling `store` again is redundant; guard with a flag or skip store when the entry was loaded from the blob — track with a bool `from_blob` on the entry.)
- `evict(path)` → `m_blob.evict(make_key(path));` `clear()` → `m_blob.clear();` in Bc1 mode; keep the PNG branches for Png mode.
- `shutdown()` → `m_blob.close();` in Bc1 mode (flushes the index).

Keep every existing `Backend::Png` code path untouched and selected when `m_backend == Backend::Png`.

- [ ] **Step 5: Build and verify in the running app**

Run: `cmake --build build/all --config Debug --target <app-target>` then launch and open a folder of images/videos.
Expected: thumbnails render identically (opaque black letterbox bars). After first view, `<thumb_dir>/bc1/thumbs.bc1blob` exists and grows; the per-file PNGs are no longer created in bc1 mode. Re-opening the folder loads thumbnails from the blob with no regeneration (cache hit, no decode).

- [ ] **Step 6: Verify fallback + disk win**

- Set `thumbnails.format = png` in config → app uses the old PNG path unchanged.
- Compare `du -sh` of the old PNG cache dir vs the new `bc1/thumbs.bc1blob` for the same folder: expect roughly 5× smaller and a single file instead of thousands.

- [ ] **Step 7: Commit**

```bash
git add code/ui/FileExplorer/file_thumbnail_cache.hpp code/ui/FileExplorer/file_thumbnail_cache.cpp code/ui/FileExplorer/thumbnail_generator.hpp code/ui/FileExplorer/thumbnail_generator.cpp code/ui/config/config_runtime.hpp code/ui/config/config_runtime.cpp
git commit -m "feat(filebrowser): optional BC1 binary thumbnail backend with png fallback

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Notes / deviations from the spec

- **Alpha:** the spec described BC1 "1-bit punch-through alpha." The vendored `stb_dxt` encoder does **not** expose BC1 punch-through (it does DXT1-no-alpha or DXT5-8bit-alpha only). The plan therefore **bakes the transparent letterbox to opaque black** before BC1 encoding (Task 4, Step 3). Net visible change: letterbox bars are solid black instead of transparent. If transparent bars turn out to be required, the fallback is BC3/DXT5 (4:1 instead of 8:1) via `stb_compress_dxt_block(..., alpha=1, ...)` and `VK_FORMAT_BC3_UNORM_BLOCK`.
- **mmap:** the spec mentioned mmap'ing the blob for zero-copy reads. The plan uses buffered `ifstream` slice reads (a 28 KB read into the staging copy you already make) to stay portable and testable; mmap is a safe later optimization that does not change the interface.
```
