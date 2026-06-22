#include "pch.hpp" // NOLINT
#include "thumbnail_blob_cache.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>

namespace {
std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

std::uint64_t ThumbnailBlobCache::make_key(const std::filesystem::path &file) {
    std::error_code ec;
    const auto      canonical = std::filesystem::weakly_canonical(file, ec).string();
    std::uint64_t   key       = fnv1a(canonical);
    const auto      sz        = std::filesystem::file_size(file, ec);
    if (!ec)
        key ^= sz * 1099511628211ull;
    const auto wt = std::filesystem::last_write_time(file, ec);
    if (!ec)
        key ^= static_cast<std::uint64_t>(wt.time_since_epoch().count());
    return key;
}

bool ThumbnailBlobCache::open(const std::filesystem::path &dir, std::uint64_t cap_bytes) {
    m_dir       = dir;
    m_cap_bytes = cap_bytes;
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    m_index.clear();
    m_live_bytes = m_dead_bytes = m_tick = 0;
    if (!load_index()) { // corrupt/missing -> wipe and start empty
        std::filesystem::remove(blob_path(), ec);
        std::filesystem::remove(index_path(), ec);
        m_index.clear();
        m_live_bytes = m_dead_bytes = 0;
    }
    m_open = true;
    return true;
}

void ThumbnailBlobCache::close() {
    if (!m_open)
        return;
    save_index();
    m_open = false;
}

std::optional<ThumbnailBlobCache::Stored> ThumbnailBlobCache::lookup(std::uint64_t key) {
    auto it = m_index.find(key);
    if (it == m_index.end() || it->second.dead)
        return std::nullopt;
    Record       &r = it->second;
    std::ifstream f(blob_path(), std::ios::binary);
    if (!f.is_open())
        return std::nullopt;
    f.seekg(static_cast<std::streamoff>(r.offset));
    Stored out;
    out.blocks.resize(r.length);
    f.read(std::bit_cast<char *>(out.blocks.data()), r.length);
    if (f.gcount() != static_cast<std::streamsize>(r.length))
        return std::nullopt;
    out.w           = r.w;
    out.h           = r.h;
    r.last_access   = ++m_tick;
    return out;
}

bool ThumbnailBlobCache::store(std::uint64_t key, std::span<const std::byte> blocks, int w, int h) {
    if (blocks.empty())
        return false;
    // Append offset = current blob size (reliable across platforms vs tellp+app).
    std::error_code     ec;
    const std::uint64_t offset =
        std::filesystem::exists(blob_path(), ec) ? std::filesystem::file_size(blob_path(), ec) : 0;
    std::ofstream f(blob_path(), std::ios::binary | std::ios::app);
    if (!f.is_open())
        return false;
    f.write(std::bit_cast<const char *>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
    if (!f.good())
        return false;

    Record r;
    r.offset      = offset;
    r.length      = static_cast<std::uint32_t>(blocks.size());
    r.w           = static_cast<std::uint16_t>(w);
    r.h           = static_cast<std::uint16_t>(h);
    r.last_access = ++m_tick;
    m_index[key]  = r;
    m_live_bytes += r.length;
    enforce_cap();
    return true;
}

void ThumbnailBlobCache::evict(std::uint64_t key) {
    auto it = m_index.find(key);
    if (it == m_index.end() || it->second.dead)
        return;
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
        // Find the live entry with the smallest last_access (LRU).
        auto lru = m_index.end();
        for (auto it = m_index.begin(); it != m_index.end(); ++it) {
            if (it->second.dead)
                continue;
            if (lru == m_index.end() || it->second.last_access < lru->second.last_access)
                lru = it;
        }
        if (lru == m_index.end())
            break;
        evict(lru->first);
    }
    if (m_dead_bytes > m_live_bytes / 4 + 1024 * 1024)
        compact();
}

void ThumbnailBlobCache::compact() {
    const auto    tmp = m_dir / "thumbs.bc1blob.tmp";
    std::ifstream in(blob_path(), std::ios::binary);
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!in.is_open() || !out.is_open())
        return;
    std::uint64_t     new_off = 0;
    std::vector<char> buf;
    for (auto it = m_index.begin(); it != m_index.end();) {
        if (it->second.dead) {
            it = m_index.erase(it);
            continue;
        }
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
    if (!f.is_open())
        return true; // no index yet = empty, valid
    char          magic[4]{};
    std::uint32_t version = 0;
    std::uint32_t count   = 0;
    f.read(magic, 4);
    f.read(std::bit_cast<char *>(&version), sizeof version);
    f.read(std::bit_cast<char *>(&count), sizeof count);
    if (!f.good() || std::memcmp(magic, k_magic, 4) != 0 || version != k_version)
        return false; // corrupt -> caller wipes
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t key = 0;
        Record        r;
        f.read(std::bit_cast<char *>(&key), sizeof key);
        f.read(std::bit_cast<char *>(&r.offset), sizeof r.offset);
        f.read(std::bit_cast<char *>(&r.length), sizeof r.length);
        f.read(std::bit_cast<char *>(&r.w), sizeof r.w);
        f.read(std::bit_cast<char *>(&r.h), sizeof r.h);
        f.read(std::bit_cast<char *>(&r.last_access), sizeof r.last_access);
        if (!f.good())
            return false;
        m_index[key] = r;
        m_live_bytes += r.length;
        m_tick = std::max(m_tick, r.last_access);
    }
    return true;
}

void ThumbnailBlobCache::save_index() const {
    std::ofstream f(index_path(), std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return;
    std::uint32_t count = 0;
    for (const auto &[k, r] : m_index)
        if (!r.dead)
            ++count;
    f.write(k_magic, 4);
    f.write(std::bit_cast<const char *>(&k_version), sizeof k_version);
    f.write(std::bit_cast<const char *>(&count), sizeof count);
    for (const auto &[key, r] : m_index) {
        if (r.dead)
            continue;
        f.write(std::bit_cast<const char *>(&key), sizeof key);
        f.write(std::bit_cast<const char *>(&r.offset), sizeof r.offset);
        f.write(std::bit_cast<const char *>(&r.length), sizeof r.length);
        f.write(std::bit_cast<const char *>(&r.w), sizeof r.w);
        f.write(std::bit_cast<const char *>(&r.h), sizeof r.h);
        f.write(std::bit_cast<const char *>(&r.last_access), sizeof r.last_access);
    }
}
