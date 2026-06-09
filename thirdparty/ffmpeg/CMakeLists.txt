# Build FFmpeg 8.0.git from external/FFmpeg via its autotools configure, once,
# with debug symbols and no stripping. Installs into
# build/all/thirdparty/ffmpeg/install; consumed by the mpv build via
# PKG_CONFIG_PATH (FFMPEG_PKGCONFIG_DIR). The app does NOT link FFmpeg directly
# (it is transitive through mpv); the ffmpeg::* IMPORTED targets are provided for
# completeness / future direct use.
include(ExternalProject)

set(FFMPEG_PREFIX    ${CMAKE_BINARY_DIR}/thirdparty/ffmpeg)
set(FFMPEG_INSTALL   ${FFMPEG_PREFIX}/install)
set(FFMPEG_PKGCONFIG ${FFMPEG_INSTALL}/lib/pkgconfig)

ExternalProject_Add(ffmpeg_ep
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/external/FFmpeg
    PREFIX            ${FFMPEG_PREFIX}
    BINARY_DIR        ${FFMPEG_PREFIX}/build
    CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/external/FFmpeg/configure
                        --prefix=${FFMPEG_INSTALL}
                        --cc=clang --cxx=clang++
                        --enable-shared --disable-static
                        --enable-gpl --enable-version3
                        --disable-programs --disable-doc --disable-htmlpages
                        --disable-manpages --disable-podpages --disable-txtpages
                        --disable-stripping --enable-debug=3
                        --extra-cflags=-fno-omit-frame-pointer
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_BYPRODUCTS
        ${FFMPEG_INSTALL}/lib/libavcodec.so
        ${FFMPEG_INSTALL}/lib/libavformat.so
        ${FFMPEG_INSTALL}/lib/libavutil.so
        ${FFMPEG_INSTALL}/lib/libswscale.so
        ${FFMPEG_INSTALL}/lib/libswresample.so
        ${FFMPEG_INSTALL}/lib/libavfilter.so
    USES_TERMINAL_BUILD TRUE)

# Make the install include dir exist at configure time so IMPORTED targets validate.
file(MAKE_DIRECTORY ${FFMPEG_INSTALL}/include)

# Expose the FFmpeg pkgconfig dir + install prefix to siblings (mpv).
set(FFMPEG_PKGCONFIG_DIR ${FFMPEG_PKGCONFIG} CACHE INTERNAL "FFmpeg .pc dir")
set(FFMPEG_INSTALL_DIR   ${FFMPEG_INSTALL}   CACHE INTERNAL "FFmpeg install prefix")

foreach(_lib avcodec avformat avutil swscale swresample avfilter)
    add_library(ffmpeg::${_lib} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${_lib} PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_INSTALL}/lib/lib${_lib}.so
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL}/include)
endforeach()
