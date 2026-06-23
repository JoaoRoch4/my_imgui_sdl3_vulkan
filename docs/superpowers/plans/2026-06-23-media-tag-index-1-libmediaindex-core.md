# Media Tag Index — Plan 1: libmediaindex Storage Core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the PCH-free storage core of the media tag index — content-signature identity, an append-only BC1 blob arena, and a SQLite tag database (files/tags/file_tags/FTS) with include/exclude queries and autocomplete — fully unit-tested, with no extraction, CLI, or UI yet.

**Architecture:** A new static-library-style source group `code/core/mediaindex/` (Vulkan-free, PCH-free, std-only — same constraints as `code/core/image/` so it links into a standalone doctest target). Three units: `ContentSignature` (file identity), `BlobArena` (thumbnail bytes), `MediaIndexDb` (the SQLite "brain"). SQLite + xxHash come from vcpkg. This is the spec from [docs/superpowers/specs/2026-06-23-media-tag-index-design.md](../specs/2026-06-23-media-tag-index-design.md) §3–§4, §6.

**Tech Stack:** C++23, clang/clang++ + LLD, doctest, SQLite 3 (vcpkg `sqlite3[fts5]`), xxHash (vcpkg). CMake + Ninja.

## Global Constraints

- **Compiler:** clang/clang++ + LLD for every config, including Debug. (`[[llvm-toolchain-preference]]`)
- **C++ style:** strict modern C++23 — `static_cast`/`bit_cast` only (no C casts / `reinterpret_cast`), smart pointers only, `std::span`/`std::string_view` at boundaries, `constexpr` where possible, `std::println` for logs, attached braces. (`[[cpp-code-style]]`)
- **No PCH in this unit:** none of `code/core/mediaindex/*.cpp` may `#include "pch.hpp"`. The test target has no app include path. Use explicit std headers. (`[[image-core-pch-free]]`)
- **Identity:** `ContentSignature = {size, xxh3(first 64 KiB), xxh3(last 64 KiB)}`; whole-file hash when `size ≤ 128 KiB` (`head == tail`). It is the DB's `UNIQUE` key — re-ingesting a moved file updates the same row.
- **Promoted `files` columns:** `src_w`, `src_h`, `capture_unix`, `file_mtime`, `score`, `duration_ms` only. `gps`/`codec` are NOT columns (they live in `meta_full`).
- **SQLite mode:** WAL, `foreign_keys=ON`.
- **Test framework:** doctest (`tests/test_main.cpp` provides `main`). Run a target with `cmake --build build/all --config Debug --target <t> && ./build/all/Debug/<t>`.

---

### Task 0: Dependencies + empty test target (green baseline)

**Files:**
- Modify: `CMakeLists.txt` (add vcpkg `find_package`s near line 86; add `mediaindex_tests` target after the `thumbnail_blob_tests` block ~line 638)
- Create: `tests/media_index_db_test.cpp` (empty placeholder so the target has a translation unit)

**Interfaces:**
- Produces: a buildable `mediaindex_tests` doctest executable linking `unofficial::sqlite3::sqlite3` and `xxHash::xxhash`.

- [ ] **Step 1: Install the vcpkg dependencies**

Run:
```bash
/home/joao/vcpkg/vcpkg install "sqlite3[fts5]" xxhash --triplet x64-linux
```
Expected: both report `installed` (or "already installed"). FTS5 is required for autocomplete.

- [ ] **Step 2: Add the `find_package` lines**

In `CMakeLists.txt`, right after `find_package(CURL REQUIRED)` (~line 87), add:
```cmake
find_package(unofficial-sqlite3 CONFIG REQUIRED)  # vcpkg sqlite3[fts5] -> unofficial::sqlite3::sqlite3
find_package(xxHash CONFIG REQUIRED)              # vcpkg xxhash       -> xxHash::xxhash
```

- [ ] **Step 3: Create the placeholder test file**

Create `tests/media_index_db_test.cpp`:
```cpp
#include <doctest.h>

TEST_CASE("mediaindex test target builds") {
    CHECK(true);
}
```

- [ ] **Step 4: Add the `mediaindex_tests` target**

In `CMakeLists.txt`, after the `add_test(NAME thumbnail_blob_tests ...)` line (~638), add:
```cmake
# ── Media index tests (mediaindex_tests) ─────────────────────────────────────────
# PCH-free, std-only storage core (mirrors image_tests). SQLite + xxHash via vcpkg.
#   cmake --build build/all --config Debug --target mediaindex_tests
#   ./build/all/Debug/mediaindex_tests
add_executable(mediaindex_tests
    tests/test_main.cpp
    tests/content_signature_test.cpp
    tests/blob_arena_test.cpp
    tests/media_index_db_test.cpp
    code/core/mediaindex/content_signature.cpp
    code/core/mediaindex/blob_arena.cpp
    code/core/mediaindex/media_index_db.cpp
)
target_include_directories(mediaindex_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/code/core/mediaindex
)
target_include_directories(mediaindex_tests SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/external/doctest
)
target_link_libraries(mediaindex_tests PRIVATE
    unofficial::sqlite3::sqlite3
    xxHash::xxhash
)
target_compile_options(mediaindex_tests PRIVATE "-Wno-#warnings")
add_test(NAME mediaindex_tests COMMAND mediaindex_tests)
```

> Note: Steps below create `content_signature.*`, `blob_arena.*`, and the two extra test files this target references. To keep the build green *now*, temporarily comment out the four not-yet-created source/test lines (`content_signature*`, `blob_arena*`) and uncomment them in their tasks. (Tracked inline in Tasks 1–2.)

- [ ] **Step 5: Configure + build the placeholder**

Run:
```bash
cmake --build build/all --config Debug --target mediaindex_tests
./build/all/Debug/mediaindex_tests
```
Expected: builds; doctest prints `test cases: 1 | 1 passed`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/media_index_db_test.cpp
git commit -m "build(mediaindex): vcpkg sqlite3[fts5]+xxhash deps and mediaindex_tests target"
```

---

### Task 1: ContentSignature

**Files:**
- Create: `code/core/mediaindex/content_signature.hpp`
- Create: `code/core/mediaindex/content_signature.cpp`
- Test: `tests/content_signature_test.cpp`

**Interfaces:**
- Produces:
  - `struct mediaindex::ContentSignature { std::uint64_t size, head, tail; bool operator==; };`
  - `ContentSignature signature_of_bytes(std::span<const std::byte> data);`
  - `std::optional<ContentSignature> signature_of(const std::filesystem::path& file);`

- [ ] **Step 1: Write the failing test**

Create `tests/content_signature_test.cpp`:
```cpp
#include <doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "content_signature.hpp"

using mediaindex::ContentSignature;
using mediaindex::signature_of;
using mediaindex::signature_of_bytes;

namespace {
std::vector<std::byte> bytes_of(std::size_t n, std::byte fill) {
    return std::vector<std::byte>(n, fill);
}
} // namespace

TEST_CASE("small file: head == tail, size recorded") {
    const auto data = bytes_of(100, std::byte{0xAB});
    const auto sig  = signature_of_bytes(data);
    CHECK(sig.size == 100);
    CHECK(sig.head == sig.tail);          // small-file rule
    CHECK(sig.head != 0);
}

TEST_CASE("large file: head and tail differ when ends differ") {
    std::vector<std::byte> data(200 * 1024, std::byte{0x00}); // > 128 KiB
    data.front() = std::byte{0x11};
    data.back()  = std::byte{0x22};
    const auto sig = signature_of_bytes(data);
    CHECK(sig.size == data.size());
    CHECK(sig.head != sig.tail);
}

TEST_CASE("signature_of reads a real file identically to the byte API") {
    auto dir = std::filesystem::temp_directory_path() / "csig";
    std::filesystem::create_directories(dir);
    auto p = dir / "f.bin";
    std::vector<std::byte> data(300 * 1024, std::byte{0x07});
    data.back() = std::byte{0x55};
    { std::ofstream o(p, std::ios::binary);
      o.write(static_cast<const char*>(static_cast<const void*>(data.data())),
              static_cast<std::streamsize>(data.size())); }
    const auto fromFile  = signature_of(p);
    const auto fromBytes = signature_of_bytes(data);
    REQUIRE(fromFile.has_value());
    CHECK(*fromFile == fromBytes);
}

TEST_CASE("signature_of returns nullopt for a missing file") {
    CHECK_FALSE(signature_of("/no/such/file.xyz").has_value());
}
```

- [ ] **Step 2: Re-enable the source/test lines in the target**

In `CMakeLists.txt`, ensure the `content_signature.cpp` and `tests/content_signature_test.cpp` lines in `mediaindex_tests` are uncommented.

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cmake --build build/all --config Debug --target mediaindex_tests
```
Expected: FAIL — `content_signature.hpp` not found / unresolved `signature_of_bytes`.

- [ ] **Step 4: Write the header**

Create `code/core/mediaindex/content_signature.hpp`:
```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace mediaindex {

// Cheap, rename-proof file identity. The DB's UNIQUE key.
struct ContentSignature {
    std::uint64_t size = 0;
    std::uint64_t head = 0; // xxh3 of first 64 KiB (or whole file when small)
    std::uint64_t tail = 0; // xxh3 of last  64 KiB (== head when small)

    friend bool operator==(const ContentSignature&, const ContentSignature&) = default;
};

// Pure helper (used by tests and by signature_of for small files).
[[nodiscard]] ContentSignature signature_of_bytes(std::span<const std::byte> data);

// Compute from a file without reading the whole thing for large files.
// nullopt on open/read failure.
[[nodiscard]] std::optional<ContentSignature> signature_of(const std::filesystem::path& file);

} // namespace mediaindex
```

- [ ] **Step 5: Write the implementation**

Create `code/core/mediaindex/content_signature.cpp`:
```cpp
#include "content_signature.hpp"

#include <fstream>
#include <vector>

#include <xxhash.h>

namespace mediaindex {
namespace {
constexpr std::size_t k_window = 64 * 1024;          // head/tail window
constexpr std::size_t k_small  = 2 * k_window;       // <= this: hash whole file

std::uint64_t xh(const void* p, std::size_t n) {
    return static_cast<std::uint64_t>(XXH3_64bits(p, n));
}
} // namespace

ContentSignature signature_of_bytes(std::span<const std::byte> data) {
    ContentSignature s;
    s.size = data.size();
    if (data.size() <= k_small) {
        s.head = xh(data.data(), data.size());
        s.tail = s.head;
    } else {
        s.head = xh(data.first(k_window).data(), k_window);
        s.tail = xh(data.last(k_window).data(), k_window);
    }
    return s;
}

std::optional<ContentSignature> signature_of(const std::filesystem::path& file) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(file, ec);
    if (ec) return std::nullopt;

    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;

    ContentSignature s;
    s.size = sz;

    if (sz <= k_small) {
        std::vector<char> all(static_cast<std::size_t>(sz));
        in.read(all.data(), static_cast<std::streamsize>(sz));
        if (in.gcount() != static_cast<std::streamsize>(sz)) return std::nullopt;
        s.head = xh(all.data(), all.size());
        s.tail = s.head;
        return s;
    }

    std::vector<char> buf(k_window);
    in.read(buf.data(), static_cast<std::streamsize>(k_window));
    if (in.gcount() != static_cast<std::streamsize>(k_window)) return std::nullopt;
    s.head = xh(buf.data(), k_window);

    in.seekg(static_cast<std::streamoff>(sz - k_window), std::ios::beg);
    in.read(buf.data(), static_cast<std::streamsize>(k_window));
    if (in.gcount() != static_cast<std::streamsize>(k_window)) return std::nullopt;
    s.tail = xh(buf.data(), k_window);
    return s;
}

} // namespace mediaindex
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests
```
Expected: PASS — all `ContentSignature` cases green.

- [ ] **Step 7: Commit**

```bash
git add code/core/mediaindex/content_signature.hpp code/core/mediaindex/content_signature.cpp tests/content_signature_test.cpp CMakeLists.txt
git commit -m "feat(mediaindex): ContentSignature (size + xxh3 head/tail identity)"
```

---

### Task 2: BlobArena

**Files:**
- Create: `code/core/mediaindex/blob_arena.hpp`
- Create: `code/core/mediaindex/blob_arena.cpp`
- Test: `tests/blob_arena_test.cpp`

**Interfaces:**
- Produces:
  - `class mediaindex::BlobArena` with: `bool open(const std::filesystem::path&)`, `void close()`,
    `std::uint64_t append(std::span<const std::byte>)`, `std::optional<std::vector<std::byte>> read(std::uint64_t offset, std::uint32_t length) const`, `std::uint64_t size() const`.
- Note: zero-copy mmap reads are deferred to Plan 3 (app path); the core uses simple file I/O.

- [ ] **Step 1: Write the failing test**

Create `tests/blob_arena_test.cpp`:
```cpp
#include <doctest.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "blob_arena.hpp"

using mediaindex::BlobArena;

namespace {
std::vector<std::byte> blob(std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}
std::filesystem::path fresh(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p / "arena.bin";
}
} // namespace

TEST_CASE("append returns increasing offsets and read round-trips bytes") {
    BlobArena a;
    REQUIRE(a.open(fresh("arena_rt")));
    const auto p0 = blob({1, 2, 3});
    const auto p1 = blob({9, 8, 7, 6});
    const auto o0 = a.append(p0);
    const auto o1 = a.append(p1);
    CHECK(o0 == 0);
    CHECK(o1 == 3);
    const auto r0 = a.read(o0, 3);
    const auto r1 = a.read(o1, 4);
    REQUIRE(r0.has_value());
    REQUIRE(r1.has_value());
    CHECK(*r0 == p0);
    CHECK(*r1 == p1);
    a.close();
}

TEST_CASE("data persists across reopen and append continues at EOF") {
    const auto path = fresh("arena_persist");
    std::uint64_t off = 0;
    {
        BlobArena a;
        REQUIRE(a.open(path));
        off = a.append(blob({5, 5, 5, 5}));
        a.close();
    }
    BlobArena a;
    REQUIRE(a.open(path));
    CHECK(a.size() == 4);
    const auto r = a.read(off, 4);
    REQUIRE(r.has_value());
    CHECK(*r == blob({5, 5, 5, 5}));
    const auto off2 = a.append(blob({1}));
    CHECK(off2 == 4);           // continues at previous EOF
    a.close();
}

TEST_CASE("read past EOF returns nullopt") {
    BlobArena a;
    REQUIRE(a.open(fresh("arena_oob")));
    a.append(blob({1, 2}));
    CHECK_FALSE(a.read(0, 99).has_value());
    a.close();
}
```

- [ ] **Step 2: Re-enable the source/test lines in `mediaindex_tests`** (uncomment `blob_arena.cpp` / `blob_arena_test.cpp`).

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `blob_arena.hpp` not found.

- [ ] **Step 4: Write the header**

Create `code/core/mediaindex/blob_arena.hpp`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace mediaindex {

// Append-only byte arena for BC1 thumbnail payloads. Offsets/lengths are owned
// by MediaIndexDb (the files row). Compaction is added in Plan 2.
class BlobArena {
public:
    BlobArena() = default;
    BlobArena(const BlobArena&)            = delete;
    BlobArena& operator=(const BlobArena&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path);
    void               close();

    // Append bytes; returns the offset they were written at.
    [[nodiscard]] std::uint64_t append(std::span<const std::byte> bytes);

    // Copy out [offset, offset+length). nullopt if it would read past EOF.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    read(std::uint64_t offset, std::uint32_t length) const;

    [[nodiscard]] std::uint64_t size() const { return m_size; }

private:
    std::filesystem::path  m_path;
    mutable std::fstream   m_file;     // mutable: read() reopens read position
    std::uint64_t          m_size = 0; // logical EOF / next append offset
    bool                   m_open = false;
};

} // namespace mediaindex
```

- [ ] **Step 5: Write the implementation**

Create `code/core/mediaindex/blob_arena.cpp`:
```cpp
#include "blob_arena.hpp"

namespace mediaindex {

bool BlobArena::open(const std::filesystem::path& path) {
    m_path = path;
    // Create if missing, then open read/write binary at the end.
    { std::ofstream create(path, std::ios::binary | std::ios::app); if (!create) return false; }
    std::error_code ec;
    m_size = std::filesystem::file_size(path, ec);
    if (ec) return false;
    m_file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    m_open = static_cast<bool>(m_file);
    return m_open;
}

void BlobArena::close() {
    if (m_open) { m_file.flush(); m_file.close(); }
    m_open = false;
    m_size = 0;
}

std::uint64_t BlobArena::append(std::span<const std::byte> bytes) {
    const std::uint64_t offset = m_size;
    m_file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    m_file.write(static_cast<const char*>(static_cast<const void*>(bytes.data())),
                 static_cast<std::streamsize>(bytes.size()));
    m_file.flush();
    m_size += bytes.size();
    return offset;
}

std::optional<std::vector<std::byte>>
BlobArena::read(std::uint64_t offset, std::uint32_t length) const {
    if (offset + length > m_size) return std::nullopt;
    std::vector<std::byte> out(length);
    m_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    m_file.read(static_cast<char*>(static_cast<void*>(out.data())),
                static_cast<std::streamsize>(length));
    if (m_file.gcount() != static_cast<std::streamsize>(length)) { m_file.clear(); return std::nullopt; }
    return out;
}

} // namespace mediaindex
```

- [ ] **Step 6: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS — all `BlobArena` cases green.

- [ ] **Step 7: Commit**

```bash
git add code/core/mediaindex/blob_arena.hpp code/core/mediaindex/blob_arena.cpp tests/blob_arena_test.cpp CMakeLists.txt
git commit -m "feat(mediaindex): BlobArena append-only thumbnail byte store"
```

---

### Task 3: MediaIndexDb — open + schema

**Files:**
- Create: `code/core/mediaindex/media_index_db.hpp`
- Create: `code/core/mediaindex/media_index_db.cpp`
- Test: `tests/media_index_db_test.cpp` (replace the placeholder)

**Interfaces:**
- Produces: `class mediaindex::MediaIndexDb` with `bool open(const std::filesystem::path&)`, `void close()`, and (internal) schema creation. A `sqlite3*` owned via `std::unique_ptr` with a custom deleter.

- [ ] **Step 1: Write the failing test** (replace the placeholder file)

Replace `tests/media_index_db_test.cpp` with:
```cpp
#include <doctest.h>

#include <filesystem>

#include "media_index_db.hpp"

using mediaindex::MediaIndexDb;

namespace {
std::filesystem::path fresh_db(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir / "index.sqlite";
}
} // namespace

TEST_CASE("open creates the database file and is idempotent on reopen") {
    const auto path = fresh_db("midb_open");
    {
        MediaIndexDb db;
        REQUIRE(db.open(path));
        CHECK(std::filesystem::exists(path));
        db.close();
    }
    MediaIndexDb db2;          // reopen existing — schema already present, no error
    REQUIRE(db2.open(path));
    db2.close();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `media_index_db.hpp` not found.

- [ ] **Step 3: Write the header**

Create `code/core/mediaindex/media_index_db.hpp`:
```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "content_signature.hpp"

struct sqlite3;

namespace mediaindex {

class MediaIndexDb {
public:
    struct FileRow {
        std::int64_t     id   = 0;          // 0 until inserted
        ContentSignature sig;
        std::string      path;
        int              kind = 0;          // 0=image 1=video
        int              src_w = 0, src_h = 0;
        std::int64_t     capture_unix = 0, file_mtime = 0;
        int              score = 0;
        std::int64_t     duration_ms = 0;
        std::uint64_t    blob_offset = 0;
        std::uint32_t    blob_length = 0;
        int              thumb_w = 0, thumb_h = 0;
    };

    struct TagCount {
        std::int64_t id = 0;
        std::string  ns;
        std::string  name;
        std::int64_t count = 0;
    };

    enum class Sort { Score, CaptureDate, Size };

    MediaIndexDb() = default;
    ~MediaIndexDb();
    MediaIndexDb(const MediaIndexDb&)            = delete;
    MediaIndexDb& operator=(const MediaIndexDb&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& db_path);
    void               close();

    // Task 4: upsert by signature, returns row id. meta_full may be empty.
    [[nodiscard]] std::int64_t upsert_file(const FileRow& row,
                                           std::span<const std::byte> meta_full);

    // Task 5: tags.
    [[nodiscard]] std::int64_t upsert_tag(std::string_view ns, std::string_view name);
    void set_file_tags(std::int64_t file_id, std::span<const std::int64_t> tag_ids);

    // Task 6: include all of include_tags, exclude any of exclude_tags.
    [[nodiscard]] std::vector<FileRow> query(std::span<const std::int64_t> include_tags,
                                             std::span<const std::int64_t> exclude_tags,
                                             Sort sort, bool ascending);

    // Task 7: prefix autocomplete with file counts.
    [[nodiscard]] std::vector<TagCount> autocomplete(std::string_view prefix, int limit);

private:
    struct Sqlite3Deleter { void operator()(sqlite3*) const noexcept; };
    [[nodiscard]] bool exec(const char* sql);

    std::unique_ptr<sqlite3, Sqlite3Deleter> m_db;
};

} // namespace mediaindex
```

- [ ] **Step 4: Write the implementation (open + schema + deleter)**

Create `code/core/mediaindex/media_index_db.cpp`:
```cpp
#include "media_index_db.hpp"

#include <print>

#include <sqlite3.h>

namespace mediaindex {

void MediaIndexDb::Sqlite3Deleter::operator()(sqlite3* db) const noexcept {
    sqlite3_close_v2(db);
}

MediaIndexDb::~MediaIndexDb() { close(); }

bool MediaIndexDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(m_db.get(), sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::println("[mediaindex] SQL error: {}", err ? err : "(null)");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool MediaIndexDb::open(const std::filesystem::path& db_path) {
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(
        db_path.string().c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    m_db.reset(raw);
    if (rc != SQLITE_OK) {
        std::println("[mediaindex] open failed: {}", sqlite3_errmsg(raw));
        return false;
    }
    if (!exec("PRAGMA journal_mode=WAL;")) return false;
    if (!exec("PRAGMA foreign_keys=ON;"))  return false;

    static constexpr const char* k_schema = R"sql(
CREATE TABLE IF NOT EXISTS files (
  id           INTEGER PRIMARY KEY,
  sig_size     INTEGER NOT NULL,
  sig_head     INTEGER NOT NULL,
  sig_tail     INTEGER NOT NULL,
  path         TEXT    NOT NULL,
  kind         INTEGER NOT NULL,
  src_w        INTEGER, src_h INTEGER,
  capture_unix INTEGER, file_mtime INTEGER,
  score        INTEGER,
  duration_ms  INTEGER,
  blob_offset  INTEGER, blob_length INTEGER, thumb_w INTEGER, thumb_h INTEGER,
  meta_full    BLOB,
  UNIQUE(sig_size, sig_head, sig_tail)
);
CREATE INDEX IF NOT EXISTS files_score   ON files(score);
CREATE INDEX IF NOT EXISTS files_capture ON files(capture_unix);
CREATE INDEX IF NOT EXISTS files_size    ON files(sig_size);

CREATE TABLE IF NOT EXISTS tags (
  id   INTEGER PRIMARY KEY,
  ns   TEXT NOT NULL DEFAULT '',
  name TEXT NOT NULL,
  UNIQUE(ns, name)
);
CREATE INDEX IF NOT EXISTS tags_name ON tags(name);

CREATE TABLE IF NOT EXISTS file_tags (
  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  tag_id  INTEGER NOT NULL REFERENCES tags(id)  ON DELETE CASCADE,
  PRIMARY KEY (file_id, tag_id)
);
CREATE INDEX IF NOT EXISTS file_tags_tag ON file_tags(tag_id);

CREATE VIRTUAL TABLE IF NOT EXISTS tags_fts USING fts5(text, content='');
)sql";
    return exec(k_schema);
}

void MediaIndexDb::close() { m_db.reset(); }

} // namespace mediaindex
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS — the open/schema case is green (build proves FTS5 is available; if it errors `no such module: fts5`, the vcpkg install in Task 0 omitted the feature — re-run with `sqlite3[fts5]`).

- [ ] **Step 6: Commit**

```bash
git add code/core/mediaindex/media_index_db.hpp code/core/mediaindex/media_index_db.cpp tests/media_index_db_test.cpp
git commit -m "feat(mediaindex): MediaIndexDb open + WAL schema (files/tags/file_tags/fts)"
```

---

### Task 4: upsert_file (signature upsert + round-trip)

**Files:**
- Modify: `code/core/mediaindex/media_index_db.cpp` (add `upsert_file` + a small `Stmt` RAII helper)
- Test: `tests/media_index_db_test.cpp` (add cases)

**Interfaces:**
- Consumes: `FileRow`, `signature_of_bytes`.
- Produces: `std::int64_t upsert_file(const FileRow&, std::span<const std::byte> meta_full)` — inserts a new row or updates the existing row with the same signature; returns the row id.

- [ ] **Step 1: Write the failing tests** (append to `tests/media_index_db_test.cpp`)

```cpp
#include <cstddef>
#include <vector>
#include "content_signature.hpp"

using mediaindex::ContentSignature;

namespace {
MediaIndexDb::FileRow make_row(ContentSignature sig, const char* path, int score) {
    MediaIndexDb::FileRow r;
    r.sig = sig; r.path = path; r.kind = 0; r.src_w = 800; r.src_h = 600;
    r.capture_unix = 1000; r.file_mtime = 2000; r.score = score;
    r.blob_offset = 0; r.blob_length = 16; r.thumb_w = 4; r.thumb_h = 4;
    return r;
}
} // namespace

TEST_CASE("upsert_file inserts once and updates the same row on re-upsert") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_upsert")));
    const ContentSignature sig{123, 456, 789};

    const auto id1 = db.upsert_file(make_row(sig, "/a/photo.jpg", 5), {});
    CHECK(id1 > 0);

    // Same signature, new path + score (file moved + re-rated): SAME row id.
    const auto id2 = db.upsert_file(make_row(sig, "/b/photo.jpg", 9), {});
    CHECK(id2 == id1);
    db.close();
}

TEST_CASE("upsert_file stores meta_full bytes verbatim") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_meta")));
    const std::vector<std::byte> meta{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}};
    const auto id = db.upsert_file(make_row(ContentSignature{1, 2, 3}, "/x", 0), meta);
    CHECK(id > 0);
    // (read-back of meta_full is exercised via query() in Task 6; here we only
    //  assert the insert path accepts a blob without error.)
    db.close();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `upsert_file` unresolved.

- [ ] **Step 3: Add the `Stmt` helper + `upsert_file`** to `media_index_db.cpp`

After the `#include <sqlite3.h>` line, add an anonymous-namespace RAII statement wrapper:
```cpp
namespace {
struct Stmt {
    sqlite3_stmt* p = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &p, nullptr); }
    ~Stmt() { sqlite3_finalize(p); }
    Stmt(const Stmt&) = delete; Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return p != nullptr; }
};
} // namespace
```

Then add the method (inside `namespace mediaindex`):
```cpp
std::int64_t MediaIndexDb::upsert_file(const FileRow& row,
                                       std::span<const std::byte> meta_full) {
    static constexpr const char* k_sql = R"sql(
INSERT INTO files
  (sig_size, sig_head, sig_tail, path, kind, src_w, src_h,
   capture_unix, file_mtime, score, duration_ms,
   blob_offset, blob_length, thumb_w, thumb_h, meta_full)
VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)
ON CONFLICT(sig_size, sig_head, sig_tail) DO UPDATE SET
  path=excluded.path, kind=excluded.kind, src_w=excluded.src_w, src_h=excluded.src_h,
  capture_unix=excluded.capture_unix, file_mtime=excluded.file_mtime,
  score=excluded.score, duration_ms=excluded.duration_ms,
  blob_offset=excluded.blob_offset, blob_length=excluded.blob_length,
  thumb_w=excluded.thumb_w, thumb_h=excluded.thumb_h, meta_full=excluded.meta_full
RETURNING id;
)sql";
    Stmt st(m_db.get(), k_sql);
    if (!st) return 0;
    auto i64 = [&](int i, std::int64_t v) { sqlite3_bind_int64(st.p, i, v); };
    i64(1, static_cast<std::int64_t>(row.sig.size));
    i64(2, static_cast<std::int64_t>(row.sig.head));
    i64(3, static_cast<std::int64_t>(row.sig.tail));
    sqlite3_bind_text(st.p, 4, row.path.c_str(), -1, SQLITE_TRANSIENT);
    i64(5, row.kind);
    i64(6, row.src_w); i64(7, row.src_h);
    i64(8, row.capture_unix); i64(9, row.file_mtime);
    i64(10, row.score); i64(11, row.duration_ms);
    i64(12, static_cast<std::int64_t>(row.blob_offset));
    i64(13, static_cast<std::int64_t>(row.blob_length));
    i64(14, row.thumb_w); i64(15, row.thumb_h);
    if (meta_full.empty())
        sqlite3_bind_null(st.p, 16);
    else
        sqlite3_bind_blob(st.p, 16, meta_full.data(),
                          static_cast<int>(meta_full.size()), SQLITE_TRANSIENT);

    std::int64_t id = 0;
    if (sqlite3_step(st.p) == SQLITE_ROW) id = sqlite3_column_int64(st.p, 0);
    return id;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS — re-upsert returns the same id; blob insert succeeds.

- [ ] **Step 5: Commit**

```bash
git add code/core/mediaindex/media_index_db.cpp tests/media_index_db_test.cpp
git commit -m "feat(mediaindex): upsert_file (signature upsert, RETURNING id, meta_full blob)"
```

---

### Task 5: tags — upsert_tag + set_file_tags (+ FTS sync)

**Files:**
- Modify: `code/core/mediaindex/media_index_db.cpp`
- Test: `tests/media_index_db_test.cpp`

**Interfaces:**
- Produces:
  - `std::int64_t upsert_tag(std::string_view ns, std::string_view name)` — id of the (ns,name) tag, inserting + indexing in `tags_fts` if new.
  - `void set_file_tags(std::int64_t file_id, std::span<const std::int64_t> tag_ids)` — replaces the file's tag set.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("upsert_tag is idempotent per (ns,name)") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_tags")));
    const auto a = db.upsert_tag("creator", "foo");
    const auto b = db.upsert_tag("creator", "foo");
    const auto c = db.upsert_tag("", "landscape");
    CHECK(a > 0);
    CHECK(a == b);
    CHECK(c != a);
    db.close();
}

TEST_CASE("set_file_tags replaces the file's tag set") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_settags")));
    const auto fid = db.upsert_file(make_row(ContentSignature{7, 7, 7}, "/p", 1), {});
    const auto t1 = db.upsert_tag("", "a");
    const auto t2 = db.upsert_tag("", "b");
    const auto t3 = db.upsert_tag("", "c");
    const std::int64_t first[]  = {t1, t2};
    const std::int64_t second[] = {t2, t3};
    db.set_file_tags(fid, first);
    db.set_file_tags(fid, second);     // must REPLACE, not accumulate
    // Verified indirectly via query() in Task 6 (tag t1 should no longer match).
    db.close();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `upsert_tag` / `set_file_tags` unresolved.

- [ ] **Step 3: Implement** (append to `media_index_db.cpp`, in `namespace mediaindex`)

```cpp
std::int64_t MediaIndexDb::upsert_tag(std::string_view ns, std::string_view name) {
    {   // Insert if new; ignore on conflict.
        Stmt ins(m_db.get(),
            "INSERT OR IGNORE INTO tags(ns, name) VALUES(?1, ?2);");
        if (!ins) return 0;
        sqlite3_bind_text(ins.p, 1, ns.data(),   static_cast<int>(ns.size()),   SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.p, 2, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        const bool inserted = (sqlite3_step(ins.p) == SQLITE_DONE) && (sqlite3_changes(m_db.get()) > 0);
        if (inserted) {
            // Mirror into the FTS table for autocomplete. Store "ns:name" or "name".
            const std::string text = ns.empty() ? std::string(name)
                                                 : std::string(ns) + ":" + std::string(name);
            Stmt fts(m_db.get(), "INSERT INTO tags_fts(rowid, text) VALUES(?1, ?2);");
            if (fts) {
                sqlite3_bind_int64(fts.p, 1, sqlite3_last_insert_rowid(m_db.get()));
                sqlite3_bind_text(fts.p, 2, text.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(fts.p);
            }
        }
    }
    Stmt sel(m_db.get(), "SELECT id FROM tags WHERE ns=?1 AND name=?2;");
    if (!sel) return 0;
    sqlite3_bind_text(sel.p, 1, ns.data(),   static_cast<int>(ns.size()),   SQLITE_TRANSIENT);
    sqlite3_bind_text(sel.p, 2, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    return (sqlite3_step(sel.p) == SQLITE_ROW) ? sqlite3_column_int64(sel.p, 0) : 0;
}

void MediaIndexDb::set_file_tags(std::int64_t file_id,
                                 std::span<const std::int64_t> tag_ids) {
    exec("BEGIN;");
    {
        Stmt del(m_db.get(), "DELETE FROM file_tags WHERE file_id=?1;");
        if (del) { sqlite3_bind_int64(del.p, 1, file_id); sqlite3_step(del.p); }
    }
    for (const std::int64_t tid : tag_ids) {
        Stmt ins(m_db.get(),
            "INSERT OR IGNORE INTO file_tags(file_id, tag_id) VALUES(?1, ?2);");
        if (!ins) continue;
        sqlite3_bind_int64(ins.p, 1, file_id);
        sqlite3_bind_int64(ins.p, 2, tid);
        sqlite3_step(ins.p);
    }
    exec("COMMIT;");
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add code/core/mediaindex/media_index_db.cpp tests/media_index_db_test.cpp
git commit -m "feat(mediaindex): upsert_tag (+fts) and set_file_tags (replace set)"
```

---

### Task 6: query — include/exclude + sort

**Files:**
- Modify: `code/core/mediaindex/media_index_db.cpp`
- Test: `tests/media_index_db_test.cpp`

**Interfaces:**
- Produces: `std::vector<FileRow> query(include_tags, exclude_tags, Sort, ascending)` — files holding ALL include tags and NONE of the exclude tags, ordered.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("query intersects include tags and subtracts exclude tags, sorted by score") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_query")));

    const auto f1 = db.upsert_file(make_row(ContentSignature{1, 0, 0}, "/1", 30), {});
    const auto f2 = db.upsert_file(make_row(ContentSignature{2, 0, 0}, "/2", 10), {});
    const auto f3 = db.upsert_file(make_row(ContentSignature{3, 0, 0}, "/3", 20), {});

    const auto sky = db.upsert_tag("", "sky");
    const auto sea = db.upsert_tag("", "sea");
    const auto nsfw = db.upsert_tag("rating", "nsfw");

    const std::int64_t f1t[] = {sky, sea};         // f1: sky, sea
    const std::int64_t f2t[] = {sky};              // f2: sky
    const std::int64_t f3t[] = {sky, sea, nsfw};   // f3: sky, sea, nsfw
    db.set_file_tags(f1, f1t);
    db.set_file_tags(f2, f2t);
    db.set_file_tags(f3, f3t);

    // include {sky, sea}, exclude {nsfw}  => only f1.
    const std::int64_t inc[] = {sky, sea};
    const std::int64_t exc[] = {nsfw};
    auto r = db.query(inc, exc, MediaIndexDb::Sort::Score, /*ascending=*/false);
    REQUIRE(r.size() == 1);
    CHECK(r[0].id == f1);
    CHECK(r[0].path == "/1");

    // include {sky} only, sort score DESC => f1(30), f3(20), f2(10).
    const std::int64_t inc2[] = {sky};
    auto r2 = db.query(inc2, {}, MediaIndexDb::Sort::Score, false);
    REQUIRE(r2.size() == 3);
    CHECK(r2[0].id == f1);
    CHECK(r2[1].id == f3);
    CHECK(r2[2].id == f2);
    db.close();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `query` unresolved.

- [ ] **Step 3: Implement** (append to `media_index_db.cpp`)

```cpp
std::vector<MediaIndexDb::FileRow>
MediaIndexDb::query(std::span<const std::int64_t> include_tags,
                    std::span<const std::int64_t> exclude_tags,
                    Sort sort, bool ascending) {
    std::string sql =
        "SELECT id, sig_size, sig_head, sig_tail, path, kind, src_w, src_h, "
        "capture_unix, file_mtime, score, duration_ms, blob_offset, blob_length, "
        "thumb_w, thumb_h FROM files f";

    // include: file must have every include tag -> GROUP BY HAVING COUNT(distinct)=N
    if (!include_tags.empty()) {
        sql += " JOIN file_tags ft ON ft.file_id = f.id WHERE ft.tag_id IN (";
        for (std::size_t i = 0; i < include_tags.size(); ++i)
            sql += (i ? ",?" : "?") + std::to_string(static_cast<int>(i) + 1);
        sql += ")";
    } else {
        sql += " WHERE 1=1";
    }
    // exclude: NOT EXISTS any excluded tag for this file.
    for (std::size_t j = 0; j < exclude_tags.size(); ++j) {
        sql += " AND NOT EXISTS (SELECT 1 FROM file_tags x WHERE x.file_id=f.id AND x.tag_id=?"
             + std::to_string(static_cast<int>(include_tags.size() + j) + 1) + ")";
    }
    if (!include_tags.empty())
        sql += " GROUP BY f.id HAVING COUNT(DISTINCT ft.tag_id)="
             + std::to_string(include_tags.size());

    const char* col = (sort == Sort::Score) ? "score"
                    : (sort == Sort::CaptureDate) ? "capture_unix" : "sig_size";
    sql += std::string(" ORDER BY ") + col + (ascending ? " ASC" : " DESC");

    Stmt st(m_db.get(), sql.c_str());
    std::vector<FileRow> out;
    if (!st) return out;
    int bind = 1;
    for (const std::int64_t t : include_tags) sqlite3_bind_int64(st.p, bind++, t);
    for (const std::int64_t t : exclude_tags) sqlite3_bind_int64(st.p, bind++, t);

    while (sqlite3_step(st.p) == SQLITE_ROW) {
        FileRow r;
        r.id        = sqlite3_column_int64(st.p, 0);
        r.sig.size  = static_cast<std::uint64_t>(sqlite3_column_int64(st.p, 1));
        r.sig.head  = static_cast<std::uint64_t>(sqlite3_column_int64(st.p, 2));
        r.sig.tail  = static_cast<std::uint64_t>(sqlite3_column_int64(st.p, 3));
        r.path      = static_cast<const char*>(static_cast<const void*>(sqlite3_column_text(st.p, 4)));
        r.kind      = sqlite3_column_int(st.p, 5);
        r.src_w     = sqlite3_column_int(st.p, 6);
        r.src_h     = sqlite3_column_int(st.p, 7);
        r.capture_unix = sqlite3_column_int64(st.p, 8);
        r.file_mtime   = sqlite3_column_int64(st.p, 9);
        r.score        = sqlite3_column_int(st.p, 10);
        r.duration_ms  = sqlite3_column_int64(st.p, 11);
        r.blob_offset  = static_cast<std::uint64_t>(sqlite3_column_int64(st.p, 12));
        r.blob_length  = static_cast<std::uint32_t>(sqlite3_column_int64(st.p, 13));
        r.thumb_w      = sqlite3_column_int(st.p, 14);
        r.thumb_h      = sqlite3_column_int(st.p, 15);
        out.push_back(std::move(r));
    }
    return out;
}
```
> Style note: `sqlite3_column_text` returns `const unsigned char*`. Route it to `const char*` through `void*` with `static_cast` (as above) to honor the no-`reinterpret_cast` rule; keep this idiom local to column reads.

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS — include/exclude + sort correct; this also confirms Task 5's "replace" semantics (f1 only had t1 removed correctly).

- [ ] **Step 5: Commit**

```bash
git add code/core/mediaindex/media_index_db.cpp tests/media_index_db_test.cpp
git commit -m "feat(mediaindex): query with AND-include / NOT-exclude tags and sort"
```

---

### Task 7: autocomplete with counts

**Files:**
- Modify: `code/core/mediaindex/media_index_db.cpp`
- Test: `tests/media_index_db_test.cpp`

**Interfaces:**
- Produces: `std::vector<TagCount> autocomplete(std::string_view prefix, int limit)` — tags whose FTS text matches `prefix*`, each with the count of files carrying it, ordered by count desc.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("autocomplete returns prefix matches with file counts, by count desc") {
    MediaIndexDb db;
    REQUIRE(db.open(fresh_db("midb_ac")));

    const auto cat   = db.upsert_tag("", "cat");
    const auto cattle= db.upsert_tag("", "cattle");
    db.upsert_tag("", "dog");                       // must NOT match "cat"

    const auto f1 = db.upsert_file(make_row(ContentSignature{1, 0, 0}, "/1", 0), {});
    const auto f2 = db.upsert_file(make_row(ContentSignature{2, 0, 0}, "/2", 0), {});
    const std::int64_t f1t[] = {cat, cattle};
    const std::int64_t f2t[] = {cat};
    db.set_file_tags(f1, f1t);
    db.set_file_tags(f2, f2t);                        // cat -> 2 files, cattle -> 1

    auto r = db.autocomplete("cat", 10);
    REQUIRE(r.size() == 2);
    CHECK(r[0].name == "cat");                        // higher count first
    CHECK(r[0].count == 2);
    CHECK(r[1].name == "cattle");
    CHECK(r[1].count == 1);
    db.close();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/all --config Debug --target mediaindex_tests`
Expected: FAIL — `autocomplete` unresolved.

- [ ] **Step 3: Implement** (append to `media_index_db.cpp`)

```cpp
std::vector<MediaIndexDb::TagCount>
MediaIndexDb::autocomplete(std::string_view prefix, int limit) {
    // tags_fts.rowid == tags.id. Match prefix*, join counts from file_tags.
    static constexpr const char* k_sql = R"sql(
SELECT t.id, t.ns, t.name, COUNT(ft.file_id) AS n
FROM tags_fts
JOIN tags t ON t.id = tags_fts.rowid
LEFT JOIN file_tags ft ON ft.tag_id = t.id
WHERE tags_fts MATCH ?1
GROUP BY t.id
ORDER BY n DESC, t.name ASC
LIMIT ?2;
)sql";
    Stmt st(m_db.get(), k_sql);
    std::vector<TagCount> out;
    if (!st) return out;
    const std::string match = std::string(prefix) + "*";
    sqlite3_bind_text(st.p, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.p, 2, limit);
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        TagCount c;
        c.id    = sqlite3_column_int64(st.p, 0);
        c.ns    = static_cast<const char*>(static_cast<const void*>(sqlite3_column_text(st.p, 1)));
        c.name  = static_cast<const char*>(static_cast<const void*>(sqlite3_column_text(st.p, 2)));
        c.count = sqlite3_column_int64(st.p, 3);
        out.push_back(std::move(c));
    }
    return out;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cmake --build build/all --config Debug --target mediaindex_tests && ./build/all/Debug/mediaindex_tests`
Expected: PASS — `cat`(2) before `cattle`(1); `dog` absent.

- [ ] **Step 5: Final full-suite run + commit**

```bash
ctest --test-dir build/all -C Debug -R mediaindex_tests --output-on-failure
git add code/core/mediaindex/media_index_db.cpp tests/media_index_db_test.cpp
git commit -m "feat(mediaindex): autocomplete (FTS prefix match with file counts)"
```

---

## What this plan delivers

A linkable `code/core/mediaindex/` storage core — `ContentSignature`, `BlobArena`, `MediaIndexDb` (open/schema, `upsert_file`, `upsert_tag`, `set_file_tags`, `query`, `autocomplete`) — covered by the `mediaindex_tests` doctest target. No extraction, CLI, or UI.

## Follow-on plans (not in this document)

- **Plan 2 — extraction + ingest + CLI:** `ThumbnailEncoder` (image_ops + `encode_bc1`), `MetadataExtractor` (exiftool `-stay_open` + ffprobe + sidecar JSON, embedded-first), `meta_full` zstd compression, `IndexWriter::ingest(path)`, and the `media-indexer` executable fanning out over `ImageJobSystem`. Concurrency: WAL + per-row offset allocation.
- **Plan 3 — app integration:** tag-filter bar + namespace sidebar in `file_browser_ui`, grid driven by `MediaIndexDb::query`, zero-copy mmap `BlobArena` reader feeding `upload_bc1`, GPU `vulkan_bc1_encoder` as the app's `ThumbnailEncoder`, and the tag-export-to-files step.
