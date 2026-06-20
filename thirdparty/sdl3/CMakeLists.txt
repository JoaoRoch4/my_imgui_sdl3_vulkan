# Build SDL3 (3.5.0 dev checkout, external/SDL) as a STATIC archive, once per build
# configuration — the same treatment the FFmpeg/mpv/libplacebo media stack gets:
# static .a baked into the app, no runtime .so / rpath. Under Ninja Multi-Config
# every config compiles its own libSDL3.a (Debug gets the _d postfix) into
# build/all/thirdparty/sdl3/build/<CONFIG>; EXCLUDE_FROM_ALL means a config only
# builds when the app — which links SDL3::SDL3 — actually needs it (a bare `ninja`
# builds none). Unlike FFmpeg, SDL is a native CMake project, so add_subdirectory
# gives this per-config behaviour for free — no ExternalProject wrapper needed.
#
# Output target: SDL3::SDL3. SDL aliases it to SDL3-static whenever SDL3-shared is
# absent (SDL_SHARED OFF), so every existing SDL3::SDL3 consumer (the app + the
# imgui backend lib) is redirected to the static archive with no call-site change.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

# The app is a PIE (-pie); a static archive linked into it must be position-
# independent or the link dies on R_X86_64_32 relocations. This SDL checkout only
# marks SDL3-shared as PIC, so force PIC on the whole subtree before adding it
# (mirrors FFmpeg's --enable-pic). Inherited by SDL3-static + its helper targets.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/SDL
    ${CMAKE_BINARY_DIR}/thirdparty/sdl3/build
    EXCLUDE_FROM_ALL)

# Default symbol visibility + DWARF + frame pointers in every config, so a sampling
# profiler can resolve SDL frames (matches the stb/imgui/FFmpeg treatment).
app_profilable_treatment(SDL3-static)
