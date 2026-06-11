# Build libplacebo from external/libplacebo (meson) ONCE PER build configuration,
# using clang/LLD. Submodules (glad/Vulkan-Headers/jinja/markupsafe/fast_float) are
# populated at pinned commits. Each config installs into
# build/all/thirdparty/libplacebo/<CONFIG>/install. Output target: placebo::placebo
# (per-config IMPORTED_LOCATION_<CONFIG>). See cmake/PerConfigExternal.cmake.
include(ExternalProject)

set(PLACEBO_ROOT   ${CMAKE_BINARY_DIR}/thirdparty/libplacebo)
set(PLACEBO_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)
app_resolve_build_configs(_placebo_cfgs)

foreach(cfg IN LISTS _placebo_cfgs)
    set(_prefix  ${PLACEBO_ROOT}/${cfg})
    set(_install ${_prefix}/install)
    set(_build   ${_prefix}/build)
    app_meson_buildtype_args(_meson_flags ${cfg})

    ExternalProject_Add(libplacebo_ep_${cfg}
        SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/libplacebo
        PREFIX            ${_prefix}
        EXCLUDE_FROM_ALL  TRUE
        CONFIGURE_COMMAND meson setup ${_build}
                            ${CMAKE_SOURCE_DIR}/external/libplacebo
                            --native-file ${PLACEBO_NATIVE}
                            --prefix ${_install}
                            --libdir lib
                            --default-library static
                            ${_meson_flags}
                            -Ddemos=false -Dtests=false
        # Build AND install in the BUILD step — BUILD_BYPRODUCTS attach to the build
        # step, so install must run there too or ninja links a not-yet-installed .so
        # (we have no config-aware add_dependencies() to order a separate install).
        BUILD_COMMAND     ninja -C ${_build}
                  COMMAND ninja -C ${_build} install
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  ${_install}/lib/libplacebo.a
        USES_TERMINAL_BUILD TRUE)

    file(MAKE_DIRECTORY ${_install}/include)
    set(PLACEBO_PKGCONFIG_DIR_${cfg} ${_install}/lib/pkgconfig CACHE INTERNAL "libplacebo .pc dir (${cfg})")
endforeach()

# Include-only INTERFACE target — the static libplacebo.a is linked centrally via
# the app's pkg-config --static response file (mpv Requires.private libplacebo), so
# this carries only the (config-invariant) header path the app compiles against.
add_library(placebo::placebo INTERFACE IMPORTED GLOBAL)
set_target_properties(placebo::placebo PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${PLACEBO_ROOT}/$<CONFIG>/install/include)
