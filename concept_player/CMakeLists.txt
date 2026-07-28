# Video-player design concept (see concept_player/main.cpp and README.md).
#
# A standalone program that prototypes the new player layout — menu bar, auto-hiding
# scene view (left), auto-hiding folder view (right), inline hover preview, and an
# animated transport bar — with every panel able to pop out of the player.
#
# Built AS PART OF the project; run it with:
#   ./build/debug/concept_player   (or build it alone: cmake --build build/all --target concept_player)
#
# Like launcher/, it builds its OWN minimal Dear ImGui and links only from-source
# SDL3::SDL3, so it does NOT drag in FFmpeg / mpv / libplacebo / Vulkan-Loader and
# compiles in seconds. It differs from launcher/ in ONE deliberate way: the renderer
# backend is imgui_impl_sdlgpu3, not imgui_impl_sdlrenderer3, because SDL_Renderer3
# lists multi-viewport as unsupported ("[ ] Renderer: Multi-viewport support") while
# SDL_GPU3 supports it — that is what makes a popped-out panel a real OS window under
# X11. SDL_GPU is core SDL3, so this costs no extra dependency. Under Wayland viewports
# are switched off at runtime (see the rationale in main.cpp) and pop-out degrades to a
# floating window inside the player.
set(_imgui ${CMAKE_SOURCE_DIR}/external/imgui-${IMGUI_VERSION})

set(_concept_src
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/concept_player_app.cpp  # composes the whole layout
    ${CMAKE_CURRENT_SOURCE_DIR}/motion.cpp              # ImAnim seam (see note below)
    ${CMAKE_CURRENT_SOURCE_DIR}/panel_reveal.cpp        # auto-hide + edge-proximity reveal
    ${CMAKE_CURRENT_SOURCE_DIR}/concept_panel.cpp       # overlay <-> popped-out OS window
    ${CMAKE_CURRENT_SOURCE_DIR}/playback_clock.cpp      # stand-in transport state
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_pattern.cpp       # procedural stand-in video frames
    ${CMAKE_CURRENT_SOURCE_DIR}/video_stage.cpp         # centre canvas
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_strip.cpp         # left "+5 sec frame" column
    ${CMAKE_CURRENT_SOURCE_DIR}/folder_shelf.cpp        # right folder view
    ${CMAKE_CURRENT_SOURCE_DIR}/transport_bar.cpp)      # bottom seek strip + controls

set(_imgui_src
    ${_imgui}/imgui.cpp
    ${_imgui}/imgui_draw.cpp
    ${_imgui}/imgui_tables.cpp
    ${_imgui}/imgui_widgets.cpp
    ${_imgui}/backends/imgui_impl_sdl3.cpp
    ${_imgui}/backends/imgui_impl_sdlgpu3.cpp)

add_executable(concept_player ${_concept_src} ${_imgui_src})

target_include_directories(concept_player SYSTEM PRIVATE ${_imgui} ${_imgui}/backends)
target_include_directories(concept_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(concept_player PRIVATE SDL3::SDL3)
target_compile_features(concept_player PRIVATE cxx_std_20)

# ── ImAnim: real library when present, same-shaped fallback when not ───────────
# The project's `imanim` skill documents external/ImAnim (im_anim.h + im_anim.cpp) as
# the way to animate ImGui widgets, but that checkout is NOT vendored here — only the
# skill doc is, on every branch. motion.cpp therefore ships a fallback with ImAnim's
# exact keying model ((owner,channel) -> eased value, crossfade on target change).
# Drop external/ImAnim in place and re-configure: this branch compiles it in and every
# Motion:: call forwards to iam_tween_*, with no change at any call site.
set(_imanim_dir ${CMAKE_SOURCE_DIR}/external/ImAnim)
if(EXISTS ${_imanim_dir}/im_anim.cpp)
    message(STATUS "concept_player: ImAnim found — using external/ImAnim")
    target_sources(concept_player PRIVATE ${_imanim_dir}/im_anim.cpp)
    target_include_directories(concept_player SYSTEM PRIVATE ${_imanim_dir})
    target_compile_definitions(concept_player PRIVATE CONCEPT_HAS_IMANIM=1)
    set_source_files_properties(${_imanim_dir}/im_anim.cpp PROPERTIES
        COMPILE_OPTIONS "-w"
        SKIP_PRECOMPILE_HEADERS ON
        SKIP_UNITY_BUILD_INCLUSION ON)
else()
    message(STATUS "concept_player: external/ImAnim absent — using the built-in "
                   "ImAnim-shaped motion fallback (see concept_player/motion.hpp)")
endif()

# Precompiled header (pch.hpp: STL + imgui.h) shared across the concept's own TUs.
target_precompile_headers(concept_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pch.hpp)

# Unity/jumbo build: files stay split one-type-per-file for humans; CMake batches them.
set_target_properties(concept_player PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)

# The vendored ImGui TUs are large, warn-noisy, and have their own static symbols — keep
# them OUT of both the PCH and the unity batches, and silence their warnings only.
set_source_files_properties(${_imgui_src} PROPERTIES
    COMPILE_OPTIONS "-w"
    SKIP_PRECOMPILE_HEADERS ON
    SKIP_UNITY_BUILD_INCLUSION ON)

# Drop the binary next to the per-config app binaries (matches the app's
# RUNTIME_OUTPUT_DIRECTORY_* in the root CMakeLists — RelWithDebInfo is build/RELWITHDEBINFO).
set_target_properties(concept_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          ${CMAKE_SOURCE_DIR}/build/debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        ${CMAKE_SOURCE_DIR}/build/release
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/build/RELWITHDEBINFO)
