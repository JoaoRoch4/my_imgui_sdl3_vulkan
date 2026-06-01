---
name: ffmpeg-codebase
description: >
  Navigate and work with the FFmpeg source vendored at external/FFmpeg (libav*
  libraries + fftools). Use when reading/patching FFmpeg internals, locating a
  decoder/encoder/demuxer/filter/hwaccel, understanding the hardware-context
  system (Vulkan/CUDA/VAAPI device & frame contexts), tracing the
  decode/encode/demux APIs, FFmpeg coding conventions (AVClass/options,
  AVBufferRef refcounting, av_log, AVERROR), or debugging FFmpeg-originated
  crashes seen in this app (e.g. NVDEC teardown in libavcodec/nvdec.c). Pairs
  with the [[vulkan-video-zerocopy]] skill for the Vulkan-decode path.
---

# Working in the FFmpeg source (external/FFmpeg)

Vendored full FFmpeg checkout, **version 8.0.git** (newer than the system
`ffmpeg` 7.1.2). It's already configured/built — `.o`/`.d` artifacts sit next to
sources, so `compile_commands`-style navigation and grep both work.

## Library map (what lives where)

| Dir | Role |
|---|---|
| `libavutil/` | Core utilities: `AVFrame`, `AVDictionary`, **hwcontext** (device/frame), math, pixel formats (`pixfmt.h`, `pixdesc.c`), logging (`log.c`), `AVBufferRef` refcounting (`buffer.c`), `opt.c` (AVOptions). |
| `libavcodec/` | Codecs: decoders/encoders/parsers/bitstream filters + **hwaccels**. `avcodec.h` is the public API. |
| `libavformat/` | (de)muxers, protocols, I/O (`avio`). `avformat.h`. |
| `libavfilter/` | Filter graph + filters (`vf_*`, `af_*`), incl. `vf_libplacebo.c`. |
| `libswscale/` | CPU pixel-format/colorspace conversion & scaling. |
| `libswresample/` | Audio resample/format conversion. |
| `libavdevice/` | Capture/render devices (v4l2, alsa, …). |
| `fftools/` | The CLI apps: `ffmpeg.c`, `ffplay.c` (+ `ffplay_renderer.c`), `ffprobe.c`. Great end-to-end usage examples. |

## Public decode/encode API (the contract this app uses)

Send/receive model (since ~2017), in `libavcodec/avcodec.h`:
```c
avformat_open_input / avformat_find_stream_info        // libavformat: demux
avcodec_find_decoder(id); avcodec_alloc_context3(codec)
avcodec_parameters_to_context(ctx, stream->codecpar)
avcodec_open2(ctx, codec, &opts)
// loop:
av_read_frame(fmt, pkt)
avcodec_send_packet(ctx, pkt)         // returns AVERROR(EAGAIN)/EOF semantics
avcodec_receive_frame(ctx, frame)     // drain until EAGAIN
```
- All return `int`; `< 0` is an error. Decode `AVERROR(EAGAIN)`/`AVERROR_EOF` as
  flow control, not failure. Stringify with `av_err2str(ret)`.
- `AVFrame`/`AVPacket`/`AVBufferRef` are **reference-counted**: `av_frame_unref`,
  `av_packet_unref`, `av_buffer_ref/unref`. Never free the data directly.
- Objects with options use **AVClass**: discover via `av_opt_next`, set via
  `av_opt_set*` / the `AVDictionary` passed to `avcodec_open2`.

## Hardware context system (most relevant to this project)

`libavutil/hwcontext.h` + per-backend `hwcontext_*.{c,h}`:
- **Device context** (`AVHWDeviceContext`): one per GPU/API. Create with
  `av_hwdevice_ctx_create(type, ...)`, OR **import an existing device** by
  `av_hwdevice_ctx_alloc(type)` → fill the typed `hwctx` → `av_hwdevice_ctx_init`.
- **Frame context** (`AVHWFramesContext`): pool of GPU frames of a hw pixfmt.
- A decoder uses hardware when `AVCodecContext.hw_device_ctx` is set and its
  `get_format` callback returns the hw pixfmt (e.g. `AV_PIX_FMT_VULKAN`,
  `AV_PIX_FMT_CUDA`).

Backends present here (`ls libavutil/hwcontext_*.c`): `vulkan`, `cuda`, `vaapi`,
`drm`, `vdpau`, `qsv`, `opencl`, …

### Vulkan path (decode-only, GPU, no OpenGL) — see [[vulkan-video-zerocopy]]
- `libavutil/hwcontext_vulkan.{c,h}` — `AVVulkanDeviceContext` (fields:
  `inst`, `phys_dev`, `act_dev`, `device_features`, enabled instance/device
  extensions, queue families, `lock_queue`/`unlock_queue`). This is how you make
  FFmpeg share the app's `VkDevice`.
- `libavcodec/vulkan_decode.{c,h}`, `vulkan.c`, `vulkan_av1.c`, `vulkan_apv.c` —
  the `VK_KHR_video_decode_*` decoders. Output frames are `AV_PIX_FMT_VULKAN`.
- `fftools/ffplay_renderer.c` — canonical FFmpeg-Vulkan-decode → **libplacebo**
  render example. Best single file to copy patterns from.

### CUDA/NVDEC path (the one that crashed this app)
- `libavcodec/nvdec.c` — NVDEC hwaccel. **`nvdec_unmap_mapped_frame` (~line 472)
  is where this app SIGSEGV'd on shutdown**: a CUDA error during frame unmap is
  logged via `ff_cuda_check` → `av_log`, and tearing down the CUDA/NVDEC context
  while frames are still mapped hits a bad pointer. Triggered only with
  `hwdec=nvdec`. `libavutil/hwcontext_cuda.*` + `libavutil/cuda_check.h` are the
  surrounding pieces. Prefer the Vulkan decode path or SW to avoid this.
- `*_cuvid` decoders (`libavcodec/cuviddec.c`) are the NVDEC CUVID front-ends
  (`h264_cuvid`, `hevc_cuvid`, …) seen in `ffmpeg -decoders`.

## Conventions / gotchas
- Error codes are negative `AVERROR(e)`; compare with `AVERROR_EOF`,
  `AVERROR(EAGAIN)`, etc. Don't treat `EAGAIN` as fatal.
- Logging goes through `av_log(avcl, level, fmt, …)` where `avcl` is an
  AVClass-bearing context (or NULL). A bad format/arg here is a common crash site.
- Memory: `av_malloc/av_freep`, `av_frame_alloc/free`, ref/unref everywhere;
  ownership transfers on `move`-style refs.
- Threading: codecs may use frame/slice threads; hwaccel frames must be consumed
  before the device/frames context is destroyed (the NVDEC crash above is exactly
  a teardown-ordering violation).

## Navigation recipes
```sh
cd external/FFmpeg
# find a decoder registration / codec
grep -rn "ff_h264_decoder\|\.name *= *\"h264\"" libavcodec | head
# list hwaccels for a codec
grep -rn "AV_HWACCEL\|hwaccel" libavcodec/h264*.c | head
# how a public API is implemented
grep -rn "int avcodec_receive_frame" libavcodec/decode.c
# all Vulkan-decode entry points
grep -rln "VK_KHR_video_decode\|AV_PIX_FMT_VULKAN" libavcodec libavutil
```

## Build notes
- Already built in-tree (`.o`/`.d` present). Rebuild a single object with
  `make libavcodec/vulkan_decode.o` after `./configure` was run. Public headers
  for *consumers* are the installed/`pkg-config` ones; this checkout is the source
  of truth for *behavior* and for reading the Vulkan/hwcontext internals.
- Relevant configure flags for this project's interests: `--enable-vulkan`
  (Vulkan Video), `--enable-nvdec`/`--enable-cuda-llvm` (NVDEC), `--enable-libdrm`.
