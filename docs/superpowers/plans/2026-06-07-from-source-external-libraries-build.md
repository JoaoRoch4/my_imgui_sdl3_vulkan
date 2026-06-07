# From-Source External Libraries (Per-Folder Build) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile SDL3, the Vulkan loader, FFmpeg, mpv, libplacebo and taglib from source inside this project (and lift the existing imgui/implot/stb builds into the same pattern), so every compiled dependency lives in its own folder and carries debug symbols for profiling.

**Architecture:** The root `CMakeLists.txt` becomes a thin orchestrator that `include()`s a profilable-treatment helper and `add_subdirectory()`s one tracked `thirdparty/<lib>/CMakeLists.txt` per dependency. CMake-native libs (imgui/implot/stb/SDL3/taglib/Vulkan-Loader) are built as in-graph targets; foreign autotools/meson libs (FFmpeg/libplacebo/mpv) hide behind `ExternalProject_Add` wrappers that install into `build/all/thirdparty/<lib>/install` and are consumed as IMPORTED targets. The app links the resulting targets instead of system/vcpkg libs.

**Tech Stack:** CMake ≥3.20 (Ninja Multi-Config), clang/clang++ + LLD, pkg-config, meson/ninja, autotools (FFmpeg), ExternalProject.

> **Spec:** `docs/superpowers/specs/2026-06-07-from-source-external-libraries-build-design.md`.
> **Refinement vs. spec:** the spec placed each library's CMakeLists in `external/<lib>/`. Because `.gitignore` ends with `external/*` (vendored sources are intentionally untracked), the *authored build glue* instead lives in a tracked top-level **`thirdparty/<lib>/`** folder that builds from the (ignored) `external/<lib>/` source. Same "one folder per library" principle, correct git story.

> **Source state (already done):** `external/Vulkan-Loader` (v1.4.350) and `external/Vulkan-Headers` (v1.4.350) are cloned; `external/libplacebo/3rdparty/{glad,jinja,markupsafe,Vulkan-Headers,fast_float}` submodules are populated at pinned commits; `external/SDL3-3.4.10`, `external/taglib`, `external/FFmpeg`, `external/mpv` sources are present.

## Conventions used by every task

- **Configure:** `cmake --preset all` (Ninja Multi-Config → `build/all`, clang/LLD pinned by the preset).
- **Build one config:** `cmake --build build/all --config Debug` (exe → `build/debug/example_sdl3_vulkan_debug`).
- **Smoke-run:** `timeout 8 ./build/debug/example_sdl3_vulkan_debug || true` (launches the window; we only confirm it starts + links).
- **Tests:** `cmake --build build/all --config Debug --target image_tests image_job_tests && ctest --test-dir build/all -C Debug`.
- After each phase the app must **configure, build (Debug), and launch** before moving on.

---

## Task 0: Scaffolding — profilable helper + orchestrator prep

**Files:**
- Create: `cmake/ProfilableTreatment.cmake`
- Create: `thirdparty/README.md`
- Modify: `CMakeLists.txt` (add `include()` of the helper near the top)

- [ ] **Step 1: Create the profilable-treatment helper**

Create `cmake/ProfilableTreatment.cmake`:

```cmake
# Applies the uniform "profilable" treatment to a CMake target we build from
# source: default symbol visibility (names survive into .dynsym), DWARF debug
# info in every config (so a sampling profiler maps addresses -> source), and
# kept frame pointers (exact stack unwinding). Mirrors the historical stb/imgui
# treatment. Safe to call on C or C++ targets.
function(app_profilable_treatment tgt)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "app_profilable_treatment: '${tgt}' is not a target")
  endif()
  set_target_properties(${tgt} PROPERTIES
    C_VISIBILITY_PRESET       default
    CXX_VISIBILITY_PRESET     default
    VISIBILITY_INLINES_HIDDEN OFF)
  target_compile_options(${tgt} PRIVATE
    -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
endfunction()
```

- [ ] **Step 2: Record how the vendored/cloned sources are obtained**

Create `thirdparty/README.md`:

```markdown
# thirdparty/ — per-library build glue

Each subfolder holds the **authored CMake glue** to build one external library
from source. The library *sources* live under the git-ignored `external/<lib>/`.

Cloned (not vendored) sources — re-fetch with:

    git -C external/libplacebo submodule update --init --depth 1 \
      3rdparty/glad 3rdparty/jinja 3rdparty/markupsafe \
      3rdparty/Vulkan-Headers 3rdparty/fast_float
    git clone --depth 1 --branch vulkan-sdk-1.4.350.0 \
      https://github.com/KhronosGroup/Vulkan-Loader.git  external/Vulkan-Loader
    git clone --depth 1 --branch vulkan-sdk-1.4.350.0 \
      https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers

Build outputs go to `build/all/thirdparty/<lib>/` (git-ignored).
```

- [ ] **Step 3: Include the helper in the root CMakeLists**

In `CMakeLists.txt`, immediately after the `project(example_sdl3_vulkan CXX)` line (line 16), add:

```cmake
# Per-library build glue lives under thirdparty/<lib>/; shared helper below.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(ProfilableTreatment)
```

- [ ] **Step 4: Configure to verify the include resolves**

Run: `cmake --preset all`
Expected: configures with no error about `ProfilableTreatment`. (App still builds the old inline way — unchanged so far.)

- [ ] **Step 5: Commit**

```bash
git add cmake/ProfilableTreatment.cmake thirdparty/README.md CMakeLists.txt
git commit -m "build: add profilable-treatment helper + thirdparty/ scaffolding"
```

---

## Task 1: Lift imgui / implot / stb into thirdparty/ (zero behavior change)

The current root inlines these three (lines ~59–184). We move each into its own
`thirdparty/<lib>/CMakeLists.txt`, keeping the **same** pkg-config-derived include
dirs (still inherited from the root scope at this phase), and apply the helper.

**Files:**
- Create: `thirdparty/imgui/CMakeLists.txt`
- Create: `thirdparty/implot/CMakeLists.txt`
- Create: `thirdparty/stb/CMakeLists.txt`
- Modify: `CMakeLists.txt` (delete inline blocks; add `add_subdirectory` calls)

- [ ] **Step 1: Create `thirdparty/imgui/CMakeLists.txt`**

```cmake
# Dear ImGui (docking) + SDL3/Vulkan backends + FreeType/PlutoSVG rasteriser,
# built once into a static lib. Include dirs for SDL3/Vulkan/FreeType/PlutoSVG
# are inherited from the root scope (pkg_check_modules runs before this dir is
# added). Later phases swap SDL3/Vulkan to from-source targets.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui-1.92.8-docking)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)

target_include_directories(imgui SYSTEM PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
)
target_include_directories(imgui SYSTEM PRIVATE
    ${SDL3_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
)
target_compile_definitions(imgui PRIVATE IMGUI_ENABLE_FREETYPE_PLUTOSVG)
target_compile_options(imgui PRIVATE
    ${SDL3_CFLAGS_OTHER}
    ${VULKAN_CFLAGS_OTHER}
    ${FREETYPE_CFLAGS_OTHER}
    ${PLUTOSVG_CFLAGS_OTHER}
    -w
)
app_profilable_treatment(imgui)
```

- [ ] **Step 2: Create `thirdparty/implot/CMakeLists.txt`**

```cmake
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/external/implot)
add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(implot SYSTEM PUBLIC ${IMPLOT_DIR})
target_link_libraries(implot PUBLIC imgui)
target_compile_options(implot PRIVATE -w)
app_profilable_treatment(implot)
```

- [ ] **Step 3: Create `thirdparty/stb/CMakeLists.txt`**

```cmake
# stb single-file implementations compiled into ONE standalone lib so a profiler
# attributes samples to named stb_* frames. The TU is generated into the build
# tree (only rewritten on content change) and compiled as C++ to match the
# mangled names the C++ consumers reference.
set(STB_DIR ${CMAKE_SOURCE_DIR}/external/stb)
set(STB_IMPL_TU ${CMAKE_BINARY_DIR}/stb/stb_impl.cpp)
file(GENERATE OUTPUT ${STB_IMPL_TU} CONTENT [[// Auto-generated by thirdparty/stb/CMakeLists.txt — do not edit.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize2.h>
]])
set_source_files_properties(${STB_IMPL_TU} PROPERTIES GENERATED TRUE)

add_library(stb STATIC ${STB_IMPL_TU})
target_include_directories(stb SYSTEM PUBLIC ${STB_DIR})
target_compile_options(stb PRIVATE -w)
app_profilable_treatment(stb)
```

- [ ] **Step 4: Delete the inline blocks in `CMakeLists.txt`**

Remove from `CMakeLists.txt`:
- The `IMGUI_DIR` / `IMPLOT_DIR` / `STB_DIR` `set(...)` lines (lines 27–32).
- The entire `add_library(imgui …)` … through the `add_library(implot …)` block
  … through the `add_library(stb …)` block and the `foreach(_prof_lib stb imgui implot)`
  treatment loop (the whole region lines ~59–184).

- [ ] **Step 5: Add the subdirectories in `CMakeLists.txt`**

Immediately **after** the dependency-discovery block (after `find_library(MPV_LIBRARIES …)`, ~line 57) — so the pkg-config vars exist in scope — add:

```cmake
# ── Compiled external libraries (each in its own thirdparty/<lib> folder) ──────
add_subdirectory(thirdparty/imgui)
add_subdirectory(thirdparty/implot)
add_subdirectory(thirdparty/stb)
```

- [ ] **Step 6: Configure + build Debug**

Run: `cmake --preset all && cmake --build build/all --config Debug`
Expected: builds `imgui`, `implot`, `stb` (now under `build/all/thirdparty/...` object dirs) and links the exe exactly as before. No new warnings.

- [ ] **Step 7: Smoke-run**

Run: `timeout 8 ./build/debug/example_sdl3_vulkan_debug || true`
Expected: window opens, no missing-symbol/link errors.

- [ ] **Step 8: Commit**

```bash
git add thirdparty/imgui thirdparty/implot thirdparty/stb CMakeLists.txt
git commit -m "build: move imgui/implot/stb into per-folder thirdparty builds"
```

---

## Task 2: SDL3 from source

**Files:**
- Create: `thirdparty/sdl3/CMakeLists.txt`
- Modify: `CMakeLists.txt` (drop `pkg_check_modules(SDL3 …)`; add subdir; swap link)
- Modify: `thirdparty/imgui/CMakeLists.txt` (headers from `SDL3::SDL3` target)

- [ ] **Step 1: Create `thirdparty/sdl3/CMakeLists.txt`**

```cmake
# Build SDL3 (3.4.10) from external/SDL3-3.4.10 as a shared lib with debug
# symbols. Output target: SDL3::SDL3.
set(SDL_SHARED       ON  CACHE BOOL "" FORCE)
set(SDL_STATIC       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/SDL3-3.4.10
    ${CMAKE_BINARY_DIR}/thirdparty/sdl3
    EXCLUDE_FROM_ALL)
app_profilable_treatment(SDL3-shared)
```

- [ ] **Step 2: Wire it into the root, before the imgui subdir**

In `CMakeLists.txt`, replace `pkg_check_modules(SDL3 REQUIRED sdl3)` (line 38) with nothing (delete it), and add — **before** `add_subdirectory(thirdparty/imgui)`:

```cmake
add_subdirectory(thirdparty/sdl3)
```

- [ ] **Step 3: Point imgui at the SDL3 target**

In `thirdparty/imgui/CMakeLists.txt`, delete `${SDL3_INCLUDE_DIRS}` from the SYSTEM PRIVATE include list and `${SDL3_CFLAGS_OTHER}` from compile options, then add:

```cmake
target_link_libraries(imgui PRIVATE SDL3::SDL3)
```

- [ ] **Step 4: Swap the app link + includes + link dirs**

In `CMakeLists.txt`:
- In `target_link_libraries(example_sdl3_vulkan PRIVATE …)`: replace `${SDL3_LIBRARIES}` with `SDL3::SDL3`.
- In `target_include_directories(example_sdl3_vulkan SYSTEM PRIVATE …)`: delete `${SDL3_INCLUDE_DIRS}`.
- In `target_link_directories(…)`: delete `${SDL3_LIBRARY_DIRS}`.
- In `target_compile_options(…)`: delete `${SDL3_CFLAGS_OTHER}`.

- [ ] **Step 5: Configure + build Debug**

Run: `cmake --preset all && cmake --build build/all --config Debug`
Expected: SDL3 builds from source; exe links `SDL3::SDL3`.

- [ ] **Step 6: Verify the app links the from-source SDL3**

Run: `ldd build/debug/example_sdl3_vulkan_debug | grep -i sdl3`
Expected: resolves to `build/all/thirdparty/sdl3/...libSDL3.so*` (NOT `/usr/local/lib64`).

- [ ] **Step 7: Smoke-run, then commit**

Run: `timeout 8 ./build/debug/example_sdl3_vulkan_debug || true` (window opens).

```bash
git add thirdparty/sdl3 thirdparty/imgui/CMakeLists.txt CMakeLists.txt
git commit -m "build: compile SDL3 from source, drop system sdl3"
```

---

## Task 3: taglib from source

**Files:**
- Create: `thirdparty/taglib/CMakeLists.txt`
- Modify: `CMakeLists.txt` (drop `find_package(taglib)`; add subdir; swap `TagLib::tag` → `tag`)

- [ ] **Step 1: Create `thirdparty/taglib/CMakeLists.txt`**

```cmake
# Build TagLib from external/taglib as a static lib. Output target: tag.
set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS     OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/taglib
    ${CMAKE_BINARY_DIR}/thirdparty/taglib
    EXCLUDE_FROM_ALL)
app_profilable_treatment(tag)
```

- [ ] **Step 2: Wire into root + swap link names**

In `CMakeLists.txt`:
- Delete `find_package(taglib REQUIRED)` (line 50).
- Add `add_subdirectory(thirdparty/taglib)` alongside the other thirdparty adds.
- In `target_link_libraries(example_sdl3_vulkan PRIVATE …)`: replace `TagLib::tag` with `tag`.
- In `target_link_libraries(image_job_tests PRIVATE …)`: replace `TagLib::tag` with `tag`.

- [ ] **Step 3: Build Debug + tests**

Run:
```bash
cmake --preset all
cmake --build build/all --config Debug --target example_sdl3_vulkan image_job_tests
ctest --test-dir build/all -C Debug
```
Expected: exe + tests build; `image_job_tests` (which links `tag` for headers) passes.

- [ ] **Step 4: Smoke-run, then commit**

```bash
git add thirdparty/taglib CMakeLists.txt
git commit -m "build: compile TagLib from source, drop vcpkg taglib"
```

---

## Task 4: Vulkan loader from source

**Files:**
- Create: `thirdparty/vulkan-loader/CMakeLists.txt`
- Modify: `CMakeLists.txt` (drop `pkg_check_modules(VULKAN)`; add subdir; swap link/includes)
- Modify: `thirdparty/imgui/CMakeLists.txt` (Vulkan headers from `Vulkan::Headers`)

- [ ] **Step 1: Create `thirdparty/vulkan-loader/CMakeLists.txt`**

```cmake
# Build the Vulkan loader (libvulkan.so) from external/Vulkan-Loader (v1.4.350)
# against external/Vulkan-Headers (same tag). Vulkan-Headers provides the
# Vulkan::Headers INTERFACE target; the loader's find_package(VulkanHeaders ...
# QUIET) is satisfied by that already-defined target. Output targets: vulkan
# (loader) + Vulkan::Headers (API headers).
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Headers
    ${CMAKE_BINARY_DIR}/thirdparty/vulkan-headers
    EXCLUDE_FROM_ALL)

set(BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(BUILD_WSI_XCB_SUPPORT     ON CACHE BOOL "" FORCE)
set(BUILD_WSI_XLIB_SUPPORT    ON CACHE BOOL "" FORCE)
set(BUILD_WSI_WAYLAND_SUPPORT ON CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Loader
    ${CMAKE_BINARY_DIR}/thirdparty/vulkan-loader
    EXCLUDE_FROM_ALL)
app_profilable_treatment(vulkan)
```

- [ ] **Step 2: Wire into root + swap**

In `CMakeLists.txt`:
- Delete `pkg_check_modules(VULKAN REQUIRED vulkan)` (line 39).
- Add `add_subdirectory(thirdparty/vulkan-loader)` **before** `add_subdirectory(thirdparty/imgui)`.
- In `target_link_libraries(example_sdl3_vulkan PRIVATE …)`: replace `${VULKAN_LIBRARIES}` with `vulkan Vulkan::Headers`.
- In `target_include_directories(example_sdl3_vulkan SYSTEM PRIVATE …)`: delete `${VULKAN_INCLUDE_DIRS}`.
- In `target_link_directories(…)`: delete `${VULKAN_LIBRARY_DIRS}`.
- In `target_compile_options(…)`: delete `${VULKAN_CFLAGS_OTHER}`.

- [ ] **Step 3: Point imgui at the Vulkan headers target**

In `thirdparty/imgui/CMakeLists.txt`, delete `${VULKAN_INCLUDE_DIRS}` and `${VULKAN_CFLAGS_OTHER}`, and extend the link line:

```cmake
target_link_libraries(imgui PRIVATE SDL3::SDL3 Vulkan::Headers)
```

- [ ] **Step 4: Build Debug + verify loader provenance**

Run: `cmake --preset all && cmake --build build/all --config Debug`
Then: `ldd build/debug/example_sdl3_vulkan_debug | grep -i vulkan`
Expected: resolves to `build/all/thirdparty/vulkan-loader/...libvulkan.so*` (NOT `/usr/lib64`).

- [ ] **Step 5: Smoke-run (validates the loader finds the system ICD)**

Run: `timeout 8 ./build/debug/example_sdl3_vulkan_debug || true`
Expected: Vulkan window renders; the from-source loader discovers the system driver via `/usr/share/vulkan/icd.d`. (Validation layers in Debug load from the SDK via `VK_LAYER_PATH` — set it if layers are wanted: `VK_LAYER_PATH=external/vulkan-sdk/1.4.350.1/x86_64/lib`.)

- [ ] **Step 6: Commit**

```bash
git add thirdparty/vulkan-loader thirdparty/imgui/CMakeLists.txt CMakeLists.txt
git commit -m "build: compile Vulkan loader from source, drop system vulkan"
```

---

## Task 5: FFmpeg from source (ExternalProject) — foundation for mpv

FFmpeg is consumed by the mpv build only; it does **not** go on the app's link line.

**Files:**
- Create: `thirdparty/ffmpeg/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add subdir)

- [ ] **Step 1: Create `thirdparty/ffmpeg/CMakeLists.txt`**

```cmake
# Build FFmpeg 8.0.git from external/FFmpeg via its autotools configure, once,
# with debug symbols and no stripping. Installs into
# build/all/thirdparty/ffmpeg/install; consumed by the mpv build via
# PKG_CONFIG_PATH. Exposes the ffmpeg::* IMPORTED targets the app does not link
# directly but mpv depends on.
include(ExternalProject)

set(FFMPEG_PREFIX  ${CMAKE_BINARY_DIR}/thirdparty/ffmpeg)
set(FFMPEG_INSTALL ${FFMPEG_PREFIX}/install)
set(FFMPEG_PKGCONFIG ${FFMPEG_INSTALL}/lib/pkgconfig)

ExternalProject_Add(ffmpeg_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/FFmpeg
    PREFIX            ${FFMPEG_PREFIX}
    BINARY_DIR        ${FFMPEG_PREFIX}/build
    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/external/FFmpeg/configure
                        --prefix=${FFMPEG_INSTALL}
                        --cc=clang --cxx=clang++
                        --enable-shared --disable-static
                        --enable-gpl --enable-version3
                        --disable-programs --disable-doc --disable-htmlpages
                        --disable-manpages --disable-podpages --disable-txtpages
                        --disable-stripping --enable-debug=3
                        --extra-cflags=-fno-omit-frame-pointer
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_BYPRODUCTS
        ${FFMPEG_INSTALL}/lib/libavcodec.so
        ${FFMPEG_INSTALL}/lib/libavformat.so
        ${FFMPEG_INSTALL}/lib/libavutil.so
        ${FFMPEG_INSTALL}/lib/libswscale.so
        ${FFMPEG_INSTALL}/lib/libswresample.so
        ${FFMPEG_INSTALL}/lib/libavfilter.so
    USES_TERMINAL_BUILD TRUE)

# Make the install include dir exist at configure time so IMPORTED targets are valid.
file(MAKE_DIRECTORY ${FFMPEG_INSTALL}/include)

# Expose the FFmpeg pkgconfig dir + install prefix to siblings (mpv) via a cache var.
set(FFMPEG_PKGCONFIG_DIR ${FFMPEG_PKGCONFIG} CACHE INTERNAL "FFmpeg .pc dir")
set(FFMPEG_INSTALL_DIR   ${FFMPEG_INSTALL}   CACHE INTERNAL "FFmpeg install prefix")

# IMPORTED targets (used transitively by mpv's link interface if needed).
foreach(_lib avcodec avformat avutil swscale swresample avfilter)
    add_library(ffmpeg::${_lib} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${_lib} PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_INSTALL}/lib/lib${_lib}.so
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL}/include)
    add_dependencies(ffmpeg::${_lib} ffmpeg_ep)
endforeach()
```

- [ ] **Step 2: Wire into root (build target only; no app link)**

In `CMakeLists.txt`, add near the other thirdparty adds:

```cmake
add_subdirectory(thirdparty/ffmpeg)
```

- [ ] **Step 3: Build FFmpeg alone and verify install**

Run: `cmake --preset all && cmake --build build/all --config Debug --target ffmpeg_ep`
Then: `ls build/all/thirdparty/ffmpeg/install/lib/libav*.so`
Expected: `libavcodec.so libavformat.so libavutil.so …` exist. (First build is minutes-long.)

- [ ] **Step 4: Verify pkg-config is queryable for mpv**

Run: `PKG_CONFIG_PATH=build/all/thirdparty/ffmpeg/install/lib/pkgconfig pkg-config --modversion libavcodec`
Expected: prints a 61.x/62.x version from the from-source build.

- [ ] **Step 5: Commit**

```bash
git add thirdparty/ffmpeg CMakeLists.txt
git commit -m "build: compile FFmpeg from source (ExternalProject) for mpv"
```

---

## Task 6: libplacebo from source (ExternalProject, meson)

**Files:**
- Create: `thirdparty/libplacebo/meson-clang-native.ini`
- Create: `thirdparty/libplacebo/CMakeLists.txt`
- Modify: `CMakeLists.txt` (drop `pkg_check_modules(LIBPLACEBO)`; add subdir; swap link/includes)

- [ ] **Step 1: Create the meson clang native file**

Create `thirdparty/libplacebo/meson-clang-native.ini`:

```ini
[binaries]
c = 'clang'
cpp = 'clang++'
c_ld = 'lld'
cpp_ld = 'lld'
```

- [ ] **Step 2: Create `thirdparty/libplacebo/CMakeLists.txt`**

```cmake
# Build libplacebo from external/libplacebo (meson) once, with debug symbols,
# using clang/LLD. Submodules (glad/Vulkan-Headers/jinja/markupsafe/fast_float)
# are already populated at pinned commits. Installs into
# build/all/thirdparty/libplacebo/install. Output target: placebo::placebo.
include(ExternalProject)

set(PLACEBO_PREFIX  ${CMAKE_BINARY_DIR}/thirdparty/libplacebo)
set(PLACEBO_INSTALL ${PLACEBO_PREFIX}/install)
set(PLACEBO_NATIVE  ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)

ExternalProject_Add(libplacebo_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/libplacebo
    PREFIX            ${PLACEBO_PREFIX}
    CONFIGURE_COMMAND meson setup ${PLACEBO_PREFIX}/build
                        ${CMAKE_SOURCE_DIR}/external/libplacebo
                        --native-file ${PLACEBO_NATIVE}
                        --prefix ${PLACEBO_INSTALL}
                        --libdir lib
                        --buildtype debugoptimized
                        -Db_ndebug=false -Dstrip=false
                        -Ddemos=false -Dtests=false
                        -Dc_args=-fno-omit-frame-pointer
    BUILD_COMMAND     ninja -C ${PLACEBO_PREFIX}/build
    INSTALL_COMMAND   ninja -C ${PLACEBO_PREFIX}/build install
    BUILD_BYPRODUCTS  ${PLACEBO_INSTALL}/lib/libplacebo.so
    USES_TERMINAL_BUILD TRUE)

file(MAKE_DIRECTORY ${PLACEBO_INSTALL}/include)
set(PLACEBO_PKGCONFIG_DIR ${PLACEBO_INSTALL}/lib/pkgconfig CACHE INTERNAL "libplacebo .pc dir")

add_library(placebo::placebo SHARED IMPORTED GLOBAL)
set_target_properties(placebo::placebo PROPERTIES
    IMPORTED_LOCATION ${PLACEBO_INSTALL}/lib/libplacebo.so
    INTERFACE_INCLUDE_DIRECTORIES ${PLACEBO_INSTALL}/include)
add_dependencies(placebo::placebo libplacebo_ep)
```

- [ ] **Step 3: Wire into root + swap the app link**

In `CMakeLists.txt`:
- Delete `pkg_check_modules(LIBPLACEBO REQUIRED libplacebo)` (line 44) and the `set(LIBPLACEBO_INCLUDE_DIRS …)` shim (line 56).
- Add `add_subdirectory(thirdparty/libplacebo)`.
- In `target_link_libraries(example_sdl3_vulkan PRIVATE …)`: replace `${LIBPLACEBO_LIBRARIES}` with `placebo::placebo`.
- In `target_include_directories(example_sdl3_vulkan SYSTEM PRIVATE …)`: delete `${LIBPLACEBO_INCLUDE_DIRS}`.
- In `target_link_directories(…)`: delete `${LIBPLACEBO_LIBRARY_DIRS}`.

- [ ] **Step 4: Build libplacebo + the app**

Run: `cmake --preset all && cmake --build build/all --config Debug`
Then: `ldd build/debug/example_sdl3_vulkan_debug | grep -i placebo`
Expected: resolves to `build/all/thirdparty/libplacebo/install/lib/libplacebo.so` (NOT `/usr/lib64`).

- [ ] **Step 5: Smoke-run (Path B uses libplacebo), then commit**

Run: `IMGUI_USE_VPP=1 timeout 8 ./build/debug/example_sdl3_vulkan_debug || true`

```bash
git add thirdparty/libplacebo CMakeLists.txt
git commit -m "build: compile libplacebo from source, drop system libplacebo"
```

---

## Task 7: mpv from source (ExternalProject, meson) + final link swap

**Files:**
- Create: `thirdparty/mpv/meson-clang-native.ini`
- Create: `thirdparty/mpv/CMakeLists.txt`
- Modify: `CMakeLists.txt` (drop `find_library(MPV)` + `MPV_INCLUDE_DIRS` shim; add subdir; swap link)

- [ ] **Step 1: Create the meson clang native file**

Create `thirdparty/mpv/meson-clang-native.ini`:

```ini
[binaries]
c = 'clang'
cpp = 'clang++'
c_ld = 'lld'
cpp_ld = 'lld'
```

- [ ] **Step 2: Create `thirdparty/mpv/CMakeLists.txt`**

```cmake
# Build libmpv from external/mpv (meson) once, with debug symbols, against our
# from-source FFmpeg (and from-source libplacebo) discovered via PKG_CONFIG_PATH.
# Installs into build/all/thirdparty/mpv/install. Output target: mpv::mpv.
include(ExternalProject)

set(MPV_PREFIX  ${CMAKE_BINARY_DIR}/thirdparty/mpv)
set(MPV_INSTALL ${MPV_PREFIX}/install)
set(MPV_NATIVE  ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)

# Resolve our sibling install prefixes (set as INTERNAL cache vars by their dirs).
set(_pc_path "${FFMPEG_PKGCONFIG_DIR}:${PLACEBO_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")

ExternalProject_Add(mpv_ep
    DEPENDS           ffmpeg_ep libplacebo_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/mpv
    PREFIX            ${MPV_PREFIX}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${_pc_path}
                        meson setup ${MPV_PREFIX}/build
                        ${CMAKE_SOURCE_DIR}/external/mpv
                        --native-file ${MPV_NATIVE}
                        --prefix ${MPV_INSTALL}
                        --libdir lib
                        --buildtype debugoptimized
                        -Db_ndebug=false -Dstrip=false
                        -Dlibmpv=true -Dcplayer=false
                        -Dlua=disabled
                        -Dc_args=-fno-omit-frame-pointer
    BUILD_COMMAND     ninja -C ${MPV_PREFIX}/build
    INSTALL_COMMAND   ninja -C ${MPV_PREFIX}/build install
    BUILD_BYPRODUCTS  ${MPV_INSTALL}/lib/libmpv.so
    USES_TERMINAL_BUILD TRUE)

file(MAKE_DIRECTORY ${MPV_INSTALL}/include)

add_library(mpv::mpv SHARED IMPORTED GLOBAL)
set_target_properties(mpv::mpv PROPERTIES
    IMPORTED_LOCATION ${MPV_INSTALL}/lib/libmpv.so
    INTERFACE_INCLUDE_DIRECTORIES ${MPV_INSTALL}/include)
add_dependencies(mpv::mpv mpv_ep)
```

- [ ] **Step 3: Wire into root + swap the app link**

In `CMakeLists.txt`:
- Delete `find_library(MPV_LIBRARIES NAMES mpv REQUIRED)` (line 57) and the `set(MPV_INCLUDE_DIRS …)` shim (line 54).
- Add `add_subdirectory(thirdparty/mpv)` **after** `add_subdirectory(thirdparty/ffmpeg)` and `add_subdirectory(thirdparty/libplacebo)`.
- In `target_link_libraries(example_sdl3_vulkan PRIVATE …)`: replace `${MPV_LIBRARIES}` with `mpv::mpv`.
- In `target_include_directories(example_sdl3_vulkan SYSTEM PRIVATE …)`: delete `${MPV_INCLUDE_DIRS}` (the bundled `external/mpv/include` shim — real headers now come from the target).

- [ ] **Step 4: Build everything (Debug) and verify mpv provenance**

Run: `cmake --preset all && cmake --build build/all --config Debug`
Then: `ldd build/debug/example_sdl3_vulkan_debug | grep -iE 'mpv|avcodec'`
Expected: `libmpv.so` resolves to `build/all/thirdparty/mpv/install/...`; mpv in turn pulls our `libavcodec.so` from the FFmpeg install (check with `ldd build/all/thirdparty/mpv/install/lib/libmpv.so | grep avcodec`).

- [ ] **Step 5: Smoke-run video playback (both paths)**

Run:
```bash
timeout 8 ./build/debug/example_sdl3_vulkan_debug || true          # Path A
IMGUI_USE_VPP=1 timeout 8 ./build/debug/example_sdl3_vulkan_debug || true  # Path B
```
Expected: app launches; opening a video decodes/plays via the from-source libmpv.

- [ ] **Step 6: Commit**

```bash
git add thirdparty/mpv CMakeLists.txt
git commit -m "build: compile mpv from source against from-source FFmpeg, drop system mpv"
```

---

## Task 8: Full multi-config verification + symbol check

**Files:** none (verification only)

- [ ] **Step 1: Build all three configs**

Run: `cmake --workflow --preset all`
Expected: Debug, Release, RelWithDebInfo all configure + build. (Foreign deps build once, then link into all three.)

- [ ] **Step 2: Confirm NO system/vcpkg provenance remains for the swapped libs**

Run:
```bash
for cfg in debug release release-log; do
  echo "== $cfg =="; ldd build/$cfg/example_sdl3_vulkan_$cfg \
    | grep -iE 'sdl3|vulkan|libmpv|placebo' ;
done
```
Expected: every match points under `build/all/thirdparty/...`, none under `/usr` or `/usr/local`.

- [ ] **Step 3: Confirm symbols are profilable**

Run:
```bash
nm -C --defined-only build/all/thirdparty/mpv/install/lib/libmpv.so | head
file build/all/thirdparty/sdl3/*/libSDL3.so*
```
Expected: `libmpv.so` lists named symbols; `file` reports the SDL3 `.so` is **not stripped** ("with debug_info, not stripped").

- [ ] **Step 4: Run the test suite in Release**

Run: `cmake --build build/all --config Release --target image_tests image_job_tests && ctest --test-dir build/all -C Release`
Expected: all tests pass.

- [ ] **Step 5: Final commit (if any cleanup)**

```bash
git add -A
git commit -m "build: verify full from-source multi-config build + symbols" || true
```

---

## Self-review notes (coverage vs. spec)

- Spec §3 (orchestrator + per-folder) → Tasks 0–7 (each lib its own `thirdparty/<lib>/`).
- Spec §3.2 (profilable recipe) → `app_profilable_treatment` (CMake libs) + `--enable-debug=3 --disable-stripping` (FFmpeg) + `--buildtype debugoptimized -Dstrip=false` (meson) in Tasks 5–7.
- Spec §5 (link-line swap) → Tasks 2/3/4/6/7 each delete one system/vcpkg lookup and swap to a target; `-rdynamic` untouched.
- Spec §6 (build order) → FFmpeg (Task 5) before mpv (Task 7, `DEPENDS ffmpeg_ep libplacebo_ep`); Vulkan-Headers before loader (Task 4).
- Spec §8 (acceptance) → Task 8 (multi-config build, ldd provenance, nm/unstripped symbol check, tests).
- Out-of-scope (WebP/CURL/reflectcpp stay vcpkg; validation layers stay SDK prebuilt) → untouched; no task references them.
