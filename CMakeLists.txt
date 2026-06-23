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
# Per-library build glue lives under thirdparty/<lib>/; shared helpers below.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(ProfilableTreatment)
include(PerConfigExternal)   # per-config ExternalProject helpers (FFmpeg/mpv/libplacebo)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sanitizers (opt-in) ───────────────────────────────────────────────────────
# -DENABLE_SANITIZERS=ON instruments the app target with AddressSanitizer + UBSan
# in the Debug config ONLY — Release / RelWithDebInfo and the perf workflow are
# never touched. It's a mode toggle on the shared build/all tree: reconfigure with
# =ON, rebuild Debug, debug via the "Debug + ASan/UBSan" launch config; turn it
# back off with -DENABLE_SANITIZERS=OFF. Only the app's own objects are
# instrumented — the prebuilt thirdparty static archives (FFmpeg/mpv/libplacebo/
# SDL3/…) stay as-is, so no media rebuild, and ASan still catches bugs in app code.
# (Mixing instrumented app + un-instrumented STL needs detect_container_overflow=0,
#  which the launch config's ASAN_OPTIONS sets.)
option(ENABLE_SANITIZERS "Build the app target with ASan+UBSan (Debug config only)" ON)

# Emit compile_commands.json (clangd / tooling) for every configure, not just the
# `all` preset. Harmless redundancy when the preset already sets it; the safety net
# is for bare `cmake -S . -B <dir>` invocations that don't go through the preset.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Append _d to library names for Debug builds so debug and release artifacts can
# coexist (CMAKE_<CONFIG>_POSTFIX applies to libraries only, e.g. libimgui_d.a).
set(CMAKE_DEBUG_POSTFIX "_d")

# ── External library paths ────────────────────────────────────────────────────
# ImGui fork lives outside this repo. Kept here only for the IMGUI_FONTS_DIR
# compile definition below; the imgui build itself now lives in thirdparty/imgui/
# (IMPLOT_DIR / STB_DIR likewise moved into their thirdparty/<lib> folders).
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui-1.92.8-docking)

# ── Dependency discovery ──────────────────────────────────────────────────────
# Only the deps with NO vendored source are resolved here: FreeType2 / PlutoSVG /
# EGL / GL via pkg-config, and reflectcpp / WebP / CURL via CMake find_package.
# Everything else is compiled from source under thirdparty/<lib>/ (added below).
find_package(PkgConfig REQUIRED)
# Still resolved from the system (no vendored source): FreeType, PlutoSVG, EGL, GL.
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(PLUTOSVG REQUIRED plutosvg)
# ── LOCAL OVERRIDE: FreeType-enabled plutosvg ─────────────────────────────────
# The system plutosvg (/usr/local) was built with PLUTOSVG_ENABLE_FREETYPE=OFF, so
# plutosvg_ft_svg_hooks() returns NULL and FreeType cannot rasterise the SVG/COLR
# glyphs in NotoColorEmoji (FT_Err_Missing_SVG_Hooks, 158) — color emoji then render
# as missing-glyph boxes. Link a locally-rebuilt FreeType-enabled static archive
# instead (built from /home/joao/dev/plutosvg with -DPLUTOSVG_ENABLE_FREETYPE=ON).
# Override the path with -DPLUTOSVG_FT_LIBRARY=<path>; falls back to system if absent.
# NOTE: absolute path outside the repo — not portable; vendor into thirdparty/ later.
set(PLUTOSVG_FT_LIBRARY "/home/joao/dev/plutosvg/build-ft/libplutosvg.a"
    CACHE FILEPATH "FreeType-enabled libplutosvg.a (PLUTOSVG_ENABLE_FREETYPE=ON)")
if(EXISTS "${PLUTOSVG_FT_LIBRARY}")
    # Swap the bare 'plutosvg' entry for the absolute FT-enabled archive; 'plutovg'
    # stays after it (plutosvg.a references plutovg/FT symbols → must precede them).
    list(TRANSFORM PLUTOSVG_LIBRARIES REPLACE "^plutosvg$" "${PLUTOSVG_FT_LIBRARY}")
    message(STATUS "plutosvg: FreeType-enabled override -> ${PLUTOSVG_FT_LIBRARY}")
else()
    message(WARNING "PLUTOSVG_FT_LIBRARY missing (${PLUTOSVG_FT_LIBRARY}); color emoji disabled")
endif()
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GL REQUIRED gl)
# fontconfig: portable, per-distro font discovery for the ImGui fallback chain
# (symbols / math / emoji / CJK / Hebrew / Arabic / hieroglyphs). Transitive via
# FreeType, but the app calls FcFontMatch directly, so resolve it explicitly.
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
# vcpkg / system CMake packages with no vendored source.
find_package(reflectcpp CONFIG REQUIRED)
find_package(CURL REQUIRED)
# NOTE: SDL3, Vulkan (loader), mpv, libplacebo, taglib — and stb/imgui/implot —
# are now compiled from source; see the thirdparty/<lib> subdirectories below.
# SDL3 is the static archive built by thirdparty/sdl3 (SDL3::SDL3), not vcpkg.

# ── Compiled external libraries (each in its own thirdparty/<lib> folder) ─────
# The root file is now a thin orchestrator: each dependency owns its build under
# thirdparty/<lib>/. Order matters — SDL3 / Vulkan-Loader / taglib define targets
# that imgui links; FFmpeg is built before mpv (mpv consumes it). Foreign
# (autotools/meson) builds are wrapped as ExternalProjects inside their folder.
# libplacebo is processed BEFORE ffmpeg: FFmpeg's vf_libplacebo filter compiles
# against libplacebo's headers (pl_log_create_<PL_API_VER>), so it must see OUR
# install tree (PLACEBO_PKGCONFIG_DIR_<cfg> + libplacebo_ep_<cfg> target) — else
# configure resolves the system libplacebo and the API version diverges at link.
add_subdirectory(thirdparty/sdl3)
add_subdirectory(thirdparty/vulkan-loader)
add_subdirectory(thirdparty//WebP)
add_subdirectory(thirdparty/taglib)
add_subdirectory(thirdparty/imgui)
add_subdirectory(thirdparty/implot)
add_subdirectory(thirdparty/stb)
add_subdirectory(thirdparty/libplacebo)
add_subdirectory(thirdparty/ffmpeg)
add_subdirectory(thirdparty/ffmpegthumbnailer)   # static, against our FFmpeg 8.0
add_subdirectory(thirdparty/mpv)

# ── Sources ───────────────────────────────────────────────────────────────────
add_executable(example_sdl3_vulkan

    # Entry point & main loop
    code/main/main.cpp
    code/main/app.cpp

    # Core utilities
    code/core/thread/thread_overwatch.cpp
    code/core/thread/thread_registry.cpp
    code/core/thread/managed_thread.cpp

    # Parallel image engine (JobQueue + ImageJobSystem over stb/libwebp)
    code/core/image/image_ops.cpp
    code/core/image/job_queue.cpp
    code/core/image/image_job_system.cpp
    # Rendering layer (SDL3 window + ImGui + Vulkan backend)
    code/rendering/sdl3_context.cpp
    code/rendering/imgui_context.cpp

    code/rendering/vulkan/vulkan_context.cpp
    code/rendering/vulkan/vulkan_upload_context.cpp
    code/rendering/vulkan/vulkan_texture.cpp
    code/rendering/vulkan/vulkan_bc1_encoder.cpp

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
    code/ui/FileExplorer/thumbnail_blob_cache.cpp
    code/ui/FileExplorer/file_browser_file_operations_core.cpp
    code/ui/FileExplorer/file_browser_file_operations.cpp
    code/ui/FileExplorer/file_browser_thumbnail_context.cpp
    code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp
    code/ui/FileExplorer/thumbnail_generator.cpp
    code/ui/FileExplorer/thumbnail_reproducer.cpp

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

    # Thread reflection / introspection panel
    code/ui/window/widgets/thread_reflection_panel.cpp
    code/ui/config/config_runtime_ui_context.hpp
    code/ui/config/config_runtime_ui_context.cpp
    code/Args/Args.hpp
    code/Args/Args.cpp
    code/Args/startup_options.hpp
    code/Args/startup_options.cpp
    code/main/main.hpp
    code/core/Memory_management.hpp
)

# ── Compute shader compilation (BC1 GPU encoder) ──────────────────────────────
# Drives bc1_encode.comp -> SPIR-V -> C++ header (uint32_t array) via
# glslangValidator's --vn switch. The header is dropped into a generated
# include directory the executable picks up; no runtime shader-file lookup,
# no external xxd/python script. Recompiles whenever the .comp source changes.
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED
    DOC "GLSL -> SPIR-V compiler used to embed BC1 encoder shader")

set(_bc1_comp_src "${CMAKE_CURRENT_SOURCE_DIR}/code/rendering/vulkan/shaders/bc1_encode.comp")
set(_bc1_gen_dir  "${CMAKE_BINARY_DIR}/generated/shaders")
set(_bc1_gen_hpp  "${_bc1_gen_dir}/bc1_encode_spv.hpp")

file(MAKE_DIRECTORY "${_bc1_gen_dir}")
add_custom_command(
    OUTPUT  "${_bc1_gen_hpp}"
    COMMAND ${GLSLANG_VALIDATOR} -V "${_bc1_comp_src}"
            --vn bc1_encode_spv
            -o "${_bc1_gen_hpp}"
    DEPENDS "${_bc1_comp_src}"
    COMMENT "Compiling BC1 compute shader -> ${_bc1_gen_hpp}"
    VERBATIM
)
add_custom_target(bc1_shader DEPENDS "${_bc1_gen_hpp}")

# Link the executable to the shader header (build dependency + include path).
# The dependency is added below, after add_executable runs.

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
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/build/RELWITHDEBINFO
        # Per-config executable names so each build is identifiable on sight.
        OUTPUT_NAME_DEBUG          example_sdl3_vulkan_debug
        OUTPUT_NAME_RELEASE        example_sdl3_vulkan_release
        OUTPUT_NAME_RELWITHDEBINFO example_sdl3_vulkan_RELWITHDEBINFO
    )

    add_custom_target(all_configs
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config Debug
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config Release
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config RelWithDebInfo
        COMMENT "Building all configs -> build/debug, build/release, build/RELWITHDEBINFO"
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
add_dependencies(example_sdl3_vulkan bc1_shader)
target_include_directories(example_sdl3_vulkan PRIVATE
    ${_bc1_gen_dir}                                       # bc1_encode_spv.hpp (compiled shader)
    ${CMAKE_CURRENT_SOURCE_DIR}/code
    ${CMAKE_CURRENT_SOURCE_DIR}/code/main
    ${CMAKE_CURRENT_SOURCE_DIR}/code/Args
    ${CMAKE_CURRENT_SOURCE_DIR}/code/pch
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core/thread
    ${CMAKE_CURRENT_SOURCE_DIR}/code/core/image
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
    # ImGui / ImPlot / stb headers arrive transitively from the linked imgui /
    # implot / stb targets; SDL3 / Vulkan / mpv / libplacebo headers arrive from
    # their from-source targets (SDL3::SDL3, Vulkan::Headers, mpv::mpv,
    # placebo::placebo) — see target_link_libraries below.
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
)

# ── Compile options ───────────────────────────────────────────────────────────
target_compile_options(example_sdl3_vulkan PRIVATE
    # Warnings — always on
    -Wall -Wformat

    # Debug: include DWARF debug info
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-mno-omit-leaf-frame-pointer>
    $<$<CONFIG:RelWithDebInfo>:-g>
    $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>
    $<$<CONFIG:RelWithDebInfo>:-mno-omit-leaf-frame-pointer>

    # Sanitizers (opt-in via -DENABLE_SANITIZERS=ON; Debug only). The two
    # -fsanitize= flags are kept separate — clang merges them, and a comma inside
    # a generator expression would be parsed as an argument separator.
    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>>:-fsanitize=address>
    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>>:-fsanitize=undefined>
    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>>:-fno-sanitize-recover=undefined>

    # NOTE: RelWithDebInfo is deliberately NOT sanitized — it's the profiling
    # config, and ASan's shadow-memory load/store instrumentation grossly distorts
    # CPU profiles (esp. tight pixel loops in JPEG decode / stbir resize). Keep ASan
    # in Debug for correctness; profile RelWithDebInfo (or Release) clean.

    # Release (Clang only): aggressive optimisation + LTO + dead-code elimination
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-march=native>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-mtune=native>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-fomit-frame-pointer>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-ffunction-sections>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-fdata-sections>

    # Pass through any extra flags reported by pkg-config (FreeType / PlutoSVG)
    ${FREETYPE_CFLAGS_OTHER}
    ${PLUTOSVG_CFLAGS_OTHER}
)

# ── Linker options ────────────────────────────────────────────────────────────
target_link_options(example_sdl3_vulkan PRIVATE
    # Export all symbols into the dynamic symbol table (-Wl,--export-dynamic) so
    # profilers (perf, backtrace_symbols, sampling tools) can resolve stb /
    # ImGui / ImPlot and application frames at runtime. Applied to every config.
    -rdynamic

    # Sanitizers (opt-in; Debug only) — must also be passed at link time so the
    # ASan/UBSan runtimes are linked into the executable.
    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>>:-fsanitize=address>
    $<$<AND:$<BOOL:${ENABLE_SANITIZERS}>,$<CONFIG:Debug>>:-fsanitize=undefined>

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
    ${FREETYPE_LIBRARY_DIRS}
    ${PLUTOSVG_LIBRARY_DIRS}
    ${FONTCONFIG_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GL_LIBRARY_DIRS}
)

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(example_sdl3_vulkan PRIVATE
    imgui              # precompiled Dear ImGui (core + SDL3/Vulkan backends + freetype)
    implot             # precompiled ImPlot (transitively brings in imgui)
    stb                # precompiled stb (image / image_write / image_resize2)
    SDL3::SDL3         # from-source SDL3            (thirdparty/sdl3)
    vulkan             # from-source Vulkan loader   (thirdparty/vulkan-loader)
    Vulkan::Headers    # Vulkan API headers (interface)
    ${FREETYPE_LIBRARIES}
    ${PLUTOSVG_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}   # FcFontMatch / FcPatternCreate — see load_fonts() discovery
    reflectcpp::reflectcpp
    WebP::webp    
    CURL::libcurl
    tag                # from-source TagLib         (thirdparty/taglib)
    mpv::mpv           # from-source libmpv          (thirdparty/mpv)
    ${EGL_LIBRARIES}
    ${GL_LIBRARIES}
    placebo::placebo   # from-source libplacebo      (thirdparty/libplacebo)
    ${CMAKE_DL_LIBS}   # dlopen/dlclose for Vulkan loader
    stdc++exp          # std::stacktrace backend (libstdc++ >= 13); used by ManagedThread::escalate
)

# ── ffmpegthumbnailer (static, from-source) ───────────────────────────────────
# Header path for <libffmpegthumbnailer/...> + the per-config static archive built
# against our FFmpeg 8.0 (thirdparty/ffmpegthumbnailer). The archive is linked by
# full $<CONFIG> path and placed BEFORE the mpv media-link.rsp below, so its libav*
# references resolve against the FFmpeg --start-group inside that rsp (a static lib
# must precede the libraries it depends on). LINK_DEPENDS on the same $<CONFIG>
# archive makes the link wait for — and so build — only this config's EP (which in
# turn DEPENDS on the same-config FFmpeg). No .so, no rpath; one FFmpeg in-process.
set(_ffthumb_a "${CMAKE_BINARY_DIR}/thirdparty/ffmpegthumbnailer/$<CONFIG>/install/lib/libffmpegthumbnailer.a")
target_link_libraries(example_sdl3_vulkan PRIVATE ffthumb::ffthumb "${_ffthumb_a}")
set_property(TARGET example_sdl3_vulkan APPEND PROPERTY LINK_DEPENDS "${_ffthumb_a}")

# ── Static media stack: per-config transitive link resolution ─────────────────
# FFmpeg / libplacebo / mpv are built as STATIC archives (.a) per config (see
# thirdparty/{ffmpeg,libplacebo,mpv}). A static archive carries only its own
# objects, so the app must also link mpv's whole transitive closure: the sibling
# archives + libass/zlib + system libs. That list is resolved by a per-config
# `pkg-config --static --libs mpv` STEP of each mpv EP (thirdparty/mpv), which writes
# thirdparty/mpv/<C>/media-link.rsp. The app reads it via `-Wl,@<file>` — which lands
# in the LINK_LIBRARIES slot (AFTER the objects, as static archives require) — and
# LINK_DEPENDS on the same $<CONFIG> file makes the link wait for it, pulling only
# that config's mpv EP (and the ffmpeg/libplacebo EPs it DEPENDS on). The rsp is an EP
# step, not a root add_custom_command: a root custom command is cross-config in Ninja
# Multi-Config and would force every config to build. No rpath: nothing is a runtime .so.
set(_media_rsp "${CMAKE_BINARY_DIR}/thirdparty/mpv/$<CONFIG>/media-link.rsp")
target_link_libraries(example_sdl3_vulkan PRIVATE "-Wl,@${_media_rsp}")
set_property(TARGET example_sdl3_vulkan APPEND PROPERTY LINK_DEPENDS "${_media_rsp}")

# ── Tests (doctest) ─────────────────────────────────────────────────────────────
# Standalone test runner for the parallel image module (code/core/image). Built as
# its own `image_tests` target — NOT linked into the application. Run with:
#   cmake --build build/all --config Debug --target image_tests
#   ./build/all/Debug/image_tests          (or via `ctest` in the config dir)
# The JobQueue + image_ops are PCH-free and Vulkan-free, but image_ops now decodes
# via FFmpeg (decode_file/encode_png), so the test targets that compile it must link
# the same static FFmpeg the app uses — stb + libwebp alone no longer satisfy it.
enable_testing()

# Per-config FFmpeg surface for the test targets: our 8.0 headers + the FFmpeg-only
# static-link rsp (a byproduct of ffmpeg_ep_<cfg>'s gen_static_rsp step — the full
# avformat/avcodec/swscale closure + system libs). LINK_DEPENDS on the $<CONFIG> rsp
# makes the link wait for, and so build, only that config's already-built FFmpeg.
set(_ffmpeg_test_inc "${CMAKE_BINARY_DIR}/thirdparty/ffmpeg/$<CONFIG>/install/include")
set(_ffmpeg_test_rsp "${CMAKE_BINARY_DIR}/thirdparty/ffmpeg/$<CONFIG>/ffmpeg-link.rsp")
# image_ops.cpp also calls ffmpegthumbnailer (decode_video_thumbnail), so the test
# targets need its header + static archive. The archive must be linked BEFORE the
# FFmpeg rsp (it references libav* symbols resolved by that --start-group).
set(_ffthumb_test_inc "${CMAKE_SOURCE_DIR}/external/ffmpegthumbnailer")
set(_ffthumb_test_a   "${CMAKE_BINARY_DIR}/thirdparty/ffmpegthumbnailer/$<CONFIG>/install/lib/libffmpegthumbnailer.a")

add_executable(image_tests
    tests/test_main.cpp
    tests/image_ops_test.cpp
    tests/job_queue_test.cpp
    tests/file_operations_test.cpp
    code/core/image/image_ops.cpp
    code/core/image/job_queue.cpp
    code/ui/FileExplorer/file_browser_file_operations_core.cpp
)
target_include_directories(image_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/code/core/image
    ${CMAKE_SOURCE_DIR}/code/ui/FileExplorer
)
# doctest + libwebp headers marked SYSTEM so their warnings (e.g. doctest's
# <ciso646> include) stay out of our build; our own test warnings remain visible.
target_include_directories(image_tests SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/external/doctest
    WebP::webp    
    ${_ffmpeg_test_inc}    # our FFmpeg 8.0 headers (not the /usr/local shadow install)
    ${_ffthumb_test_inc}   # <libffmpegthumbnailer/...>
)
target_link_libraries(image_tests PRIVATE
    stb                        # resize implementation (image_resize2)
    WebP::webp                 # .webp decode
    "${_ffthumb_test_a}"       # ffmpegthumbnailer (before the FFmpeg group below)
    "-Wl,@${_ffmpeg_test_rsp}" # static FFmpeg closure for decode_file/encode_png
)
set_property(TARGET image_tests APPEND PROPERTY LINK_DEPENDS "${_ffmpeg_test_rsp}" "${_ffthumb_test_a}")
# doctest unconditionally includes <ciso646> (removed in C++20); silence just that
# preprocessor #warning so test build output stays pristine.
target_compile_options(image_tests PRIVATE "-Wno-#warnings")
add_test(NAME image_tests COMMAND image_tests)

# ── Functional engine tests (image_job_tests) ───────────────────────────────────
# Exercises ImageJobSystem end-to-end with the REAL ManagedThread pool, so it must
# compile the thread infrastructure — which #includes the project PCH. It therefore
# inherits the application's include dirs (genexp) and links the GUI libs purely for
# their header paths (no objects are pulled in, since this target references none of
# their symbols). A PCH keeps the otherwise-heavy compile fast.
add_executable(image_job_tests
    tests/test_main.cpp
    tests/image_job_system_test.cpp
    tests/file_browser_file_operations_test.cpp
    code/core/image/image_ops.cpp
    code/core/image/job_queue.cpp
    code/core/image/image_job_system.cpp
    code/ui/FileExplorer/file_browser_file_operations_core.cpp
    code/ui/FileExplorer/file_browser_file_operations.cpp
    code/core/thread/managed_thread.cpp
    code/core/thread/thread_overwatch.cpp
    code/core/thread/thread_registry.cpp
)
target_include_directories(image_job_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/code
    ${CMAKE_SOURCE_DIR}/code/core/image
    ${CMAKE_SOURCE_DIR}/code/ui/FileExplorer
)
target_include_directories(image_job_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:example_sdl3_vulkan,INCLUDE_DIRECTORIES>
    ${CMAKE_SOURCE_DIR}/external/doctest
    # pch.hpp pulls SDL3 / Vulkan / mpv / libplacebo headers. Use the vendored
    # *source* header dirs for those so this test compiles without dragging in the
    # heavy mpv / libplacebo ExternalProject builds. FFmpeg is the exception: it must
    # be the same-config *installed* 8.0 headers, because image_ops.cpp links the
    # matching static archives (rsp below) — a header/ABI mismatch would crash.
    ${CMAKE_SOURCE_DIR}/external/SDL/include
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/external/mpv/include
    ${CMAKE_SOURCE_DIR}/external/libplacebo/src/include
    ${CMAKE_SOURCE_DIR}/external/libwebp/src/
    ${_ffmpeg_test_inc}
    ${_ffthumb_test_inc}
)
target_precompile_headers(image_job_tests PRIVATE code/pch/pch.hpp)
target_compile_options(image_job_tests PRIVATE "-Wno-#warnings")
target_link_libraries(image_job_tests PRIVATE
    stb
    WebP::webp
    imgui                       # header paths only (imgui.h, pulled by pch)
    implot                      # header paths only (implot.h)
    reflectcpp::reflectcpp      # header paths only (rfl.hpp)
    CURL::libcurl               # header paths only (curl/curl.h)
    tag                         # header paths only (taglib/*) — from-source TagLib
    stdc++exp                   # std::stacktrace (ManagedThread::escalate)
    "${_ffthumb_test_a}"        # ffmpegthumbnailer (before the FFmpeg group below)
    "-Wl,@${_ffmpeg_test_rsp}"  # static FFmpeg closure for decode_file/encode_png
)
set_property(TARGET image_job_tests APPEND PROPERTY LINK_DEPENDS "${_ffmpeg_test_rsp}" "${_ffthumb_test_a}")
add_test(NAME image_job_tests COMMAND image_job_tests)

# ── ThumbnailBlobCache tests (thumbnail_blob_tests) ──────────────────────────────
# The blob cache logic is pure std, but thumbnail_blob_cache.cpp leads with the
# project PCH (app convention), so this target must compile that PCH too — same
# recipe as image_job_tests: app include dirs (genexp) + vendored header dirs for
# the SDL/Vulkan/mpv/libplacebo headers pch.hpp pulls, and the GUI libs linked for
# their header paths only (no objects referenced). Run with:
#   cmake --build build/all --config Debug --target thumbnail_blob_tests
#   ./build/all/Debug/thumbnail_blob_tests
add_executable(thumbnail_blob_tests
    tests/test_main.cpp
    tests/thumbnail_blob_cache_test.cpp
    code/ui/FileExplorer/thumbnail_blob_cache.cpp
)
target_include_directories(thumbnail_blob_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/code
    ${CMAKE_SOURCE_DIR}/code/ui/FileExplorer
)
target_include_directories(thumbnail_blob_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:example_sdl3_vulkan,INCLUDE_DIRECTORIES>
    ${CMAKE_SOURCE_DIR}/external/doctest
    ${CMAKE_SOURCE_DIR}/external/SDL/include
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/external/mpv/include
    ${CMAKE_SOURCE_DIR}/external/libplacebo/src/include
    ${_ffmpeg_test_inc}
    ${_ffthumb_test_inc}
)
target_precompile_headers(thumbnail_blob_tests PRIVATE code/pch/pch.hpp)
target_compile_options(thumbnail_blob_tests PRIVATE "-Wno-#warnings")
target_link_libraries(thumbnail_blob_tests PRIVATE
    imgui                       # header paths only (imgui.h, pulled by pch)
    implot                      # header paths only (implot.h)
    reflectcpp::reflectcpp      # header paths only (rfl.hpp)
    CURL::libcurl               # header paths only (curl/curl.h)
    tag                         # header paths only (taglib/*) — from-source TagLib
    stdc++exp                   # std::stacktrace (pulled transitively by pch)
)
add_test(NAME thumbnail_blob_tests COMMAND thumbnail_blob_tests)
