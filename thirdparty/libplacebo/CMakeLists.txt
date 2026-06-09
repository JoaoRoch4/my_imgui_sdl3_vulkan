# Build libplacebo from external/libplacebo (meson) once, with debug symbols,
# using clang/LLD. Submodules (glad/Vulkan-Headers/jinja/markupsafe/fast_float)
# are populated at pinned commits. Installs into
# build/all/thirdparty/libplacebo/install. Output target: placebo::placebo.
include(ExternalProject)

set(PLACEBO_PREFIX  ${CMAKE_BINARY_DIR}/thirdparty/libplacebo)
set(PLACEBO_INSTALL ${PLACEBO_PREFIX}/install)
set(PLACEBO_NATIVE  ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)

ExternalProject_Add(libplacebo_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/libplacebo
    PREFIX            ${PLACEBO_PREFIX}
    CONFIGURE_COMMAND meson setup ${PLACEBO_PREFIX}/build
                        ${CMAKE_SOURCE_DIR}/external/libplacebo
                        --native-file ${PLACEBO_NATIVE}
                        --prefix ${PLACEBO_INSTALL}
                        --libdir lib
                        --buildtype debugoptimized
                        -Db_ndebug=false -Dstrip=false
                        -Ddemos=false -Dtests=false
                        -Dc_args=-fno-omit-frame-pointer
    BUILD_COMMAND     ninja -C ${PLACEBO_PREFIX}/build
    INSTALL_COMMAND   ninja -C ${PLACEBO_PREFIX}/build install
    BUILD_BYPRODUCTS  ${PLACEBO_INSTALL}/lib/libplacebo.so
    USES_TERMINAL_BUILD TRUE)

file(MAKE_DIRECTORY ${PLACEBO_INSTALL}/include)
set(PLACEBO_PKGCONFIG_DIR ${PLACEBO_INSTALL}/lib/pkgconfig CACHE INTERNAL "libplacebo .pc dir")

add_library(placebo::placebo SHARED IMPORTED GLOBAL)
set_target_properties(placebo::placebo PROPERTIES
    IMPORTED_LOCATION ${PLACEBO_INSTALL}/lib/libplacebo.so
    INTERFACE_INCLUDE_DIRECTORIES ${PLACEBO_INSTALL}/include)
