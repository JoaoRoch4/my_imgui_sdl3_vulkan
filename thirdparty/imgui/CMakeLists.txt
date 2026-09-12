# Dear ImGui (docking) + SDL3/Vulkan backends + FreeType/PlutoSVG rasteriser,
# compiled once into a static `imgui` lib. SDL3 + Vulkan headers come transitively
# from the SDL3::SDL3 and Vulkan::Headers targets (added before this dir).
# FreeType/PlutoSVG include dirs are inherited from the root scope (their
# pkg_check_modules run before this subdirectory is added).
# IMGUI_VERSION is the single source of truth, set at the root scope; recompute
# IMGUI_DIR from it (external/imgui-<version>) rather than hardcoding the version.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui-${IMGUI_VERSION})

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    # std::string wrappers for InputText() — declared in misc/cpp/imgui_stdlib.h.
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

# Public headers consumers (the executable + ImPlot) include. SYSTEM keeps their
# warnings out of the consuming targets.
target_include_directories(imgui SYSTEM PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${IMGUI_DIR}/misc/cpp
)
# Private: FreeType rasteriser headers.
target_include_directories(imgui SYSTEM PRIVATE
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
)
# SDL3 / Vulkan headers for the imgui_impl_sdl3 / imgui_impl_vulkan backends.
target_link_libraries(imgui PRIVATE SDL3::SDL3 Vulkan::Headers plutosvg::plutosvg Freetype::Freetype)
target_compile_definitions(imgui
    PRIVATE IMGUI_ENABLE_FREETYPE_PLUTOSVG
    # Expose ImVec2/ImVec4 operator+ - * etc. to EVERY consumer (app code writes
    # `window->Pos + delta`). imgui's own .cpp files #ifndef-guard this macro, so
    # PUBLIC here causes no redefinition; it just reaches the app + test TUs.
    PUBLIC  IMGUI_DEFINE_MATH_OPERATORS
    # Pick FreeType as the atlas font loader (imgui_draw.cpp:4306 falls back to
    # stb_truetype when this is missing). stb_truetype reads only glyf and CFF1,
    # so the stb path aborts on this system's fonts — NotoSansCJK-VF.ttc is an
    # OTTO/CFF2 collection and the emoji face is COLRv1 — with
    # "stbtt_InitFont(): failed to parse FontData". It is also what makes the
    # ImGuiFreeTypeLoaderFlags_* in imgui_context.cpp (LoadColor/Bitmap) mean
    # anything, and what IMGUI_ENABLE_FREETYPE_PLUTOSVG above requires
    # (imconfig.h:92). PUBLIC, not PRIVATE: imgui_internal.h:128 switches
    # declarations on this macro, so every TU must see the same value.
            IMGUI_ENABLE_FREETYPE)
target_compile_options(imgui PRIVATE
    ${FREETYPE_CFLAGS_OTHER}
    -w
)
app_profilable_treatment(imgui)
