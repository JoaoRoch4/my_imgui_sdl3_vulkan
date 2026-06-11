# Run at BUILD time (cmake -P) to resolve the full static link line for a pkg-config
# module and write it to a linker response file the app consumes via `-Wl,@<file>`.
#
# The from-source media stack (FFmpeg / libplacebo / mpv) is built as static
# archives. A `.a` carries only its own objects, so the app must link mpv's entire
# transitive closure (the other archives + libass/zlib + system libs). pkg-config
# knows that closure — but only once the .pc files exist, which is after the
# ExternalProjects install. So this runs as a custom command (mpv EP step), not at
# CMake configure time.
#
# Invoked as:
#   cmake -DPKG_CONFIG=<exe> -DPC_PATH=<a:b:c> -DMODULE=mpv -DOUT=<file> \
#         -P gen_static_link_rsp.cmake
#
# Output is consumed via `-Wl,@<file>` (the linker reads it), so the file holds
# LINKER syntax. Two things are normalised for the linker:
#   * driver-only flags (-pthread, -Wl,) → linker equivalents
#   * any `-l<name>` whose dev symlink (lib<name>.so / .a) is absent — common for
#     system libs that ship only lib<name>.so.N (no -devel package) — is rewritten
#     to the full path of the newest lib<name>.so.N, so a bare `-l` can't fail.

foreach(_req PKG_CONFIG PC_PATH MODULE OUT)
  if(NOT DEFINED ${_req})
    message(FATAL_ERROR "gen_static_link_rsp: -D${_req} is required")
  endif()
endforeach()

set(ENV{PKG_CONFIG_PATH} "${PC_PATH}")

execute_process(
  COMMAND "${PKG_CONFIG}" --static --libs "${MODULE}"
  OUTPUT_VARIABLE _libs
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE  _err
  RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
    "pkg-config --static --libs ${MODULE} failed (rc=${_rc})\n"
    "  PKG_CONFIG_PATH=${PC_PATH}\n  ${_err}")
endif()

separate_arguments(_tokens NATIVE_COMMAND "${_libs}")

# Pass 1 — split the search dirs so OUR install trees (the -L dirs from the .pc
# files) take priority over system dirs. System dirs are 64-bit ONLY: this is a
# multilib host, so /lib and /usr/lib hold 32-bit libs that would be incompatible
# if linked by full path.
set(_install_dirs "")
foreach(_t IN LISTS _tokens)
  if(_t MATCHES "^-L(.+)$")
    list(APPEND _install_dirs "${CMAKE_MATCH_1}")
  endif()
endforeach()
set(_sys_dirs /usr/lib64 /lib64 /usr/lib/x86_64-linux-gnu)

# Resolve a -l<name>:
#  1. If OUR install tree holds a static lib<name>.a, link it by FULL PATH. This is
#     what makes the static stack robust against same-named SYSTEM shared libs of a
#     different ABI (e.g. a system libplacebo.so.351 vs our libplacebo.a @ API 364).
#  2. Else if a linkable lib<name>.so / .a exists in a system dir, keep -l<name>
#     (the linker resolves it, skipping wrong-arch candidates).
#  3. Else (runtime-only lib shipping just lib<name>.so.N, no dev symlink — e.g. acl,
#     lz4), use the full path of the newest 64-bit lib<name>.so.N.
function(_resolve_l name out)
  foreach(_d IN LISTS _install_dirs)
    if(EXISTS "${_d}/lib${name}.a")
      set(${out} "${_d}/lib${name}.a" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  foreach(_d IN LISTS _install_dirs _sys_dirs)
    if(EXISTS "${_d}/lib${name}.so" OR EXISTS "${_d}/lib${name}.a")
      set(${out} "-l${name}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  foreach(_d IN LISTS _sys_dirs)
    file(GLOB _cands "${_d}/lib${name}.so.*")
    if(_cands)
      list(SORT _cands)
      list(GET _cands -1 _cand)
      set(${out} "${_cand}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "-l${name}" PARENT_SCOPE)       # leave it; linker will surface a real gap
endfunction()

# Pass 2 — normalise driver flags and rewrite unresolvable -l<name>.
set(_clean "")
foreach(_t IN LISTS _tokens)
  if(_t STREQUAL "-pthread")
    list(APPEND _clean "-lpthread")
  elseif(_t MATCHES "^-Wl,")
    string(REGEX REPLACE "^-Wl," "" _t "${_t}")
    string(REPLACE "," ";" _t "${_t}")
    list(APPEND _clean ${_t})
  elseif(_t MATCHES "^-l(.+)$")
    _resolve_l("${CMAKE_MATCH_1}" _r)
    list(APPEND _clean "${_r}")
  else()
    list(APPEND _clean "${_t}")
  endif()
endforeach()
list(JOIN _clean " " _joined)

# Emit a RUNPATH for any -L dir that holds shared libs and is NOT a default loader
# dir, so transitive system libs in non-standard plugin dirs resolve at RUNTIME.
# Going static flattens (e.g.) libmpv -> libpulse -> libpulsecommon into our DIRECT
# link, but the app doesn't inherit libpulse's own RUNPATH, so without this the app
# links yet fails to start ("cannot open libpulsecommon-17.0.so", which lives in
# /usr/lib64/pulseaudio). Our own install dirs hold only .a, so they need no rpath.
set(_default_dirs /usr/lib64 /lib64 /usr/lib /lib)
set(_rpaths "")
foreach(_d IN LISTS _install_dirs)
  list(FIND _default_dirs "${_d}" _isdef)
  if(_isdef EQUAL -1)
    file(GLOB _sos "${_d}/*.so*")
    if(_sos)
      list(APPEND _rpaths "-rpath" "${_d}")
    endif()
  endif()
endforeach()
list(JOIN _rpaths " " _rpath_str)

# The static archives are mutually recursive (FFmpeg's libav* especially), so wrap
# the whole list in a linker group to let the linker re-scan for symbols.
file(WRITE "${OUT}" "${_rpath_str} --start-group ${_joined} --end-group\n")

message(STATUS "gen_static_link_rsp: wrote ${OUT}")
