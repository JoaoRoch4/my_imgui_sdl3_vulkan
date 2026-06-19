# Build libffmpegthumbnailer from external/ffmpegthumbnailer ONCE PER build
# configuration, AS A STATIC ARCHIVE linked against our SAME-CONFIG from-source
# FFmpeg 8.0 (NOT the system FFmpeg 7.1 that the distro's libffmpegthumbnailer.so
# pulls). Each config installs into build/all/thirdparty/ffmpegthumbnailer/<CONFIG>/
# install. The app links the archive directly (see root CMakeLists), ahead of the
# FFmpeg group in the mpv media-link.rsp, so its libav* references resolve against
# the same static FFmpeg the rest of the app uses — one FFmpeg in the process, no
# .so, profilable. See cmake/PerConfigExternal.cmake for the per-config rationale.
include(ExternalProject)

set(FFTHUMB_ROOT ${CMAKE_BINARY_DIR}/thirdparty/ffmpegthumbnailer)
set(FFTHUMB_SRC  ${CMAKE_SOURCE_DIR}/external/ffmpegthumbnailer)
app_resolve_build_configs(_ffthumb_cfgs)

foreach(cfg IN LISTS _ffthumb_cfgs)
    set(_prefix  ${FFTHUMB_ROOT}/${cfg})
    set(_build   ${_prefix}/build)
    set(_install ${_prefix}/install)
    set(_archive ${_install}/lib/libffmpegthumbnailer.a)

    # FindFFmpeg.cmake (in the upstream tree) resolves FFmpeg via pkg_check_modules,
    # so a PKG_CONFIG_PATH pointed at THIS config's static FFmpeg .pc dir makes it
    # find our libav*.a (8.0) instead of the system shared FFmpeg (7.1).
    set(_pc_path "${FFMPEG_PKGCONFIG_DIR_${cfg}}:$ENV{PKG_CONFIG_PATH}")

    # EXCLUDE_FROM_ALL: a bare `ninja` must NOT build every config's thumbnailer.
    # The app's per-$<CONFIG> LINK_DEPENDS on the archive (root CMakeLists) pulls
    # only the config actually being linked, which DEPENDS on — and so builds —
    # the same-config FFmpeg first.
    ExternalProject_Add(ffmpegthumbnailer_ep_${cfg}
        DEPENDS           ffmpeg_ep_${cfg}
        SOURCE_DIR        ${FFTHUMB_SRC}
        PREFIX            ${_prefix}
        BINARY_DIR        ${_build}
        EXCLUDE_FROM_ALL  TRUE
        # Static archive only. ENABLE_SHARED=OFF avoids emitting a .so; JPEG/PNG
        # file writers are force-disabled (we consume the in-memory RGB path), so
        # the archive carries no libjpeg/libpng link dependency.
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${_pc_path}
                            ${CMAKE_COMMAND} -S ${FFTHUMB_SRC} -B ${_build} -G Ninja
                            -DCMAKE_BUILD_TYPE=${cfg}
                            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
                            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                            -DCMAKE_INSTALL_LIBDIR=lib
                            # FindFFmpeg.cmake resolves headers via pkg-config (pinned
                            # to our 8.0 tree above), but its FIND_LIBRARY would
                            # otherwise wander into a system /usr/local FFmpeg. Search
                            # our install/lib first so that path is honest/deterministic
                            # (only used by the upstream exe, which we don't build).
                            -DCMAKE_LIBRARY_PATH=${FFMPEG_INSTALL_DIR_${cfg}}/lib
                            -DENABLE_STATIC=ON -DENABLE_SHARED=OFF
                            -DENABLE_TESTS=OFF -DENABLE_GIO=OFF
                            -DENABLE_THUMBNAILER=OFF -DENABLE_AUDIO_THUMBNAILER=OFF
                            -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON
                            -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON
        # Build ONLY the static lib target — the upstream CMakeLists also defines an
        # unconditional `ffmpegthumbnailer` executable whose real link would need
        # FFmpeg's full static closure (which our header-only pkg_check_modules does
        # NOT provide). Then hand-copy the one archive into the install prefix: a
        # plain `cmake --install` would try to install the (unbuilt) executable and
        # fail. Folding the copy into the BUILD step makes the archive a genuine
        # byproduct of the tracked step (same reasoning as the FFmpeg EP).
        BUILD_COMMAND     ${CMAKE_COMMAND} --build ${_build} --target libffmpegthumbnailerstatic
                  COMMAND ${CMAKE_COMMAND} -E make_directory ${_install}/lib
                  COMMAND ${CMAKE_COMMAND} -E copy ${_build}/libffmpegthumbnailer.a ${_archive}
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  ${_archive}
        USES_TERMINAL_BUILD TRUE)
endforeach()

# Include-only INTERFACE target — the static archive is linked centrally by the app
# (root CMakeLists), so this carries only the header path. The public headers are
# plain source files included as <libffmpegthumbnailer/videothumbnailerc.h>, and the
# upstream static target exposes the source root as its PUBLIC include dir, so we
# point at the source tree (config-invariant) rather than copying headers.
add_library(ffthumb::ffthumb INTERFACE IMPORTED GLOBAL)
set_target_properties(ffthumb::ffthumb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${FFTHUMB_SRC})
