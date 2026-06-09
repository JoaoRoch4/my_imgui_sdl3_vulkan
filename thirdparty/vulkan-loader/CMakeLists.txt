# Build the Vulkan loader (libvulkan.so) from external/Vulkan-Loader (v1.4.350)
# against external/Vulkan-Headers (same tag). Vulkan-Headers provides the
# Vulkan::Headers INTERFACE target; the loader's find_package(VulkanHeaders ...
# QUIET) is satisfied by that already-defined target. Output targets:
#   vulkan          — the loader shared lib (libvulkan.so)
#   Vulkan::Headers — the Vulkan API headers (interface, incl. vulkan.hpp)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Headers
    ${CMAKE_BINARY_DIR}/thirdparty/vulkan-headers
    EXCLUDE_FROM_ALL)

set(BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(BUILD_WSI_XCB_SUPPORT     ON  CACHE BOOL "" FORCE)
set(BUILD_WSI_XLIB_SUPPORT    ON  CACHE BOOL "" FORCE)
set(BUILD_WSI_WAYLAND_SUPPORT ON  CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Loader
    ${CMAKE_BINARY_DIR}/thirdparty/vulkan-loader
    EXCLUDE_FROM_ALL)
app_profilable_treatment(vulkan)
