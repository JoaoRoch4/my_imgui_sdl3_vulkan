# Build libmpv from external/mpv (meson) once, with debug symbols, against our
# from-source FFmpeg (and from-source libplacebo) discovered via PKG_CONFIG_PATH.
# Installs into build/all/thirdparty/mpv/install. Output target: mpv::mpv.
include(ExternalProject)

set(MPV_PREFIX  ${CMAKE_BINARY_DIR}/thirdparty/mpv)
set(MPV_INSTALL ${MPV_PREFIX}/install)
set(MPV_NATIVE  ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)

# Resolve our sibling install prefixes (set as INTERNAL cache vars by their dirs).
set(_pc_path "${FFMPEG_PKGCONFIG_DIR}:${PLACEBO_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")

ExternalProject_Add(mpv_ep
    DEPENDS           ffmpeg_ep libplacebo_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/mpv
    PREFIX            ${MPV_PREFIX}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${_pc_path}
                        meson setup ${MPV_PREFIX}/build
                        ${CMAKE_SOURCE_DIR}/external/mpv
                        --native-file ${MPV_NATIVE}
                        --prefix ${MPV_INSTALL}
                        --libdir lib
                        --buildtype debugoptimized
                        -Db_ndebug=false -Dstrip=false
                        -Dlibmpv=true -Dcplayer=false
                        -Dlua=disabled
                        -Dc_args=-fno-omit-frame-pointer
    BUILD_COMMAND     ninja -C ${MPV_PREFIX}/build
    INSTALL_COMMAND   ninja -C ${MPV_PREFIX}/build install
    BUILD_BYPRODUCTS  ${MPV_INSTALL}/lib/libmpv.so
    USES_TERMINAL_BUILD TRUE)

file(MAKE_DIRECTORY ${MPV_INSTALL}/include)

add_library(mpv::mpv SHARED IMPORTED GLOBAL)
set_target_properties(mpv::mpv PROPERTIES
    IMPORTED_LOCATION ${MPV_INSTALL}/lib/libmpv.so
    INTERFACE_INCLUDE_DIRECTORIES ${MPV_INSTALL}/include)
