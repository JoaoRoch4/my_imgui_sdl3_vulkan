# Dear ImGui (docking) + SDL3/Vulkan backends + FreeType/PlutoSVG rasteriser,
# compiled once into a static `imgui` lib. SDL3 + Vulkan headers come transitively
# from the SDL3::SDL3 and Vulkan::Headers targets (added before this dir).
# FreeType/PlutoSVG include dirs are inherited from the root scope (their
# pkg_check_modules run before this subdirectory is added).
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
target_compile_definitions(imgui PRIVATE IMGUI_ENABLE_FREETYPE_PLUTOSVG)
target_compile_options(imgui PRIVATE
    ${FREETYPE_CFLAGS_OTHER}
    -w
)
app_profilable_treatment(imgui)
