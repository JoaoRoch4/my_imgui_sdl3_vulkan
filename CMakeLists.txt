cmake_minimum_required(VERSION 3.20)

# ── vcpkg ─────────────────────────────────────────────────────────────────────
# Disable manifest mode so vcpkg doesn't auto-install during configure.
# Toolchain defaults to the global vcpkg install; can be overridden via -D.
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "" FORCE)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "/home/joao/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Search both the global vcpkg prefix and the project-local vcpkg overlay.
list(APPEND CMAKE_PREFIX_PATH "/home/joao/vcpkg/installed/x64-linux")
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")

# ── Project ───────────────────────────────────────────────────────────────────
project(example_sdl3_vulkan CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Append _d to the executable name for Debug builds so debug and release
# binaries can coexist without overwriting each other.
set(CMAKE_DEBUG_POSTFIX "_d")

# ── External library paths ────────────────────────────────────────────────────
# ImGui docking/viewports fork lives outside this repo.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui-1.92.8-docking)
# ImPlot is vendored under external/ inside this repo.
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/external/implot)

# ── Dependency discovery ──────────────────────────────────────────────────────
# System libraries resolved via pkg-config (SDL3, Vulkan, Freetype2, PlutoSVG, mpv).
# vcpkg-managed libraries use CMake find_package (reflectcpp, Stb, WebP, CURL).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(PLUTOSVG REQUIRED plutosvg)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(LIBPLACEBO REQUIRED libplacebo)
find_package(reflectcpp CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(taglib REQUIRED)

# Use bundled mpv headers from external/mpv/include instead of system headers.
# The shared library is still resolved from the system at link time.
set(MPV_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/external/mpv/include)
# Use vendored libplacebo headers for early integration scaffolding.
set(LIBPLACEBO_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/external/libplacebo/src/include)
find_library(MPV_LIBRARIES NAMES mpv REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(example_sdl3_vulkan

    # Entry point & main loop
    code/main/main.cpp
    code/main/app.cpp

    # Core utilities
    code/core/app_context.cpp
    code/core/thread/thread_overwatch.cpp
    # Rendering layer (SDL3 window + ImGui + Vulkan backend)
    code/rendering/sdl3_context.cpp
    code/rendering/imgui_context.cpp

    code/rendering/vulkan/vulkan_context.cpp
    code/rendering/vulkan/vulkan_upload_context.cpp
    code/rendering/vulkan/vulkan_texture.cpp

    # UI — top-level window widgets
    code/ui/window/widgets/main_menu_bar.cpp
    code/ui/window/widgets/app_coordinator.cpp

    # UI — file history
    code/ui/window/widgets/history/history_context_menu.cpp
    code/ui/window/widgets/history/opened_files_window.cpp
    code/ui/window/widgets/history/recent_history_menu.cpp

    # UI — video context menu
    code/ui/media/video/widgets/video_context_menu.cpp
    code/ui/media/video/widgets/video_osd_overlay.cpp
    code/ui/media/video/widgets/video_ui_window.cpp

    # UI — window persistence (layout + style)
    code/ui/window/persistence/app_state_coordinator.cpp
    code/ui/window/persistence/window_state_toml.cpp
    code/ui/window/persistence/style_editor.cpp

    # UI — media load/history orchestration
    code/ui/media/handlers/media_load_handler.cpp
    code/ui/media/handlers/media_history_manager.cpp

    # UI — preview strip
    code/ui/media/preview_ui/history_preview.cpp

    # UI — image subsystem
    code/ui/media/image/downloader/image_downloader.cpp
    code/ui/media/image/dialog/bulk_image_open_queue.cpp
    code/ui/media/image/dialog/open_image_dialogs.cpp
    code/ui/media/image/widgets/Image_viewer_panel.cpp
    code/ui/media/image/widgets/image_context_menu.cpp

    # UI — video subsystem
    code/ui/media/video/player/video_player.cpp
    code/ui/media/video/player/video_player_placebo.cpp
    code/ui/media/video/player/placebo_egl_context.cpp
    code/ui/media/video/downloader/video_downloader.cpp
    code/ui/media/video/video_preview/video_hover_preview.cpp
    code/ui/media/video/video_preview/video_seek_preview.cpp
    code/ui/media/video/player/VideoEntry.cpp

    # UI — app configuration
    code/ui/config/app_config.cpp
    code/ui/config/config_runtime.cpp

    code/ui/FileExplorer/FileBrowser.cpp
    code/ui/FileExplorer/file_browser_context_menu.cpp
    code/ui/FileExplorer/file_thumbnail_cache.cpp

    code/ui/media/metadata_editor/metadata_editor.cpp

    code/ui/Editor/TextEditor.cpp

    # UI — console (emoji atlas + interactive shell)
    code/ui/console/emoji_atlas.cpp
    code/ui/console/vulkan_emoji_atlas.cpp
    code/ui/console/imgui_console.cpp

    # ImGui core + Freetype font rasterizer
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp

    # ImPlot (v0.17 with ImGui 1.92.x fixes)
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp

    # FPS overlay
    code/ui/window/widgets/fps_plot.cpp
)

# ── Suppress warnings in vendored / external source files ───────────────────
set_source_files_properties(
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
    code/ui/Editor/TextEditor.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

# ── Precompiled header ────────────────────────────────────────────────────────
# Compiles once per build type; all project .cpp files reuse it.
# Only covers stable, frequently-included headers — not rapidly-changing ones.
target_precompile_headers(example_sdl3_vulkan PRIVATE
    code/pch/pch.hpp
)

# ── Include directories ───────────────────────────────────────────────────────
# Project-owned headers — warnings enabled.
target_include_directories(example_sdl3_vulkan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/code
    ${CMAKE_CURRENT_SOURCE_DIR}/code/main
    ${CMAKE_CURRENT_SOURCE_DIR}/code/pch
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core/thread
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core/log
    ${CMAKE_CURRENT_SOURCE_DIR}/code/rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/code/rendering/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/config
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/FileExplorer
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/window/persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/window/widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/window/widgets/history
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/handlers
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/preview_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/image
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/image/dialog
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/image/downloader
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/image/widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/video/player
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/video/downloader
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/video/video_preview
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/video/widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/media/metadata_editor
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui/console
)

# External / vendored / system headers — marked SYSTEM so the compiler
# treats them as system headers and suppresses all warnings from them.
target_include_directories(example_sdl3_vulkan SYSTEM PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${IMPLOT_DIR}
    ${SDL3_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
    ${Stb_INCLUDE_DIR}
    ${WEBP_INCLUDE_DIR}
    ${MPV_INCLUDE_DIRS}  # external/mpv/include (bundled headers)
    ${LIBPLACEBO_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
)

# ── Compile options ───────────────────────────────────────────────────────────
target_compile_options(example_sdl3_vulkan PRIVATE
    # Warnings — always on
    -Wall -Wformat

    # Debug: include DWARF debug info
    $<$<CONFIG:Debug>:-g>

    # Release (Clang only): aggressive optimisation + LTO + dead-code elimination
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-march=native>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-mtune=native>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-fomit-frame-pointer>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-ffunction-sections>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-fdata-sections>

    # Pass through any extra flags reported by pkg-config
    ${SDL3_CFLAGS_OTHER}
    ${VULKAN_CFLAGS_OTHER}
    ${FREETYPE_CFLAGS_OTHER}
    ${PLUTOSVG_CFLAGS_OTHER}
)

# ── Linker options ────────────────────────────────────────────────────────────
target_link_options(example_sdl3_vulkan PRIVATE
    # Release (Clang only): ThinLTO + strip unused sections
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-Wl,-O3>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-Wl,--as-needed>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-Wl,--gc-sections>
)

# ── Preprocessor definitions ──────────────────────────────────────────────────
target_compile_definitions(example_sdl3_vulkan PRIVATE
    # Enable Vulkan validation layer callbacks in Debug builds only
    $<$<CONFIG:Debug>:APP_USE_VULKAN_DEBUG_REPORT>
    # Path to bundled ImGui fonts, baked in at compile time
    IMGUI_FONTS_DIR="${IMGUI_DIR}/misc/fonts"
    # Enable OT-SVG emoji rendering via PlutoSVG in both EmojiAtlas and ImGui FreeType
    IMGUI_ENABLE_FREETYPE_PLUTOSVG
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(example_sdl3_vulkan PRIVATE
    ${SDL3_LIBRARIES}
    ${VULKAN_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${PLUTOSVG_LIBRARIES}
    reflectcpp::reflectcpp
    WebP::webp
    CURL::libcurl
    TagLib::tag
    ${MPV_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GL_LIBRARIES}
    ${LIBPLACEBO_LIBRARIES}
    ${CMAKE_DL_LIBS}   # dlopen/dlclose for Vulkan loader
)
