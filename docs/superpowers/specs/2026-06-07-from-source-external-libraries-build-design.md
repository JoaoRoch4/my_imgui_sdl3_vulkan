# From-Source External Libraries: Per-Folder Modular Build

- **Date:** 2026-06-07
- **Status:** Approved design, **not yet implemented**
- **Branch:** `feature/parallel-image-library`
- **Touches:** root `CMakeLists.txt` (becomes a thin orchestrator), new
  `external/<lib>/CMakeLists.txt` per dependency, `build/thirdparty/` output tree.

## 1. Goal

Compile the project's major external libraries **from source, in this project**, so
their symbols are profilable (perf / lldb step-in), and reorganize the build so
**every** compiled dependency lives in **its own folder with its own build
description**, with the root `CMakeLists.txt` reduced to orchestration + the app
target. This generalizes the treatment the root file already applies inline to
`imgui` / `implot` / `stb`.

Two intertwined outcomes:

1. **From-source builds** of SDL3, the Vulkan **loader**, FFmpeg, mpv, libplacebo,
   and taglib (today they are system/vcpkg prebuilt libs), each with the uniform
   "profilable" recipe (debug info on, stripping off, frame pointers kept).
2. **Build modularization**: each compiled dependency becomes
   `external/<lib>/CMakeLists.txt`, pulled in by the root via `add_subdirectory`;
   foreign (autotools/meson) builds hide behind a thin wrapper `CMakeLists.txt`
   that wraps `ExternalProject_Add` and exposes an `IMPORTED` target.

### Non-goals / out of scope

- **WebP, CURL, reflectcpp** stay as `find_package`/vcpkg — no source is vendored,
  so they are not "possible from source" without fetching new repos. Untouched.
- **Vulkan validation layers** are *not* compiled. We compile the **loader** only;
  the SDK's prebuilt `libVkLayer_khronos_validation.so` continues to be used at
  runtime via `VK_LAYER_PATH`.
- No change to application C++ source. This is purely build-system work.

## 2. Why "with symbol exports" really means "-g + no-strip"

The request is "compile with symbol exports." The honest mechanism:

- A prebuilt `.so` (the previous SDL3/Vulkan/mpv path) cannot have its symbol
  visibility changed from the consumer side — `-rdynamic` only re-exports the
  **executable's own** defined symbols, not a dependency's.
- For a sampling profiler, what actually matters is a non-stripped `.symtab` +
  DWARF (`-g`), which lets it map sampled addresses → source lines **even for
  symbols the library hides from `.dynsym`** (SDL3 / Vulkan-Loader / FFmpeg all set
  `-fvisibility=hidden` on internals for ABI). Default visibility is a bonus that
  populates `.dynsym`; it is best-effort and may not fully un-hide internals.

So the universal recipe is: **debug info on, stripping off, frame pointers kept**,
expressed as target properties (CMake libs) or configure/meson flags (foreign
builds). This mirrors the existing stb/imgui/implot "Profiling / symbol-export
treatment" block in the current root `CMakeLists.txt`.

## 3. Target architecture

Root `CMakeLists.txt` becomes an **orchestrator**:

```cmake
set toolchain (clang/clang++ + LLD, all configs)
add_subdirectory(external/imgui)        # CMake target  -> imgui
add_subdirectory(external/implot)       # CMake target  -> implot
add_subdirectory(external/stb)          # CMake target  -> stb
add_subdirectory(external/taglib)       # native CMake   -> tag
add_subdirectory(external/SDL3-3.4.10)  # native CMake   -> SDL3::SDL3
add_subdirectory(external/Vulkan-Loader)# native CMake   -> vulkan (loader)
add_subdirectory(external/FFmpeg)       # wrapper: ExternalProject -> ffmpeg::*
add_subdirectory(external/libplacebo)   # wrapper: ExternalProject -> placebo::placebo
add_subdirectory(external/mpv)          # wrapper: ExternalProject -> mpv::mpv
add_executable(example_sdl3_vulkan ...)  # thin: just sources + links the targets
```

`add_subdirectory` is the **single unifying seam**: the root treats all nine
compiled deps identically — `add_subdirectory()` then link a target. Autotools/meson
messiness is contained inside the wrapper folder.

### 3.1 Folder & target layout

| Folder | Build system | Mechanism | App-visible target | Was |
| --- | --- | --- | --- | --- |
| `external/imgui-1.92.8-docking/` *(+ new CMakeLists)* | CMake (in-tree src) | `add_subdirectory` | `imgui` | inline in root |
| `external/implot/` *(+ new CMakeLists)* | CMake | `add_subdirectory` | `implot` | inline in root |
| `external/stb/` *(+ new CMakeLists)* | CMake (generated TU) | `add_subdirectory` | `stb` | inline in root |
| `external/taglib/` | native CMake | `add_subdirectory` | `tag` | vcpkg `TagLib::tag` |
| `external/SDL3-3.4.10/` | native CMake | `add_subdirectory` | `SDL3::SDL3` | system `pkg_check_modules` |
| `external/Vulkan-Loader/` *(cloned v1.4.350)* | native CMake | `add_subdirectory` | `vulkan` | system `pkg_check_modules` |
| `external/FFmpeg/` *(+ wrapper CMakeLists)* | autotools | `ExternalProject_Add` | `ffmpeg::avcodec` … | transitive via system mpv |
| `external/libplacebo/` *(+ wrapper)* | meson | `ExternalProject_Add` | `placebo::placebo` | system `pkg_check_modules` |
| `external/mpv/` *(+ wrapper)* | meson | `ExternalProject_Add` | `mpv::mpv` | system `find_library` |

**Stays system/vcpkg** (no vendored source): WebP, CURL, reflectcpp, EGL, GL.

All build artifacts land under **`build/thirdparty/<lib>/`** (centralized, keeps the
vendored source folders pristine); the foreign ExternalProjects install into
`build/thirdparty/<lib>/install/` prefixes that downstream builds consume via
`PKG_CONFIG_PATH` / `CMAKE_PREFIX_PATH`.

### 3.2 The uniform "profilable" recipe

| Build system | How the recipe is applied |
| --- | --- |
| CMake (`imgui`, `implot`, `stb`, `taglib`, `SDL3`, `vulkan` loader) | `C/CXX_VISIBILITY_PRESET default`, `VISIBILITY_INLINES_HIDDEN OFF`, `-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`, no strip |
| FFmpeg (autotools) | `--cc=clang --enable-debug=3 --disable-stripping --extra-cflags="-g -fno-omit-frame-pointer"` |
| libplacebo + mpv (meson) | `-Dbuildtype=debugoptimized -Db_ndebug=false -Dstrip=false -Dc_args="-g -fno-omit-frame-pointer"`; clang via a meson **native file** |

## 4. Per-library build notes

- **imgui / implot / stb** — pure relocation: lift today's inline `add_library` +
  include/define/visibility blocks verbatim into `external/<lib>/CMakeLists.txt`.
  Behavior identical; only the file boundary and output dir change. `implot`
  keeps `target_link_libraries(implot PUBLIC imgui)`.
- **taglib** — native CMake `add_subdirectory`; build `tag` static, link
  `TagLib::tag` consumers onto the in-tree target. Drops the vcpkg dependency.
- **SDL3 (3.4.10)** — `add_subdirectory(external/SDL3-3.4.10)`; configure with
  `SDL_SHARED=ON`, `SDL_STATIC=OFF`, `SDL_TEST_LIBRARY=OFF`; link `SDL3::SDL3`.
  Note: 3.4.10 (vendored) vs 3.5.0 (system) is a minor version drop — API used by
  the app (`imgui_impl_sdl3`, window/event/vulkan-surface) is stable across it.
- **Vulkan-Loader (v1.4.350, cloned)** — `add_subdirectory`; point
  `VULKAN_HEADERS_INSTALL_DIR` / `find_package(VulkanHeaders)` at the cloned
  `external/Vulkan-Headers` (v1.4.350). Produces `libvulkan.so`; the app links this
  instead of system `libvulkan`. At runtime the from-source loader still discovers
  the system ICD/driver via `/usr/share/vulkan/icd.d` and layers via `VK_LAYER_PATH`
  pointed at the SDK's layer dir.
- **FFmpeg (8.0.git)** — `ExternalProject_Add` running `./configure` + `make` +
  `make install` into `build/thirdparty/ffmpeg/install`. Feature set kept
  deliberately lean but sufficient for mpv (enable what mpv needs; disable
  programs/docs/tests). nasm + yasm present. **Consumed by the mpv build only** —
  never appears on the app's own link line.
- **libplacebo (meson)** — submodules now populated at pinned commits (glad
  `73db193`, jinja, markupsafe, Vulkan-Headers `74d8a6c`, fast_float).
  `ExternalProject_Add` driving meson/ninja; install into
  `build/thirdparty/libplacebo/install`. App links `placebo::placebo` from there;
  mpv's build can also find it via `PKG_CONFIG_PATH`.
- **mpv (0.41.0, meson)** — `ExternalProject_Add` with `-Dlibmpv=true
  --default-library=shared`, finding **our** FFmpeg (and optionally our libplacebo)
  via `PKG_CONFIG_PATH`. Installs `libmpv.so` + `mpv.pc`; app links `mpv::mpv`.
  luajit absent → Lua scripting auto-disabled (unused by this app).

## 5. App link-line changes (root CMakeLists)

Deleted: `pkg_check_modules(SDL3 …)`, `pkg_check_modules(VULKAN …)`,
`pkg_check_modules(LIBPLACEBO …)`, `find_library(MPV_LIBRARIES …)`,
`find_package(taglib …)`, and the `${…_LIBRARIES}` / `${…_INCLUDE_DIRS}` /
`target_link_directories` entries that fed them.

Replaced by the in-tree targets:

| Old | New |
| --- | --- |
| `${SDL3_LIBRARIES}` | `SDL3::SDL3` |
| `${VULKAN_LIBRARIES}` | `vulkan` (from-source loader) |
| `${MPV_LIBRARIES}` | `mpv::mpv` |
| `${LIBPLACEBO_LIBRARIES}` | `placebo::placebo` |
| `TagLib::tag` (vcpkg) | `tag` (in-tree) |

Includes arrive transitively from the targets where possible (the bundled
`external/mpv/include` and `external/libplacebo/src/include` header shims become
unnecessary once the real installed headers are on the target's interface).

`-rdynamic` on the executable stays (re-exports app + static-lib frames). The
from-source shared libs (`SDL3`, `vulkan`, `mpv`, `placebo`) carry their own
`.symtab` from `-g`/no-strip.

## 6. Build order, toolchain, multi-config

- **Dependency order:** `FFmpeg → mpv` (mpv's meson must resolve our FFmpeg);
  `Vulkan-Headers → Vulkan-Loader`; libplacebo before mpv if mpv consumes our
  libplacebo. SDL3, taglib, imgui/implot/stb independent. The exe `add_dependencies`
  on every ExternalProject so foreign installs exist before link.
- **Toolchain:** clang/clang++ + LLD for **all** configs (standing project
  preference). `add_subdirectory` libs inherit it; FFmpeg gets `--cc=clang`; meson
  builds get a native file selecting clang + `c_ld=lld`.
- **Multi-config (`all` preset / Ninja Multi-Config):** `add_subdirectory` libs
  build **per-config** (Debug/Release/RelWithDebInfo). FFmpeg/libplacebo/mpv build
  **once** (config-agnostic, debug, unstripped) and link into all three app configs
  — correct for C/C++ ABI-stable shared deps and avoids 3× the long foreign builds.

## 7. Risks & mitigations

| Risk | Mitigation |
| --- | --- |
| First FFmpeg+mpv+libplacebo build is minutes-long | ExternalProject stamps cache it; rebuilds only on source/option change |
| libplacebo glad/header mismatch vs SDK 1.4.350 | Using **pinned submodule commits** (the set libplacebo was tested against), not bleeding-edge — already cloned |
| Forced default visibility doesn't fully un-hide SDL/loader/FFmpeg internals | `-g` + no-strip guarantees profilability via `.symtab`/DWARF regardless |
| From-source Vulkan loader can't find driver at runtime | Loader reads system `/usr/share/vulkan/icd.d`; layers via `VK_LAYER_PATH` → SDK layer dir |
| SDL 3.4.10 vs system 3.5.0 behavior drift | Minor version; app's SDL surface is stable; revert path = re-point to system if needed |
| ExternalProject + multi-config quirks | Foreign deps built single-config by design; only CMake-native deps are per-config |

## 8. Validation / acceptance

1. `cmake --workflow --preset all` (or per-config build) completes, building all
   nine from-source deps + the exe, with **no** reference to system SDL3 / Vulkan /
   mpv / libplacebo / vcpkg taglib remaining in the link line.
2. `ldd build/<cfg>/example_sdl3_vulkan_<cfg>` resolves `libSDL3`, `libvulkan`,
   `libmpv`, `libplacebo` to the **`build/thirdparty/…`** copies (not `/usr/...`).
3. The app launches; a video plays on Path A (and Path B under `IMGUI_USE_VPP=1`).
4. Symbol check: `nm -C build/thirdparty/mpv/install/lib/libmpv.so | head` shows
   named symbols; a `perf`/lldb sample resolves frames into mpv/SDL/FFmpeg source.

## 9. Phasing (suggested implementation order)

1. **Refactor existing** imgui/implot/stb into `external/<lib>/CMakeLists.txt` +
   `build/thirdparty/` outputs — zero behavior change, establishes the pattern.
2. **SDL3** from source (clean native CMake, isolated swap).
3. **taglib** from source (clean native CMake, drops a vcpkg dep).
4. **Vulkan-Loader** from source (+ Vulkan-Headers).
5. **FFmpeg** ExternalProject (foundation for mpv).
6. **libplacebo** ExternalProject.
7. **mpv** ExternalProject (consumes 5 + optionally 6); final link-line swap.

Each step builds + runs before the next, so a failure is isolated to one library.
