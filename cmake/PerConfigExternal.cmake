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
# optimisation by default; we only toggle debug info / stripping:
#   Debug          -> full debug info (=3), unstripped, frame pointers (optimised, so
#                     video stays usable while remaining steppable)
#   RelWithDebInfo -> debug info (=2), unstripped, frame pointers
#   Release / other-> no debug info, stripped
function(app_ffmpeg_buildtype_args out_var config)
  if(config STREQUAL "Debug")
    set(_a --enable-debug=3 --disable-stripping --extra-cflags=-fno-omit-frame-pointer)
  elseif(config STREQUAL "RelWithDebInfo")
    set(_a --enable-debug=2 --disable-stripping --extra-cflags=-fno-omit-frame-pointer)
  else()
    set(_a --disable-debug --enable-stripping)
  endif()
  set(${out_var} "${_a}" PARENT_SCOPE)
endfunction()
