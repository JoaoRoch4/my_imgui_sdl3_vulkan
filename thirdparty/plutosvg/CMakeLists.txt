# plutosvg — SVG / OT-SVG / COLR rasteriser (color-emoji glyphs), built from
# external/plutosvg as a STATIC archive, FreeType-ENABLED. FreeType integration is
# what exposes plutosvg_ft_svg_hooks(); without it FreeType cannot rasterise the
# SVG/COLR glyphs in NotoColorEmoji (the app compiles imgui with
# IMGUI_ENABLE_FREETYPE_PLUTOSVG and hard-requires those hooks).
#
# Added AFTER thirdparty/plutovg: plutosvg's own CMake does
#   if(NOT TARGET plutovg::plutovg) find_package(plutovg) ... add_subdirectory(plutovg)
# so with our plutovg::plutovg already defined it links THAT (one plutovg in the
# process) and skips its bundled submodule. FreeType comes from the system
# find_package(Freetype) done at the root. Output target: plutosvg::plutosvg.
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
set(PLUTOSVG_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
# FreeType integration is mandatory for this app (see above) — force it on so it
# can't be silently disabled out from under IMGUI_ENABLE_FREETYPE_PLUTOSVG.
set(PLUTOSVG_ENABLE_FREETYPE ON  CACHE BOOL "" FORCE)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/plutosvg
    ${CMAKE_BINARY_DIR}/thirdparty/plutosvg/build
    EXCLUDE_FROM_ALL)

app_profilable_treatment(plutosvg)
