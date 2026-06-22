#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Single-blob, indexed thumbnail store. Codec-agnostic: stores opaque byte
// payloads (BC1 blocks) keyed by a 64-bit key. One append-only blob file plus a
// rewritten-on-close index. LRU byte cap with compaction. No Vulkan, no PCH —
// pure std so it links into the standalone thumbnail_blob_tests target.
class ThumbnailBlobCache {
public:
    struct Stored {
        std::vector<std::byte> blocks;
        int                    w = 0;
        int                    h = 0;
    };

    // Open/create blob + index under `dir`. cap_bytes bounds live payload bytes.
    bool open(const std::filesystem::path &dir, std::uint64_t cap_bytes);
    void close(); // flushes index to disk

    [[nodiscard]] std::optional<Stored> lookup(std::uint64_t key); // bumps LRU
    bool store(std::uint64_t key, std::span<const std::byte> blocks, int w, int h);
    void evict(std::uint64_t key);
    void clear(); // wipe blob + index

    // key = fnv1a(canonical path) ^ size ^ mtime  -> invalidates on file change.
    [[nodiscard]] static std::uint64_t make_key(const std::filesystem::path &file);

private:
    struct Record {
        std::uint64_t offset      = 0;
        std::uint32_t length      = 0;
        std::uint16_t w           = 0;
        std::uint16_t h           = 0;
        std::uint64_t last_access = 0; // monotonic tick
        bool          dead        = false;
    };

    static constexpr char         k_magic[4] = {'T', 'B', 'C', '1'};
    static constexpr std::uint32_t k_version = 1;

    [[nodiscard]] std::filesystem::path blob_path() const { return m_dir / "thumbs.bc1blob"; }
    [[nodiscard]] std::filesystem::path index_path() const { return m_dir / "thumbs.index"; }

    bool load_index(); // false -> caller wipes + starts empty
    void save_index() const;
    void enforce_cap();
    void compact();

    std::filesystem::path                     m_dir;
    std::uint64_t                             m_cap_bytes  = 0;
    std::uint64_t                             m_live_bytes = 0;
    std::uint64_t                             m_dead_bytes = 0;
    std::uint64_t                             m_tick       = 0;
    std::unordered_map<std::uint64_t, Record> m_index;
    bool                                      m_open = false;
};
