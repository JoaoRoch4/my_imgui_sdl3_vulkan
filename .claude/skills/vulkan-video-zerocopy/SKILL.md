---
name: vulkan-video-zerocopy
description: >
  Design/implement true zero-copy GPU video playback on Vulkan with NO OpenGL —
  hardware-decode with FFmpeg Vulkan Video (AV_HWDEVICE_TYPE_VULKAN, frames as
  AV_PIX_FMT_VULKAN) sharing the app's VkDevice, then map frames straight into
  libplacebo (pl_map_avframe_ex) and render (pl_render_image) into a VkImage that
  ImGui samples. Use when replacing/avoiding the fragile NVDEC→OpenGL FBO→
  GL/Vulkan interop path (surfaceless EGL, GL_EXT_memory_object, semaphore FDs),
  or when wiring FFmpeg Vulkan hwaccel + libplacebo, sharing a VkDevice across
  decode/render, or choosing the "best library" for embedded Vulkan video.
  Canonical reference: external/FFmpeg/fftools/ffplay_renderer.c.
---

# Zero-copy video on Vulkan (FFmpeg Vulkan Video + libplacebo, no OpenGL)

The goal: play/seek/decode video entirely on the GPU and hand frames to an
ImGui (Vulkan) `ImGui::Image()` **without any CPU copy and without OpenGL**.

## Why this exists / why not the current path

This project today has two paths (see the [[mpv-video-playback]] skill):
- **SW render** — `libmpv` decodes to CPU pixels, uploaded to a `VkImage`. Robust
  but one CPU→GPU copy per frame.
- **NVDEC → OpenGL FBO → libplacebo** — mpv renders via its **OpenGL** render API
  into a GL texture that's a shared `VkImage` (FD interop), then libplacebo reads
  it. The zero-copy is real but the **OpenGL/EGL interop layer is the fragile
  part**: it needs a headless EGL context, `GL_EXT_memory_object`/`_fd`,
  `GL_EXT_semaphore_fd`, and a GL driver that matches the Vulkan GPU. On NVIDIA's
  *surfaceless* EGL this commonly fails to expose `GL_EXT_memory_object`, leaving
  a placeholder that never renders.

**The modern path removes OpenGL entirely.** As of the 2024 FFmpeg/libplacebo/mpv
work, you can decode with **Vulkan Video** and present with **libplacebo** end to
end, all on one `VkDevice`:

```
demux (libavformat)
  → libavcodec decode, hwaccel = AV_HWDEVICE_TYPE_VULKAN
      → AVFrame with format AV_PIX_FMT_VULKAN  (frame data IS a VkImage)
  → pl_map_avframe_ex()      // zero-copy: wraps the AVFrame's VkImage as a pl_tex
  → pl_render_image()        // colour/tonemap/scale on the GPU
  → output VkImage → VkDescriptorSet → ImGui::Image()
```

No EGL, no GL extensions, no FD juggling — the decoder, libplacebo, and the app
all use the **same** `VkDevice`, so the decoded image is consumed in place.

## Best library choice

- **Decode:** FFmpeg `libavcodec` with the **Vulkan hwaccel** (`hwdevice` type
  `vulkan`). This is cross-vendor (NV/AMD/Intel) via `VK_KHR_video_decode_*` and
  needs no CUDA. (`*_cuvid`/NVDEC still exist but reintroduce CUDA-GL interop.)
- **Render/colour management:** **libplacebo** (`pl_renderer`) — already a project
  dependency. It owns colour space, tone mapping, scaling, and the AVFrame bridge.
- Keep **mpv** only if you need its demuxer/network/`yt-dlp`/subtitle stack;
  libmpv's render API is OpenGL-only, so mpv cannot be the Vulkan render path.
  For local files, FFmpeg-direct is the cleaner zero-copy Vulkan route.

## This machine already supports it (verified)

- `ffmpeg -hwaccels` →  `… vulkan` (FFmpeg 7.1.2 system; `external/FFmpeg` = 8.0.git).
- `vulkaninfo` → `VK_KHR_video_queue`, `VK_KHR_video_decode_h264` (rev 9),
  `VK_KHR_video_decode_h265` (rev 8), `VK_KHR_video_decode_av1` (rev 1) on the RTX 3060.
- libplacebo ships the bridge: `external/libplacebo/src/include/libplacebo/utils/libav.h`.

So H.264 / HEVC / AV1 hardware Vulkan decode + libplacebo render is available here
with no OpenGL. (VP9 has no Vulkan-decode extension → falls back to SW/another hwaccel.)

## Canonical reference — read this first

`external/FFmpeg/fftools/ffplay_renderer.c` is FFmpeg's own end-to-end example of
exactly this pipeline. Key call sequence (line numbers approx, FFmpeg 8.0.git):

| Step | Call | ffplay_renderer.c |
|---|---|---|
| Share app's device into libplacebo | `pl_vulkan_import(&import_params)` | ~283 |
| (or create one) | `pl_vulkan_create(pl_vulkan_params(...))` | ~384 |
| Wrap that device for FFmpeg | `av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN)` → fill `AVVulkanDeviceContext` → `av_hwdevice_ctx_init()` | ~397, ~460 |
| libplacebo log + renderer | `pl_log_create`, `pl_renderer_create` | ~481, ~523 |
| Decoder picks Vulkan output | `get_format` returns `AV_PIX_FMT_VULKAN` | ~702 |
| **Zero-copy map** | `pl_map_avframe_ex(gpu, &pl_frame, pl_avframe_params(.frame = avframe))` | ~741 |
| Render | `pl_render_image(renderer, &pl_frame, &target, &params)` | ~776 |
| Release | `pl_unmap_avframe(gpu, &pl_frame)` | ~790 |

ffplay renders `target` into a libplacebo **swapchain** and presents. **In this
ImGui app, render `target` into an offscreen `VkImage` instead** (as the existing
placebo path already does with `pl_output_tex` → `VkImageView` → `VkDescriptorSet`),
then draw it with `ImGui::Image()`. See [[mpv-video-playback]] for the
output-image → descriptor-set plumbing already written in
`code/ui/media/video/player/video_player_placebo.cpp`.

Other references in `external/FFmpeg`:
- `libavfilter/vf_libplacebo.c` — AVFrame ↔ libplacebo inside a filter (map/render/unmap).
- `libavcodec/vulkan_decode.{c,h}`, `libavcodec/vulkan_av1.c` — the Vulkan Video decoders.
- `libavutil/hwcontext_vulkan.{c,h}` — the Vulkan device/frame context (device sharing).

## Key APIs

libplacebo (`utils/libav.h`):
- `pl_map_avframe_ex(pl_gpu, struct pl_frame *out, const struct pl_avframe_params *)`
  — maps an `AVFrame` to a `pl_frame`; **auto-detects hwdec/Vulkan frames** and
  wraps the underlying `VkImage` with no copy. Returns false if unmappable.
- `pl_unmap_avframe(pl_gpu, struct pl_frame *)` — must pair with every successful map.
- `pl_frame_from_avframe(out, frame)` — metadata only (no textures).
- `pl_avframe_params{ .frame, .tex, .map_dovi, … }` (`PL_AVFRAME_DEFAULTS`).

FFmpeg device sharing (`libavutil/hwcontext_vulkan.h`): `AVVulkanDeviceContext`
fields to populate from the app/libplacebo device — `inst`, `phys_dev`, `act_dev`,
`device_features`, `enabled_inst_extensions`, `enabled_dev_extensions`, queue
families, and `lock_queue`/`unlock_queue` callbacks (FFmpeg and the app must not
submit to the same queue concurrently — guard with these or use distinct queues).

## Migration sketch (when implementing)

1. Build/locate FFmpeg with `--enable-vulkan` (system 7.1.2 already has it).
2. Create the Vulkan device with the features/extensions both libplacebo
   (`pl_vulkan_required_features`: `hostQueryReset`, `timelineSemaphore` — already
   added in `vulkan_context.cpp`) **and** FFmpeg Vulkan decode need
   (`VK_KHR_video_queue`, `VK_KHR_video_decode_queue` + per-codec
   `VK_KHR_video_decode_h264/h265/av1`, plus sync2/timeline). Enumerate against the
   physical device and enable what's present.
3. Reuse the existing `pl_vulkan_import` in `video_player_placebo.cpp`; build an
   `AVVulkanDeviceContext` from `m_pl_vk` (or from `m_vk`) and `av_hwdevice_ctx_init`.
4. Open the stream with `libavformat`; set `AVCodecContext.hw_device_ctx` and a
   `get_format` that returns `AV_PIX_FMT_VULKAN`.
5. Per frame: `avcodec_receive_frame` → `pl_map_avframe_ex` → `pl_render_image`
   into the per-entry output `VkImage` → `pl_unmap_avframe`. Replace the entire
   EGL/GL FBO/interop machinery (`placebo_egl_context.*`, the `s_gl`/FD code).
6. Keep the SW `libmpv` path as the fallback for URLs/yt-dlp and unsupported codecs.

## Caveats
- Per-codec Vulkan decode support varies by GPU/driver — always check
  `VK_KHR_video_decode_*` and gracefully fall back (SW MPV) when absent.
- `lock_queue`/`unlock_queue`: serialize app vs FFmpeg queue submits or use a
  dedicated decode queue family to avoid races.
- libplacebo and FFmpeg must agree on the device's enabled features/extensions you
  declared at `vkCreateDevice` time (mismatch → import/validation failure).
- Vulkan Video is decode-only here; colour/scale/tonemap is libplacebo's job.

## Sources
- libplacebo #272 — rendering zero-copy AVFrames decoded via Vulkan: https://github.com/haasn/libplacebo/issues/272
- mpv #11739 — Vulkan Video Decoding usage guide & FAQ: https://github.com/mpv-player/mpv/issues/11739
- FFmpeg ffplay Vulkan renderer via libplacebo (patch set): https://ffmpeg.org/pipermail/ffmpeg-devel/2023-October/315852.html
- libplacebo FFmpeg/dav1d integration (DeepWiki): https://deepwiki.com/haasn/libplacebo/6.1-ffmpeg-and-dav1d-integration
- FFmpeg HWAccelIntro: https://trac.ffmpeg.org/wiki/HWAccelIntro
- AVVulkanDeviceContext reference: https://ffmpeg.org/doxygen/trunk/structAVVulkanDeviceContext.html
