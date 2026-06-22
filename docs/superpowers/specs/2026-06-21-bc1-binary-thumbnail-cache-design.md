# BC1 Binary Thumbnail Cache — Design

**Date:** 2026-06-21
**Status:** Approved (design); pending implementation plan
**Scope:** File-browser thumbnail cache (`code/ui/FileExplorer`)

## Problem

The file-browser thumbnail cache (`FileThumbnailCache`) currently persists one
**PNG per file** (320×180) under a cache directory, keyed by a hash of the path
(`file_thumbnail_cache.cpp:198`). Two costs grow without bound:

1. **Disk footprint.** PNG is lossless; a photographic 320×180 thumbnail is
   typically 60–150 KB. A large media library produces a *massive* cache.
2. **File-count / inode pressure.** One small file per thumbnail means thousands
   of inodes and slow directory scans.

Secondary goal: the user wants **GPU-accelerated** upload — skip the per-frame
decode and reduce VRAM.

## Decision summary

Add an **optional BC1 (DXT1) block-compressed backend** that stores all
thumbnails in a **single append-only binary blob** with an in-memory index,
uploaded directly into a `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` image with **no
decode step**. The existing PNG path is retained as the portable fallback.

Why BC1 specifically (decided during brainstorming):

- **8:1 compression**, 4 bits/px → ~28 KB per 320×180 thumbnail vs 60–150 KB PNG.
- **No decode on upload.** Block data is mmap'd from the blob and staged straight
  into a compressed `VkImage`; the data stays compressed all the way into VRAM
  (8:1 VRAM saving vs the current uncompressed `VK_FORMAT_R8G8B8A8_UNORM` path at
  `vulkan_texture.cpp:152`).
- **1-bit punch-through alpha is the right fit.** The only transparency in a
  thumbnail is the *binary* letterbox (`thumbnail_generator.cpp:33` —
  fully-opaque image region over fully-transparent bars). BC1's punch-through
  alpha represents exactly this with no extra cost. BC7 (4:1, smooth alpha) was
  rejected: 2× the disk for quality not perceptible at grid size, plus a heavier
  non-stb dependency.
- **stb-idiomatic.** Encoding uses `stb_dxt.h` (header-only), matching the
  codebase's existing stb usage (`stb_image`, `stb_image_write`) — no heavy new
  dependency.

Alternatives considered and rejected for the primary path: WebP-in-blob (smaller
on disk but still requires a decode and stays uncompressed in VRAM), TGA (larger
than PNG — wrong direction), AVIF/HEVC-NVENC filmstrip (NVIDIA-only, heavy
FFmpeg build changes, random-access penalty; left as a possible *future* layer,
not part of this design).

## Config / the "optional" contract

A config toggle selects the backend:

```
thumbnails.format = bc1 | png
```

- **Default `bc1`** when the device exposes `textureCompressionBC`.
- **Auto-fallback to `png`** when the feature is unsupported (detected once at
  setup and logged). `png` is also selectable explicitly.

This keeps the feature "optional": it is a config choice with a hardware-driven
automatic fallback, so a machine without BC support still works unchanged.

## Architecture

Each unit has one purpose, a narrow interface, and is testable in isolation.

### 1. `img::ops::encode_bc1`

- **Location:** `code/core/image/image_ops.{hpp,cpp}`, beside `encode_png`.
- **Signature:** `std::expected<std::vector<std::byte>, ImageError> encode_bc1(ImageBuffer const& src);`
- **Depends on:** `stb_dxt.h` (added header-only to `thirdparty/stb`).
- **Contract:** input is letterboxed RGBA8 (`w`, `h`, 4 channels). Output is BC1
  block data of exactly `ceil(w/4) * ceil(h/4) * 8` bytes. Uses punch-through
  alpha mode so transparent letterbox bars decode as transparent.
- **Constraint:** `image_ops` must remain **PCH-free** (see project memory
  `image-core-pch-free`) — `stb_dxt.h` is header-only and adds no pch dependency,
  use explicit std headers only.

### 2. `ThumbnailBlobCache` — the binary cache

- **Location:** new `code/ui/FileExplorer/thumbnail_blob_cache.{hpp,cpp}`.
- **Knows nothing about Vulkan.** Pure storage + indexing + eviction.
- **On disk:**
  - `thumbs.bc1blob` — concatenated BC1 payloads, append-only.
  - `thumbs.index` — `{magic, version}` header followed by fixed-size records:
    `{ key:u64, offset:u64, length:u32, w:u16, h:u16, last_access:u64, flags:u32 }`
    (`flags` carries a `dead` bit). Loaded into an in-memory
    `std::unordered_map<u64, Record>` at setup; rewritten on shutdown / compaction.
- **Key:** `hash(path + mtime + size)` → automatic invalidation when a source
  file changes. Replaces the per-file hashed `.png` filename.
- **Interface:**
  - `std::optional<Slice> lookup(u64 key)` — returns `{ptr, length, w, h}` into the
    mmap'd blob and bumps `last_access`.
  - `u64 append(u64 key, std::span<const std::byte> blocks, int w, int h)` — append
    payload, record index entry. Single-writer (render thread).
  - `void evict(u64 key)` / `void clear()` — mark dead / wipe.
  - `void enforce_cap()` / `void compact()` — LRU + compaction (below).
- **Reads** mmap the blob (zero-copy). **Writes** are single-writer on the render
  thread, so no blob-level locking is needed for appends; the in-memory index map
  is guarded by the existing `FileThumbnailCache` mutex.

### 3. `VulkanTexture::upload_bc1`

- **Location:** extend `code/rendering/vulkan/vulkan_texture.{hpp,cpp}`.
- **Signature:** `bool upload_bc1(std::span<const std::byte> blocks, int w, int h, vulkan_context& vk);`
- Creates a `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` image (extent rounded up to 4×4
  blocks), stages the block bytes via the existing upload-buffer machinery, and
  registers the descriptor with ImGui exactly like the RGBA8 path.
- **Device feature:** `code/rendering/vulkan/vulkan_context.cpp` must request
  `textureCompressionBC` at device creation (currently features are queried at
  `vulkan_context.cpp:220` but BC is not enabled). If the physical device does
  not support it, report unsupported so the cache falls back to PNG.

### 4. `FileThumbnailCache` integration

- A backend mode (`bc1` | `png`) chosen at `setup()` from config ∧ device support.
- **`get(path)`:**
  - *bc1 hit:* compute key → `ThumbnailBlobCache::lookup` → `upload_bc1` → `Ready`.
    No decode.
  - *bc1 miss:* spawn generator (worker pool). Generator decodes source
    (stb / ffmpegthumbnailer) → letterbox → `encode_bc1` → returns
    blocks+dims into the `Entry`. The **render thread** then `append`s to the blob,
    records the index, and `upload_bc1`s. (Preserves the existing rule:
    "generators never touch Vulkan; the render thread uploads.")
  - *png mode:* unchanged from today.
- **`evict`/`clear`** delegate to the blob cache in bc1 mode (mark dead / wipe
  blob+index) and to PNG deletion in png mode.

## Data flow

```
HIT  (bc1): get(path) ─key→ index lookup ─mmap slice→ upload_bc1 → ImTextureID    [no decode]

MISS (bc1): get(path) → spawn generator
            worker: decode source → letterbox RGBA8 → encode_bc1 → Entry{blocks,w,h}
            render: append(blob)+index → upload_bc1 → ImTextureID
```

## LRU cap & compaction

- Configurable byte cap (default ~256 MB). `lookup` bumps `last_access`.
- When total blob bytes exceed the cap → mark least-recently-used entries `dead`.
- When `dead` bytes exceed a threshold (e.g. 25% of blob) → **compaction**:
  write live slices to `thumbs.bc1blob.tmp`, rebuild the index, `fsync`, atomic
  rename over the originals. Runs on shutdown and opportunistically.

## Error handling

- `textureCompressionBC` unsupported → disable bc1, use png. One check at setup,
  logged.
- `encode_bc1` fails for an entry → fall back to PNG for that single entry.
- Corrupt or version-mismatched blob/index header at startup → discard both and
  rebuild (thumbnails regenerate on demand). No partial-trust of stale files.
- Append failure (disk full / I/O error) → entry stays in-memory `Ready` for the
  session; logged; retried next run.

## Testing

- **`encode_bc1`** (`image_tests`, PCH-free):
  - Output length equals `ceil(w/4)*ceil(h/4)*8` for non-multiple-of-4 sizes.
  - Punch-through alpha: a letterboxed buffer decodes with transparent bars.
  - Round-trip PSNR sanity via a minimal BC1 decoder on a known gradient.
- **`ThumbnailBlobCache`** (new test target or existing fixture):
  - Insert N entries, read each back by key, assert byte-exactness.
  - LRU eviction drops the oldest under a small cap.
  - Compaction preserves all live entries and rebuilds a valid index.
  - Corrupt header → clean rebuild (no crash, cache starts empty).

## Files

| File | Change |
|------|--------|
| `thirdparty/stb/stb_dxt.h` | add header-only encoder |
| `code/core/image/image_ops.{hpp,cpp}` | add `encode_bc1` (PCH-free) |
| `code/ui/FileExplorer/thumbnail_blob_cache.{hpp,cpp}` | **new** blob+index+LRU+compaction |
| `code/rendering/vulkan/vulkan_texture.{hpp,cpp}` | add `upload_bc1`, BC1 image path |
| `code/rendering/vulkan/vulkan_context.cpp` | enable `textureCompressionBC` feature |
| `code/ui/FileExplorer/file_thumbnail_cache.{hpp,cpp}` | backend mode + bc1 orchestration + fallback |
| `code/ui/config/*` | `thumbnails.format` toggle + cap |
| tests | `encode_bc1` + `ThumbnailBlobCache` |

## Out of scope (possible future work)

- HEVC/NVENC filmstrip cache (NVIDIA-only GPU encode + zero-copy NVDEC decode).
- BC7 high-quality variant.
- Mipmapped thumbnails.
