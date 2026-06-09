# Build SDL3 (3.4.10) from external/SDL3-3.4.10 as a shared lib with debug
# symbols. Output target: SDL3::SDL3 (alias of SDL3-shared).
set(SDL_SHARED       ON  CACHE BOOL "" FORCE)
set(SDL_STATIC       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/SDL3-3.4.10
    ${CMAKE_BINARY_DIR}/thirdparty/sdl3
    EXCLUDE_FROM_ALL)
app_profilable_treatment(SDL3-shared)
