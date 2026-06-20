# Helpers for building the foreign (autotools / meson) ExternalProject deps once
# PER build configuration. Ninja Multi-Config cannot key an ExternalProject by
# $<CONFIG> (EP step targets are config-agnostic utility targets), so each library
# is instantiated as one EP per config — ffmpeg_ep_Debug, ffmpeg_ep_Release, … —
# into a per-config prefix, and a single IMPORTED target maps each *consuming*
# config to the matching install via IMPORTED_LOCATION_<CONFIG>. The EPs are
# EXCLUDE_FROM_ALL + BUILD_BYPRODUCTS, so a build only materialises the externals
# whose output the current config actually links (build --config Release pulls only
# the Release externals; a bare `ninja` builds none of them on its own).

# Resolve the configurations to build: every type for a multi-config generator,
# else the single CMAKE_BUILD_TYPE (defaulting to Release for an unset single-config
# configure). Writes a CMake list into ${out_var} in the caller's scope.
function(app_resolve_build_configs out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    set(${out_var} "${CMAKE_CONFIGURATION_TYPES}" PARENT_SCOPE)
  elseif(CMAKE_BUILD_TYPE)
    set(${out_var} "${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
  else()
    set(${out_var} "Release" PARENT_SCOPE)
  endif()
endfunction()

# Map a CMake config name to meson configure arguments (shared by libplacebo + mpv,
# both meson builds). Returns a CMake list in ${out_var}:
#   Debug          -> unoptimised, asserts on, frame pointers kept
#   RelWithDebInfo -> optimised, debug info + frame pointers kept (profilable), asserts off
#   Release / other-> optimised, asserts off, stripped
function(app_meson_buildtype_args out_var config)
  if(config STREQUAL "Debug")
    set(_a --buildtype debug          -Db_ndebug=false -Dstrip=false -Dc_args=-fno-omit-frame-pointer)
  elseif(config STREQUAL "RelWithDebInfo")
    set(_a --buildtype debugoptimized -Db_ndebug=true  -Dstrip=false -Dc_args=-fno-omit-frame-pointer)
  else()
    set(_a --buildtype release        -Db_ndebug=true  -Dstrip=true)
  endif()
  set(${out_var} "${_a}" PARENT_SCOPE)
endfunction()

# Map a CMake config name to FFmpeg (autotools) configure arguments. FFmpeg enables
# optimisation by default; we only toggle debug info. Stripping is DISABLED for
# every config on purpose:
#   Debug          -> full debug info (=3), frame pointers, AND C + asm
#                     optimisations OFF (--disable-optimizations --disable-asm) so
#                     FFmpeg is pure-C, -O0, fully steppable in lldb. Decode is
#                     slow — acceptable for a debug build. Bonus: --disable-asm
#                     emits no nasm objects, so this config can't hit the
#                     empty-asm-object failure described below.
#   RelWithDebInfo -> debug info (=2), frame pointers
#   Release / other-> no debug info, still NOT stripped (see note)
#
# NOTE: never pass --enable-stripping. FFmpeg's stripping step runs strip on the
# per-object output of the static build and EMPTIES the nasm-assembled x86 SIMD
# objects (e.g. h264_intrapred.o) — they install as 0-byte archive members, so
# ld.lld reports them "neither ET_REL nor LLVM bitcode" and every
# ff_pred*_mmxext/sse symbol is undefined at the app link. Stripping a static .a
# that gets linked into the app buys nothing (only the final executable is worth
# stripping), so it stays off in all configs.
function(app_ffmpeg_buildtype_args out_var config)
  if(config STREQUAL "Debug")
    set(_a --enable-debug=3 --disable-stripping --disable-optimizations --disable-asm --extra-cflags=-fno-omit-frame-pointer)
  elseif(config STREQUAL "RelWithDebInfo")
    set(_a --enable-debug=2 --disable-stripping --extra-cflags=-fno-omit-frame-pointer)
  else()
    set(_a --disable-debug --disable-stripping)
  endif()
  set(${out_var} "${_a}" PARENT_SCOPE)
endfunction()
