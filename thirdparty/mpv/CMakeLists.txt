# Build libmpv from external/mpv (meson) ONCE PER build configuration, against our
# SAME-CONFIG from-source FFmpeg + libplacebo (discovered via a per-config
# PKG_CONFIG_PATH). Each config installs into build/all/thirdparty/mpv/<CONFIG>/
# install. Output target: mpv::mpv (per-config IMPORTED_LOCATION_<CONFIG>). The app
# links this directly, so linking --config <C> pulls mpv_ep_<C>, which DEPENDS on —
# and therefore builds — ffmpeg_ep_<C> + libplacebo_ep_<C>. See
# cmake/PerConfigExternal.cmake.
include(ExternalProject)

set(MPV_ROOT   ${CMAKE_BINARY_DIR}/thirdparty/mpv)
set(MPV_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/meson-clang-native.ini)
app_resolve_build_configs(_mpv_cfgs)

foreach(cfg IN LISTS _mpv_cfgs)
    set(_prefix  ${MPV_ROOT}/${cfg})
    set(_install ${_prefix}/install)
    set(_build   ${_prefix}/build)
    app_meson_buildtype_args(_meson_flags ${cfg})

    # Same-config sibling pkgconfig dirs (set as INTERNAL cache vars by ffmpeg/libplacebo).
    set(_pc_path "${FFMPEG_PKGCONFIG_DIR_${cfg}}:${PLACEBO_PKGCONFIG_DIR_${cfg}}:$ENV{PKG_CONFIG_PATH}")

    ExternalProject_Add(mpv_ep_${cfg}
        DEPENDS           ffmpeg_ep_${cfg} libplacebo_ep_${cfg}
        SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/mpv
        PREFIX            ${_prefix}
        EXCLUDE_FROM_ALL  TRUE
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${_pc_path}
                            meson setup ${_build}
                            ${CMAKE_SOURCE_DIR}/external/mpv
                            --native-file ${MPV_NATIVE}
                            --prefix ${_install}
                            --libdir lib
                            --default-library static
                            ${_meson_flags}
                            -Dlibmpv=true -Dcplayer=false
                            -Dlua=auto
        # Build AND install in the BUILD step — BUILD_BYPRODUCTS attach to the build
        # step, so install must run there too or ninja links a not-yet-installed .so
        # (we have no config-aware add_dependencies() to order a separate install).
        BUILD_COMMAND     ninja -C ${_build}
                  COMMAND ninja -C ${_build} install
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  ${_install}/lib/libmpv.a
        USES_TERMINAL_BUILD TRUE)

    file(MAKE_DIRECTORY ${_install}/include)
    # Expose this config's pkgconfig dir so the app's static link-flag resolution
    # (pkg-config --static --libs mpv) can find mpv.pc + its Requires.private.
    set(MPV_PKGCONFIG_DIR_${cfg} ${_install}/lib/pkgconfig CACHE INTERNAL "mpv .pc dir (${cfg})")

    # Resolve the FULL static link line (libmpv.a + its transitive closure: the
    # sibling archives, libass/zlib, and system libs) into a response file the app
    # reads via `-Wl,@`. This runs as a STEP of this per-config EP (after install,
    # so the .pc files exist) rather than a root add_custom_command — a root custom
    # command is cross-config in Ninja Multi-Config and would force every config's
    # externals to build. As an EP step the rsp is a byproduct of mpv_ep_${cfg}, so
    # the app's $<CONFIG> LINK_DEPENDS pulls only this config's stack.
    ExternalProject_Add_Step(mpv_ep_${cfg} gen_static_rsp
        DEPENDEES  install
        DEPENDS    ${CMAKE_SOURCE_DIR}/cmake/gen_static_link_rsp.cmake
        BYPRODUCTS ${_prefix}/media-link.rsp
        COMMAND    ${CMAKE_COMMAND}
                     -DPKG_CONFIG=${PKG_CONFIG_EXECUTABLE}
                     -DPC_PATH=${FFMPEG_PKGCONFIG_DIR_${cfg}}:${PLACEBO_PKGCONFIG_DIR_${cfg}}:${_install}/lib/pkgconfig
                     -DMODULE=mpv
                     -DOUT=${_prefix}/media-link.rsp
                     -P ${CMAKE_SOURCE_DIR}/cmake/gen_static_link_rsp.cmake
        COMMENT    "Resolving static media-stack link flags (mpv ${cfg})")
endforeach()

# Include-only INTERFACE target — the static libmpv.a (and its whole transitive
# closure) is linked centrally via the app's pkg-config --static response file (see
# root CMakeLists). This carries only the (config-invariant) header path the app
# compiles against (mpv/client.h, render.h, render_gl.h).
add_library(mpv::mpv INTERFACE IMPORTED GLOBAL)
set_target_properties(mpv::mpv PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${MPV_ROOT}/$<CONFIG>/install/include)
