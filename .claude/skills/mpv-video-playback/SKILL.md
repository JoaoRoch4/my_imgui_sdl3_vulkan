---
name: mpv-video-playback
description: >
  Work on video reproduction in this project — playing/seeking/decoding video
  via libmpv and rendering frames into ImGui (Vulkan) textures, with two paths:
  the portable libmpv software render API and the NVDEC→OpenGL→libplacebo
  zero-copy path. Use when touching code under code/ui/media/video/player
  (VideoPlayer, VideoPlayerPlacebo, VideoEntry, PlaceboEntry, the EGL/GL-Vulkan
  interop), wiring mpv options/events/properties, debugging frame drops, hwdec,
  colour/tone mapping, or build deps (mpv, libplacebo, EGL, GL). References the
  ImPlay project and other external/ libraries for upstream API patterns.
---

# mpv + libplacebo video playback in ImGui (Vulkan)

This project plays video by driving **libmpv** as a headless decoder and pushing
each decoded frame into a **Vulkan texture** that ImGui samples with
`ImGui::Image(VkDescriptorSet, …)`. There are **two independent render paths**.
Know which one you're touching before editing.

## The two paths

| | Path A — Software render | Path B — libplacebo zero-copy |
|---|---|---|
| Class | `VideoPlayer` / `VideoEntry` | `VideoPlayerPlacebo` / `PlaceboEntry` |
| mpv VO | `vo=libmpv`, `MPV_RENDER_API_TYPE_SW` | `vo=libmpv`, OpenGL render (`mpv/render_gl.h`) |
| Frame path | mpv decodes → CPU pixels → staging `VkBuffer` → `vkCmdCopyBufferToImage` → `VkImage` | NVDEC → GL FBO (shared VkImage via FD interop) → `pl_tex` → `pl_render_image` → output `VkImage` |
| Copies | One CPU→GPU upload per frame (mpv writes straight into mapped GPU-visible memory) | Zero-copy on GPU (CUDA↔GL↔Vulkan shared image) |
| Extra deps | none beyond mpv + Vulkan | EGL surfaceless GL context, GL_EXT_memory_object_fd, VK_KHR_external_memory_fd, libplacebo |
| Use it for | Always works; default; hover/seek thumbnails; GIF/audio | High-perf HW-decode playback, colour/tone mapping via `pl_renderer` |
| Selected by | default | env `IMGUI_USE_VPP=1` (see `UseVideoPlayerPlacebo()` in `main_menu_bar.cpp`) |

`VideoPlayerPlacebo` **delegates hover/seek thumbnails to an internal
`VideoPlayer`** (`m_hover_player`) — the placebo path is only for the main
playback windows. Both classes expose a near-identical public API (see the two
headers) so the rest of the app can swap between them via the
`m_use_video_player_placebo` flag in `MainMenuBar`.

## File map

Project (`code/ui/media/video/`):
- `player/video_player.{hpp,cpp}` — Path A. Owns `m_entries`, the per-frame
  `update_frames()` / `draw()` loop, GPU resource lifecycle, mpv event polling.
- `player/VideoEntry.{hpp,cpp}` — Path A per-window state, incl. **double-buffered
  staging slots** (`StagingSlot`, `k_staging_count = 2`) and upload throttle.
- `player/video_player_placebo.{hpp,cpp}` — Path B. libplacebo GPU init
  (`init_placebo_gpu`), per-entry render (`entry_render_frame`), GL↔VK interop.
- `player/placebo_entry.hpp` — Path B per-window state. **Read its top-of-file
  ASCII diagram** — it documents the whole NVDEC→GL→VK→placebo→ImGui pipeline.
- `player/placebo_egl_context.{hpp,cpp}` — headless **surfaceless EGL** + OpenGL
  3.3 core context, one per placebo entry.
- `player/video_playback_mode.hpp` — `VideoPlaybackMode` enum (SwMpv / NvdecMpv /
  NvdecLibplacebo) + `mode_uses_hwdec` / `mode_uses_libplacebo` helpers.
- `video_preview/video_hover_preview.*`, `video_preview/video_seek_preview.*` —
  dedicated mpv handles for hover/seek thumbnails (separate from playback).
- `widgets/video_context_menu.*`, `widgets/video_osd_overlay.*`,
  `widgets/video_ui_window.*` — right-click menu, OSD text, control bar.
- `downloader/video_downloader.*` — yt-dlp/curl background download.

Reference libraries (`external/`), consult for upstream API usage:
- `external/ImPlay/` — a full libmpv + ImGui player (OpenGL). Best reference for
  the **mpv render-GL API, event loop, property observation, command patterns**.
  Key files: `include/mpv.h`, `source/mpv.cpp` (event loop, `render()`,
  `observeProperty`), `source/player.cpp` (UI wiring, key bindings).
- `external/mpv/include/` — the **bundled libmpv headers** the build uses
  (`mpv/client.h`, `mpv/render.h`, `mpv/render_gl.h`). Source of truth for the API.
- `external/libplacebo/src/include/` — libplacebo headers (`renderer.h`,
  `vulkan.h`, `gpu.h`, `colorspace.h`). Source of truth for `pl_*` calls.

## mpv fundamentals (apply to both paths)

Lifecycle per window/handle:
```cpp
mpv_handle *h = mpv_create();
mpv_set_option_string(h, "hwdec", hwdec ? "nvdec" : "no"); // options BEFORE init
mpv_set_option_string(h, "vo", "libmpv");                  // we render, not mpv
mpv_initialize(h);
mpv_render_context_create(&ctx, h, params);                // params pick SW vs GL
mpv_render_context_set_update_callback(ctx, cb, userdata); // cb just sets an atomic flag
const char *cmd[] = {"loadfile", path, nullptr};
mpv_command_async(h, 0, cmd);
// teardown: mpv_render_context_free(ctx); mpv_terminate_destroy(h);
```

Rules that bite if ignored:
- **Set options before `mpv_initialize`**, properties after. `hwdec`, `vo`,
  `loop-file`, network/cache opts are options.
- The **update callback runs on an mpv thread** — do nothing but
  `frame_dirty.store(true, release)`. All GPU/mpv-render work happens on the main
  thread in `update_frames()` (an assert guards `m_main_thread_id`).
- **Poll events every frame** with `mpv_wait_event(h, 0.0)` (non-blocking) until
  `MPV_EVENT_NONE`. The events you must handle: `MPV_EVENT_VIDEO_RECONFIG`
  (read `dwidth`/`dheight`, (re)create GPU resources, read `container-fps`),
  `MPV_EVENT_FILE_LOADED` (apply resume seek, clear error flags),
  `MPV_EVENT_END_FILE` (distinguish `MPV_END_FILE_REASON_EOF` from load failure).
- **Always `mpv_free()` strings** returned by `mpv_get_property_string` /
  `MPV_FORMAT_STRING` queries (e.g. the `hwdec-current` read).
- Property get/set use typed formats: `MPV_FORMAT_DOUBLE` (`time-pos`),
  `MPV_FORMAT_INT64` (`dwidth`), `MPV_FORMAT_FLAG` (`pause`). For observing
  changes prefer `mpv_observe_property` (see ImPlay's `observeProperty`).
- Common options already in use: URL streaming sets `ytdl=yes`,
  `ytdl-format=…`, `cache=yes`, `demuxer-max-bytes`, `stream-buffer-size`;
  GIFs/loop set `loop-file=inf`; hwdec sets
  `hwdec-codecs=h264,hevc,av1,vp9,mpeg4,vc1`.

## Path A — software render API

Frame upload (`VideoPlayer::upload_frame`): mpv renders **directly into mapped
GPU-visible staging memory** (no extra memcpy):
```cpp
std::array<int,2> size = {w, h};
size_t stride = size_t(w) * 4;
mpv_render_param p[] = {
  {MPV_RENDER_PARAM_SW_SIZE,   size.data()},
  {MPV_RENDER_PARAM_SW_FORMAT, "rgba"},
  {MPV_RENDER_PARAM_SW_STRIDE, &stride},
  {MPV_RENDER_PARAM_SW_POINTER, slot->mapped}, // persistently-mapped VkDeviceMemory
  {MPV_RENDER_PARAM_INVALID, nullptr},
};
if (mpv_render_context_render(ctx, p) < 0) return false; // no new frame
// then: barrier SHADER_READ→TRANSFER_DST, vkCmdCopyBufferToImage, barrier back, submit with per-slot fence
```
Key design points:
- **Two `StagingSlot`s ping-pong** so the GPU can read slot N while mpv fills
  slot N+1 — this was the fix for chronic frame drops. Each slot has its own
  buffer+command buffer+fence; check `in_flight`/fence before reuse.
- **Upload throttle**: `frame_interval` = 90% of one frame period (from
  `container-fps`), gating uploads so we don't churn duplicate frames.
- GPU resources (`VkImage`/view/sampler/`VkDescriptorSet`) are created on the
  first `VIDEO_RECONFIG` and recreated if dimensions change (call
  `vkDeviceWaitIdle` before tearing down).

## Path B — NVDEC → GL → libplacebo zero-copy

Pipeline (see `placebo_entry.hpp` diagram):
```
NVDEC(CUDA) → OpenGL FBO ↔ shared VkImage (FD interop) → pl_tex
  → pl_render_image (colour/tone/scale) → output VkImage → VkDescriptorSet → ImGui::Image
```
Setup (`init_placebo_gpu`): load `vkGetMemoryFdKHR`/`vkGetSemaphoreFdKHR`, create
`pl_log`, then **import the app's existing Vulkan device** with
`pl_vulkan_import` (extensions: external memory + external memory FD + external
semaphore + external semaphore FD), then `pl_renderer_create`. libplacebo does
**not** own the device.

Per entry:
- `placebo_egl_context.cpp` creates a **surfaceless** EGL display
  (`EGL_PLATFORM_SURFACELESS_MESA`), binds `EGL_OPENGL_API`, GL 3.3 core.
- `entry_create_shared_image`: a `VkImage` allocated with
  `VkExportMemoryAllocateInfo` / `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`,
  exported as an FD and imported into GL (`glImportMemoryFdEXT` →
  `glTextureStorageMem2DEXT` → FBO).
- `entry_create_sync`: two semaphores exported FD ↔ GL
  (`VkExportSemaphoreCreateInfo`) for GL↔VK ordering.
- `entry_render_frame` per dirty frame: make EGL current → (wait GL release sem)
  → `mpv_render_context_render` into `mpv_opengl_fbo` → signal GL ready sem →
  `glFlush` → release EGL → `pl_vulkan_release_ex`(input) → build input/output
  `pl_frame` → `pl_render_image` → `pl_vulkan_hold_ex`(input back to GL) →
  `pl_vulkan_hold_ex`(output in `SHADER_READ_ONLY` so ImGui samples it).
- **hold/release discipline is mandatory**: a `pl_tex` you sample in ImGui must
  be *held* in `SHADER_READ_ONLY` before draw and *released* back to libplacebo
  before the next render, or you get layout/sync validation errors and corruption.
- `pl_frame.color`/`.repr` describe colour space; set
  `planes[0].flipped = true` for the OpenGL Y convention.

## Per-frame contract (both paths, from `MainMenuBar::Build`)
1. `update_frames()` — poll mpv events, upload/render dirty frames. Main thread only.
2. `draw()` — inside the ImGui frame, `ImGui::Begin` per window, `ImGui::Image`
   the entry's `VkDescriptorSet` (placeholder texture until the first frame).
3. Closed entries (`open == false`) are evicted by `std::erase_if` after draw;
   free GPU + mpv resources there, never mid-iteration.

## Build / dependencies
- `CMakeLists.txt`: `pkg_check_modules` for `libplacebo`, `egl`, `gl`;
  `find_library(MPV_LIBRARIES NAMES mpv)`. Headers are **vendored**:
  `MPV_INCLUDE_DIRS = external/mpv/include`,
  `LIBPLACEBO_INCLUDE_DIRS = external/libplacebo/src/include`.
- Runtime needs a working NVIDIA stack for Path B (NVDEC + GL/EGL + CUDA interop);
  Path A has no such requirement and is the safe fallback.

## Gotchas checklist
- Don't call mpv render / GPU work off the main thread (the update callback only
  flips an atomic).
- Free mpv strings; free render context before `mpv_terminate_destroy`.
- Recreate GPU resources on `VIDEO_RECONFIG`, after `vkDeviceWaitIdle`.
- Path B: keep the EGL context current only around the mpv GL render, and never
  forget the hold/release pairing on `pl_input_tex`/`pl_output_tex`.
- When adding a feature to one path, check whether the other path (and
  `m_hover_player`) needs the mirror change — the public APIs are kept in sync.
