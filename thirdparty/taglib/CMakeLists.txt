# Build TagLib from external/taglib as a static lib with debug symbols.
# Output target: tag.
set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS     OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_SOURCE_DIR}/external/taglib
    ${CMAKE_BINARY_DIR}/thirdparty/taglib
    EXCLUDE_FROM_ALL)
app_profilable_treatment(tag)
