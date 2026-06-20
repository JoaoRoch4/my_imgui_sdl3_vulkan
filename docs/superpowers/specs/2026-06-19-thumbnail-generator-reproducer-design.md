# Design: split file-browser thumbnailing into Generator + Reproducer

**Date:** 2026-06-19
**Branch:** `refactor/registry-owned-subsystems`

## Goal

Separate the file-browser thumbnail decode into two single-responsibility classes,
per the engine policy:

- **Generate** a thumbnail from a media source → **ffmpegthumbnailer** (video) +
  **stb** (image). Persist a PNG.
- **Reproduce** (reload) an already-cached thumbnail for display → **stb** (decode
  the cached PNG); **mpv `hwdec=nvdec-copy`** only as a re-derive fallback.

"Use the fastest": stb for image/PNG decode (no per-file `AVFormatContext` like the
current libav `decode_file`), nvdec-copy hardware decode for the mpv fallback.

## Current state (what's being replaced)

`FileBrowserThumbnailContext::submit_image()` runs one composite job on the
`ImageJobSystem` pool that: (cache hit) decodes the PNG via libav `decode_file`, or
(miss) decodes the source via ffmpegthumbnailer (video) / libav `decode_file`
(image), letterboxes to 320×180, and `encode_png`s. A dormant libmpv worker
(`FileBrowserThumbnailThread`, the idle `FbThumbVideo` threads) already exists and
already uses `hwdec=nvdec-copy` + SW readback + `stbi_write_png`.

## New components (code/ui/FileExplorer/)

### `thumbnail_image_io.hpp` (header-only)
`fbthumb::decode_stb(path, channels) -> std::expected<img::ImageBuffer, img::ImageError>`
— shared stb (`stbi_load`) decoder used by both classes. Header-only inline so no
new CMake entry; `stb_image.h` impl is already linked via `thirdparty/stb`.

### `ThumbnailGenerator` (static)
`generate(source, is_video, out_png, w, h) -> ImgResult`:
- video → `img::ops::decode_video_thumbnail` (ffmpegthumbnailer, `workaround_bugs=1`);
- image/gif → `fbthumb::decode_stb` (stb first frame for gif — no seek);
- then `letterbox_fit(w,h)` (moved here from the context's anon namespace) →
  `img::ops::encode_png(out)`; returns the letterboxed RGBA.
Static so the `ImageJobSystem` lambda captures no `this`.

### `ThumbnailReproducer`
- `static reload(png_path) -> ImgResult`: `fbthumb::decode_stb` of the cached PNG.
- Instance owns the libmpv worker (`FileBrowserThumbnailThread`, nvdec-copy) for the
  re-derive fallback: `start(on_done)`, `shutdown()`, `submit(key,file,out)`,
  `clear_pending()`. Touched only from the render thread.

## Wiring in `FileBrowserThumbnailContext`

- `submit_image` lambda becomes a dispatch: `exists(out)` → `ThumbnailReproducer::reload(out)`;
  else → `ThumbnailGenerator::generate(file, decode_as_video, out, k_thumb_w, k_thumb_h)`.
- Member `FileBrowserThumbnailThread m_video` → `ThumbnailReproducer m_reproducer`
  (owns the worker). `setup/shutdown/clear` delegate; `on_video_done` callback +
  `m_video_results` drain in `begin_frame` are unchanged.
- mpv fallback: `Entry` gains `source_is_video` and `tried_mpv`. In `poll_entry`,
  when the ImageJob future resolves to `Failed` for a video source and `!tried_mpv`,
  submit the source to `m_reproducer` (async); the mpv result returns through the
  existing `on_video_done` → `m_video_results` → `begin_frame` path.
- `classify()` / `is_image_ext` / `is_video_ext`: `.gif` is image (already fixed in
  the working tree) — keep consistent so the dominant render path routes GIFs to stb.
- Fix the stale "disable hwdec" comment over the worker's `nvdec-copy` line.

## CMake

Add `thumbnail_generator.cpp` + `thumbnail_reproducer.cpp` to the app sources.

## Verification

- `./build.sh debug --rebuild` compiles + links.
- GIFs and images thumbnail via stb (no "Seeking in video failed"); videos via
  ffmpegthumbnailer; cached thumbnails reload via stb; a video ffmpegthumbnailer
  can't handle falls back to the mpv nvdec-copy worker.
