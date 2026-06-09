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
# The loader does `find_package(VulkanHeaders CONFIG QUIET)` then compares
# ${VulkanHeaders_VERSION}. With headers from add_subdirectory (not an installed
# CONFIG package) that variable is empty, making the loader's version-check if()
# error out. Provide it explicitly (matching the Vulkan-Headers tag) so the check
# is a harmless no-op.
set(VulkanHeaders_VERSION 1.4.350)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/Vulkan-Loader
    ${CMAKE_BINARY_DIR}/thirdparty/vulkan-loader/build
    EXCLUDE_FROM_ALL)
app_profilable_treatment(vulkan)

# The Vulkan loader MUST keep the universal soname libvulkan.so.1: SDL3 dlopen()s it
# by that exact name, and libplacebo/libmpv were built NEEDing it. The project-wide
# CMAKE_DEBUG_POSTFIX "_d" would rename it to libvulkan_d.so.1, leaving the app on the
# from-source loader while everything else falls back to the SYSTEM libvulkan.so.1 —
# two loaders in one process, which breaks VkSurface creation. Clear the postfix so
# all components share this one from-source loader.
set_target_properties(vulkan PROPERTIES DEBUG_POSTFIX "")
