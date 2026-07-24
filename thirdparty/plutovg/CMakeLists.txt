# plutovg — 2D vector-graphics library, built from external/plutovg as a STATIC
# archive. Upstream's CMake defines the target `plutovg` and its alias
# `plutovg::plutovg`, which both plutosvg (thirdparty/plutosvg) and the imgui
# FreeType/PlutoSVG rasteriser (thirdparty/imgui) link against. Must be added
# BEFORE thirdparty/plutosvg so plutosvg reuses this target instead of falling back
# to its bundled submodule copy.
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)  # static .a, baked into the app
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)  # skip the demo exes (SDL/GL deps)

# The app is a PIE (-pie); a static archive linked into it must be position-
# independent or the link dies on R_X86_64_32 relocations (mirrors SDL3/FFmpeg).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/plutovg
    ${CMAKE_BINARY_DIR}/thirdparty/plutovg/build
    EXCLUDE_FROM_ALL)

# Default symbol visibility + DWARF + frame pointers so a sampling profiler can
# resolve plutovg frames (matches the stb/imgui/SDL treatment).
app_profilable_treatment(plutovg)
