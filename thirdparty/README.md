# thirdparty/ — per-library build glue

Each subfolder holds the **authored CMake glue** to build one external library
from source. The library *sources* live under the git-ignored `external/<lib>/`.

Cloned (not vendored) sources — re-fetch with:

    git -C external/libplacebo submodule update --init --depth 1 \
      3rdparty/glad 3rdparty/jinja 3rdparty/markupsafe \
      3rdparty/Vulkan-Headers 3rdparty/fast_float
    git clone --depth 1 --branch vulkan-sdk-1.4.350.0 \
      https://github.com/KhronosGroup/Vulkan-Loader.git  external/Vulkan-Loader
    git clone --depth 1 --branch vulkan-sdk-1.4.350.0 \
      https://github.com/KhronosGroup/Vulkan-Headers.git external/Vulkan-Headers

Build outputs go to `build/all/thirdparty/<lib>/` (git-ignored).
