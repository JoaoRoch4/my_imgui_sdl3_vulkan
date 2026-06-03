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

# Append _d to library names for Debug builds so debug and release artifacts can
# coexist (CMAKE_<CONFIG>_POSTFIX applies to libraries only, e.g. libimgui_d.a).
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

# ── Precompiled external GUI libraries (ImGui + ImPlot) ───────────────────────
# ImGui and ImPlot change very rarely, so they are compiled once into standalone
# static libraries instead of being rebuilt as part of the main executable.
# Because imconfig.h no longer pulls in pch.hpp, these libraries don't receive
# the project PCH and don't depend on the project's headers — so editing project
# code (or pch.hpp itself) never rebuilds them. Warnings are silenced per target
# with -w. Release optimisation (-O3 -DNDEBUG) comes from the default
# CMAKE_CXX_FLAGS_RELEASE; the executable-only LTO / -march=native flags are
# intentionally NOT applied here, keeping these objects quick to build and
# cleanly linkable into the ThinLTO executable.
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

# Public headers that consumers (the executable + ImPlot) include. Marked SYSTEM
# so their warnings stay suppressed in the consuming targets.
target_include_directories(imgui SYSTEM PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
)

# Private: only needed to compile the SDL3 / Vulkan backends and the
# FreeType + PlutoSVG font rasteriser.
target_include_directories(imgui SYSTEM PRIVATE
    ${SDL3_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
)

target_compile_definitions(imgui PRIVATE
    # OT-SVG emoji rendering via PlutoSVG (also set in imconfig.h; kept explicit
    # to mirror the original build and document the link-time dependency).
    IMGUI_ENABLE_FREETYPE_PLUTOSVG
)

# Suppress warnings from vendored sources; pass through pkg-config extra flags.
target_compile_options(imgui PRIVATE
    ${SDL3_CFLAGS_OTHER}
    ${VULKAN_CFLAGS_OTHER}
    ${FREETYPE_CFLAGS_OTHER}
    ${PLUTOSVG_CFLAGS_OTHER}
    -w
)

add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

# implot.h includes <imgui.h>, so anything linking ImPlot also needs ImGui:
# propagate the dependency PUBLIC-ly.
target_include_directories(implot SYSTEM PUBLIC
    ${IMPLOT_DIR}
)
target_link_libraries(implot PUBLIC imgui)
target_compile_options(implot PRIVATE -w)

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

    code/ui/FileExplorer/file_browser_ui.cpp
    code/ui/FileExplorer/file_browser_thread.cpp
    code/ui/FileExplorer/file_browser_context_menu.cpp
    code/ui/FileExplorer/file_thumbnail_cache.cpp

    code/ui/media/metadata_editor/metadata_editor.cpp

    code/ui/Editor/TextEditor.cpp

    # UI — console (emoji atlas + interactive shell)
    code/ui/console/emoji_atlas.cpp
    code/ui/console/vulkan_emoji_atlas.cpp
    code/ui/console/imgui_console.cpp

    # ImGui + ImPlot are compiled separately as the `imgui` / `implot` static
    # libraries (see the precompiled-libraries section above) and linked in below.

    # FPS overlay
    code/ui/window/widgets/fps_plot.cpp
    code/ui/config/config_runtime_ui_context.hpp
    code/ui/config/config_runtime_ui_context.cpp
)

# ── Multi-config output layout + "build everything" target ────────────────────
# The `all` preset uses Ninja Multi-Config: a single configure dir (build/all)
# builds every configuration. Send each config's *executable* to its own result
# folder under build/<name>/ — outside build/all, matching the historical layout
# and replacing the old single-config build dirs. Because those folders sit two
# levels below the repo root, the app's shared-cache lookup
# (SDL_GetBasePath()/../../cache) resolves to <repo>/cache for all three, so they
# share one window_state.toml / thumbnail / video cache.
#
# `all_configs` builds all three configs in one invocation. This is what you run
# to "build everything": VS Code CMake Tools (and `cmake --build`) only build the
# active config, so select this target — or run `cmake --workflow --preset all`.
# Guarded by CMAKE_CONFIGURATION_TYPES (set only for multi-config generators).
if(CMAKE_CONFIGURATION_TYPES)
    set_target_properties(example_sdl3_vulkan PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          ${CMAKE_SOURCE_DIR}/build/debug
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        ${CMAKE_SOURCE_DIR}/build/release
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/build/release-log
        # Per-config executable names so each build is identifiable on sight.
        OUTPUT_NAME_DEBUG          example_sdl3_vulkan_debug
        OUTPUT_NAME_RELEASE        example_sdl3_vulkan_release
        OUTPUT_NAME_RELWITHDEBINFO example_sdl3_vulkan_release-log
    )

    add_custom_target(all_configs
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config Debug
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config Release
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config RelWithDebInfo
        COMMENT "Building all configs -> build/debug, build/release, build/release-log"
        VERBATIM
    )
endif()

# ── Suppress warnings in vendored / external source files ───────────────────
# (ImGui + ImPlot now build as separate static libraries with -w applied at the
#  target level — see the precompiled-libraries section above.)
set_source_files_properties(
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
    # ImGui / ImPlot headers arrive transitively from the linked `imgui` /
    # `implot` targets (their PUBLIC SYSTEM include dirs).
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
    # Enable debug-specific code paths
  $<$<CONFIG:Debug>:_DEBUG>
$<$<CONFIG:RelWithDebInfo>:__APP_FORCE_LOGS>    


    # Enable Vulkan validation layer callbacks in Debug builds only
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:APP_USE_VULKAN_DEBUG_REPORT>
    
    # Path to bundled ImGui fonts, baked in at compile time
    IMGUI_FONTS_DIR="${IMGUI_DIR}/misc/fonts"
    
    # Enable OT-SVG emoji rendering via PlutoSVG
    IMGUI_ENABLE_FREETYPE_PLUTOSVG

    APP_USE_UNLIMITED_FRAME_RATE
)

# ── Linker search paths ───────────────────────────────────────────────────────
# pkg-config reports these libraries by bare name (e.g. -lSDL3); their containing
# directories must be searched explicitly. SDL3 lives in /usr/local/lib64, which
# GNU ld searches by default but LLD does not — without these the LLD link fails
# to find SDL3 / plutosvg / plutovg.
target_link_directories(example_sdl3_vulkan PRIVATE
    ${SDL3_LIBRARY_DIRS}
    ${VULKAN_LIBRARY_DIRS}
    ${FREETYPE_LIBRARY_DIRS}
    ${PLUTOSVG_LIBRARY_DIRS}
    ${LIBPLACEBO_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GL_LIBRARY_DIRS}
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(example_sdl3_vulkan PRIVATE
    imgui              # precompiled Dear ImGui (core + SDL3/Vulkan backends + freetype)
    implot             # precompiled ImPlot (transitively brings in imgui)
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
