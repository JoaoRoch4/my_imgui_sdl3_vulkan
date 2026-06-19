# Build FFmpeg 8.0.git from external/FFmpeg via its autotools configure, ONCE PER
# build configuration. Each config installs into
# build/all/thirdparty/ffmpeg/<CONFIG>/install and is consumed by the same-config
# mpv build via PKG_CONFIG_PATH (FFMPEG_PKGCONFIG_DIR_<CONFIG>). The app does NOT
# link FFmpeg directly (it is transitive through mpv); the ffmpeg::* IMPORTED
# targets are provided for completeness / future direct use, with per-config
# locations. See cmake/PerConfigExternal.cmake for the per-config rationale.
include(ExternalProject)

set(FFMPEG_ROOT ${CMAKE_BINARY_DIR}/thirdparty/ffmpeg)
app_resolve_build_configs(_ffmpeg_cfgs)

foreach(cfg IN LISTS _ffmpeg_cfgs)
    set(_prefix  ${FFMPEG_ROOT}/${cfg})
    set(_install ${_prefix}/install)
    app_ffmpeg_buildtype_args(_ff_flags ${cfg})

    # EXCLUDE_FROM_ALL: a bare `ninja` must NOT build every config's FFmpeg. The EP
    # is pulled only when the same-config mpv build (DEPENDS, below) needs it.
    ExternalProject_Add(ffmpeg_ep_${cfg}
        SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/FFmpeg
        PREFIX            ${_prefix}
        BINARY_DIR        ${_prefix}/build
        EXCLUDE_FROM_ALL  TRUE
        CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/external/FFmpeg/configure
                            --prefix=${_install}
                            --cc=clang --cxx=clang++
                            # Static archives, linked into the app for profilability.
                            # --enable-pic so the objects link into the PIE executable;
                            # --pkg-config-flags=--static so FFmpeg records its own deps
                            # for static consumers in the generated .pc files.
                            --enable-static --disable-shared --enable-pic
                            --pkg-config-flags=--static
                            --enable-gpl --enable-version3
                            --disable-programs --disable-doc --disable-htmlpages
                            --disable-manpages --disable-podpages --disable-txtpages
                            ${_ff_flags}
        # Build AND install in the BUILD step. BUILD_BYPRODUCTS attach to the build
        # step, but the install-tree .so files below are created by `make install`;
        # with a separate INSTALL step nothing forces install to run before the app
        # links its IMPORTED_LOCATION (we have no config-aware add_dependencies()),
        # so an interrupted build leaves a fresh -build stamp but no installed .so
        # and the link fails on a missing file. Folding install into build makes the
        # byproducts genuinely produced by the tracked step.
        BUILD_COMMAND     make -j
                  COMMAND make install
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS
            ${_install}/lib/libavcodec.a
            ${_install}/lib/libavformat.a
            ${_install}/lib/libavutil.a
            ${_install}/lib/libswscale.a
            ${_install}/lib/libswresample.a
            ${_install}/lib/libavfilter.a
        USES_TERMINAL_BUILD TRUE)

    # Make the install include dir exist at configure time so IMPORTED targets validate.
    file(MAKE_DIRECTORY ${_install}/include)

    # Expose this config's pkgconfig dir + install prefix to the same-config mpv build.
    set(FFMPEG_PKGCONFIG_DIR_${cfg} ${_install}/lib/pkgconfig CACHE INTERNAL "FFmpeg .pc dir (${cfg})")
    set(FFMPEG_INSTALL_DIR_${cfg}   ${_install}               CACHE INTERNAL "FFmpeg install prefix (${cfg})")

    # Resolve the FULL static link line for the image-core FFmpeg surface into a
    # response file the image_tests / image_job_tests targets read via `-Wl,@`.
    # Modules: avformat+avcodec+swscale (decode_file/encode_png) + avfilter
    # (ffmpegthumbnailer's internal scaling/film-strip filter graph) — pkg-config
    # pulls their transitive closure (avutil/swresample/postproc + system libs like
    # lzma/z/drm/va/X11). image_ops.cpp uses libav* directly and doesn't go through
    # mpv, hence this dedicated FFmpeg-only rsp. Same EP-step rationale as mpv's media
    # rsp: runs after install (so the .pc files exist), is a per-config byproduct, and
    # a root custom command would be cross-config in Ninja Multi-Config.
    ExternalProject_Add_Step(ffmpeg_ep_${cfg} gen_static_rsp
        DEPENDEES  install
        DEPENDS    ${CMAKE_SOURCE_DIR}/cmake/gen_static_link_rsp.cmake
        BYPRODUCTS ${_prefix}/ffmpeg-link.rsp
        COMMAND    ${CMAKE_COMMAND}
                     -DPKG_CONFIG=${PKG_CONFIG_EXECUTABLE}
                     -DPC_PATH=${_install}/lib/pkgconfig
                     "-DMODULE=libavfilter libavformat libavcodec libswscale"
                     -DOUT=${_prefix}/ffmpeg-link.rsp
                     -P ${CMAKE_SOURCE_DIR}/cmake/gen_static_link_rsp.cmake
        COMMENT    "Resolving static FFmpeg link flags (${cfg})")
endforeach()

# Include-only INTERFACE targets per FFmpeg library. The actual static archives are
# linked centrally through the app's pkg-config --static response file (see root
# CMakeLists), so these carry only the (config-invariant) header path. The app does
# not use FFmpeg headers directly; kept for completeness / future direct use.
foreach(_lib avcodec avformat avutil swscale swresample avfilter)
    add_library(ffmpeg::${_lib} INTERFACE IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${_lib} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_ROOT}/$<CONFIG>/install/include)
endforeach()
