# my_imgui_sdl3_vulkan

A native, GPU-accelerated **media browser and viewer** built from scratch on
**SDL3 + Vulkan + Dear ImGui** — fast async file browsing, GPU-compressed
thumbnails, hardware-accelerated video playback (mpv + libplacebo), and a set
of homegrown developer/diagnostic tools (thread reflection, memory tracking,
runtime TOML config), all in modern C++23.

> **Status:** actively-developed personal project, pre-1.0. APIs, structure
> and UI change frequently — see [`todo.txt`](todo.txt) for the live task list.

---

## Features

**File browser**
- Async, thread-pooled directory scanning — file operations (copy/move/delete/trash)
  never block the UI thread.
- Grid / list / masonry view modes, multi-select, rubber-band select, context menu.
- Thumbnail pipeline with a CPU decode path and an optional **GPU BC1/DXT1-compressed
  thumbnail cache** (single-blob store, LRU + compaction) for fast redraws at scale.

**Media viewing**
- Image viewer backed by a parallel image-processing engine (`image_ops` + `JobQueue`
  over a `ManagedThread` pool).
- Video playback via **mpv + libplacebo**, hardware-decoded through FFmpeg
  (NVDEC/VAAPI/VDPAU), with a native mpv OSD and hover-preview scrubbing.
- Built-in video downloader (`yt-dlp` subprocess wrapper) with live progress.
- Audio/video metadata (tag) editor via TagLib.

**Developer / diagnostic tooling**
- `ManagedThread` + reflection system — every worker thread self-reports into a
  live "Thread Overwatch" panel.
- Centralized `MemoryManagement` registry that guards against duplicate allocation.
- Runtime configuration panel backed by TOML (via reflect-cpp), hot-editable and
  persisted to `window_state.toml`.
- In-app console, FPS plot (ImPlot), optional ImGui multi-viewport support, and
  ASan+UBSan-instrumented Debug builds — **on by default**, turn them off with
  `-DENABLE_SANITIZERS=OFF`.

### Command-line flags

| Flag | Effect |
| --- | --- |
| `--monitor-thread` | Force the live Thread Overwatch panel open on launch |
| `--file-browser` / `--no-file-browser` | Force the file explorer open / closed this session |
| `--no-video` | Disable video loading/playback this session (images still work) |
| `--no-media` | Disable all media loading this session (implies `--no-video`) |

Flags are **session-only** overrides — they never rewrite your saved
`window_state.toml` preferences.

---

## Tech stack

| Area | Library |
| --- | --- |
| Windowing / input | [SDL3](https://github.com/libsdl-org/SDL) |
| Rendering | Vulkan |
| UI | [Dear ImGui](https://github.com/ocornut/imgui) (docking) + [ImPlot](https://github.com/epezent/implot) |
| Video decode/playback | [FFmpeg](https://ffmpeg.org/), [mpv](https://github.com/mpv-player/mpv), [libplacebo](https://code.videolan.org/videolan/libplacebo) |
| Images | [stb](https://github.com/nothings/stb), [libwebp](https://github.com/webmproject/libwebp), [plutovg/plutosvg](https://github.com/sammycage/plutosvg) |
| Metadata / tags | [TagLib](https://github.com/taglib/taglib) |
| Config serialization | [reflect-cpp](https://github.com/getml/reflect-cpp) (TOML) |
| Networking | curl |
| Testing | [doctest](https://github.com/doctest/doctest) |
| Build | CMake (Ninja Multi-Config) + vcpkg |

Almost the whole media/UI stack is **compiled from source**: `setup.sh` clones it
into the git-ignored `external/` tree at pinned refs, and the authored CMake glue
under `thirdparty/<lib>/` builds it. Only curl and reflect-cpp come from vcpkg on
Linux; freetype, fontconfig, EGL and GL come from the system. `vcpkg.json` still
declares the full dependency set for the platforms that need it — see
[`docs/README.md`](docs/README.md#what-comes-from-where).

## Project layout

```
.
├── code/               Application source (C++23)
│   ├── main/             Entry point, App lifecycle, window/runtime state
│   ├── core/             Memory management, threading, logging, image ops
│   ├── rendering/        Vulkan device/swapchain/pipeline, SDL3 context, texture upload
│   ├── ui/
│   │   ├── window/         App coordinator, main menu bar, thread-reflection panel, FPS plot
│   │   ├── FileExplorer/   Async file browser + thumbnail cache
│   │   ├── media/          image / video / metadata_editor / preview_ui / handlers
│   │   ├── console/        In-app developer console
│   │   ├── config/         Runtime TOML configuration
│   │   └── Editor/         Text editor (WIP)
│   ├── Args/             Command-line argument parsing
│   └── pch/              Precompiled header
├── tests/              doctest unit tests (image ops, image jobs, job queue,
│                       file operations, thumbnail blob cache)
├── thirdparty/         Authored CMake build glue for from-source dependencies
├── cmake/              Shared CMake helper modules
├── scripts/            Python helpers (video download)
├── tools/              IdeMcpServer (C#) — IDE integration helper
├── docs/               Design specs, implementation plans, architecture notes
├── QT/VulkanMedia/     Standalone Qt6/QML UI port (opt-in; the Windows build target)
├── bootstrap.py        Cross-platform setup + IDE configuration (Linux & Windows)
├── setup.sh            One-shot dependency bootstrap (Fedora/Nobara)
├── build.sh            Build/run/test/profile wrapper around the CMake presets
└── vcpkg.json          Every dependency that has a vcpkg port (24). mpv,
                        libplacebo and ffmpegthumbnailer have none — always
                        from source. Declarative: manifest mode is OFF, and on
                        Linux the build only takes curl + reflectcpp from vcpkg
```

---

## Getting started

The primary platform is **Linux (Fedora/Nobara)**, clang + Ninja Multi-Config.
On Windows the buildable target is `QT/VulkanMedia` → `appVulkanMedia.exe`; the
main SDL3/Vulkan app does not configure there yet (see
[`docs/windows-msvc-context.md`](docs/windows-msvc-context.md) §6 for the exact
blockers).

### Quick start — any OS, any IDE

```sh
git clone https://github.com/JoaoRoch4/my_imgui_sdl3_vulkan.git
cd my_imgui_sdl3_vulkan

python3 bootstrap.py          # packages + vcpkg + external/ + configure + IDE config
python3 bootstrap.py build debug      # or: python3 bootstrap.py run debug
```

[`bootstrap.py`](bootstrap.py) runs on Linux and Windows, needs nothing but the
Python standard library, and is idempotent — re-run it any time to pick up only
what is missing. System packages are covered for **dnf**, **apt** and **pacman**;
on Windows it verifies the toolchain and offers winget. It also writes the **VS Code**, **CLion** and **Visual Studio**
configuration for whichever machine it runs on. `python3 bootstrap.py doctor`
reports what the machine has without changing anything;
`python3 bootstrap.py --help` lists every phase and flag.

### Quick start — the bash path (Fedora/Nobara)

```sh
./setup.sh --build      # installs system packages, vcpkg deps (curl, reflectcpp[toml]),
                        # clones the from-source media stack, then builds Debug
./build.sh debug --run  # or, if you already built: just run it
```

`setup.sh` is idempotent too. Run `./setup.sh --help` / `./build.sh --help` for
all phases and flags (`--skip-packages`, `--skip-vcpkg`, `--jobs N`, `--rebuild`,
`--fresh`, `--perf`, …). Prerequisites: Fedora/Nobara with `dnf5` (or `dnf`) and
`sudo`; [vcpkg](https://github.com/microsoft/vcpkg) is auto-bootstrapped if missing.

### Manual CMake

```sh
cmake --preset all                      # Ninja Multi-Config -> build/all
cmake --build build/all --config Debug
```

Each configuration writes its executable to its own folder:

| Config | Binary |
| --- | --- |
| Debug | `build/debug/example_sdl3_vulkan_debug` |
| Release | `build/release/example_sdl3_vulkan_release` |
| RelWithDebInfo | `build/RELWITHDEBINFO/example_sdl3_vulkan_RELWITHDEBINFO` |

### Running tests

```sh
./build.sh debug --test
# or
ctest --test-dir build/all -C Debug --output-on-failure
```

---

## Documentation

- [`docs/README.md`](docs/README.md) — documentation index: setup, the branches,
  and what each document covers. **Start here.**
- [`docs/windows-msvc-context.md`](docs/windows-msvc-context.md) — the measured
  Windows/MSVC toolchain: which toolset the repo uses, which Visual Studio
  installation CMake picks, what still does not build there. *(pt-BR)*
- [`docs/`](docs) — design specs and implementation plans, written spec-first
  under `docs/superpowers/specs/` and `docs/superpowers/plans/` (thread
  reflection, BC1 thumbnail cache, media tag index, mpv native OSD, …).
- [`docs/memory_management.md`](docs/memory_management.md) — why `AppContext`
  uses `unique_ptr` ownership + raw observer pointers throughout.
- [`QT/VulkanMedia/README.md`](QT/VulkanMedia/README.md) — the Qt6/QML UI port.

## Roadmap

See [`todo.txt`](todo.txt) for the current, unfiltered task list. Notable
upcoming work: FFmpeg-based tagging for videos, a metadata-manager window,
GPU-accelerated thumbnail previews, and a file-browser refactor that splits
file-operations and thumbnail-generation into dedicated subsystems.

## License

No license file is currently included in this repository — all rights are
reserved by default. Open an issue if you'd like to discuss reuse or
licensing terms.
