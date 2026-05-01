cmake_minimum_required(VERSION 3.20)

set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "" FORCE)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "/home/joao/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

list(APPEND CMAKE_PREFIX_PATH "/home/joao/vcpkg/installed/x64-linux")
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")

project(example_sdl3_vulkan CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR /home/joao/vscode/my_imgui/External/Imgui-Viewports)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)
pkg_check_modules(VULKAN REQUIRED vulkan)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(PLUTOSVG REQUIRED plutosvg)
find_package(reflectcpp CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL REQUIRED)

add_executable(example_sdl3_vulkan
    code/main.cpp
    code/app.cpp
    code/ui/main_menu_bar.cpp
    code/ui/window_state_toml.cpp
    code/ui/style_editor.cpp
    code/ui/Image_viewer_panel.cpp
    code/rendering/sdl3_context.cpp
    code/rendering/vulkan_context.cpp
    code/rendering/imgui_context.cpp
    code/rendering/vulkan_texture.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
)

target_include_directories(example_sdl3_vulkan PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/misc/freetype
    ${CMAKE_CURRENT_SOURCE_DIR}/code
    ${CMAKE_CURRENT_SOURCE_DIR}/code/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/code/rendering
    ${SDL3_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${PLUTOSVG_INCLUDE_DIRS}
    ${Stb_INCLUDE_DIR}
    ${WEBP_INCLUDE_DIR}
)

target_compile_options(example_sdl3_vulkan PRIVATE
    -g -Wall -Wformat
    ${SDL3_CFLAGS_OTHER}
    ${VULKAN_CFLAGS_OTHER}
    ${FREETYPE_CFLAGS_OTHER}
    ${PLUTOSVG_CFLAGS_OTHER}
)

target_compile_definitions(example_sdl3_vulkan PRIVATE
    $<$<CONFIG:Debug>:APP_USE_VULKAN_DEBUG_REPORT>
    IMGUI_FONTS_DIR="${IMGUI_DIR}/misc/fonts"
)

target_link_libraries(example_sdl3_vulkan PRIVATE
    ${SDL3_LIBRARIES}
    ${VULKAN_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${PLUTOSVG_LIBRARIES}
    reflectcpp::reflectcpp
    WebP::webp
    CURL::libcurl
    ${CMAKE_DL_LIBS}
)
