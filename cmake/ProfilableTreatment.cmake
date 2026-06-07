# Applies the uniform "profilable" treatment to a CMake target we build from
# source: default symbol visibility (names survive into .dynsym), DWARF debug
# info in every config (so a sampling profiler maps addresses -> source), and
# kept frame pointers (exact stack unwinding). Mirrors the historical stb/imgui
# treatment. Safe to call on C or C++ targets.
function(app_profilable_treatment tgt)
  if(NOT TARGET ${tgt})
    message(FATAL_ERROR "app_profilable_treatment: '${tgt}' is not a target")
  endif()
  set_target_properties(${tgt} PROPERTIES
    C_VISIBILITY_PRESET       default
    CXX_VISIBILITY_PRESET     default
    VISIBILITY_INLINES_HIDDEN OFF)
  target_compile_options(${tgt} PRIVATE
    -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
endfunction()
