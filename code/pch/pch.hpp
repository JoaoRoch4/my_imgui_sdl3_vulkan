#pragma once

// =============================================================================
// C++ Standard Library
// =============================================================================

// Language support
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cfloat>
#include <cassert>
#include <cstdarg>
#include <csetjmp>
#include <csignal>
#include <ctime>

// Memory management
#include <memory>
#include <memory_resource>
#include <new>
#include <scoped_allocator>

// Type traits & reflection
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <concepts>
#include <source_location>

// Utilities
#include <utility>
#include <tuple>
#include <optional>
#include <variant>
#include <any>
#include <expected>
#include <functional>
#include <compare>
#include <bit>
#include <bitset>

// Strings
#include <string>
#include <string_view>
#include <charconv>
#include <format>
#include <print>

// Containers
#include <array>
#include <vector>
#include <deque>
#include <list>
#include <forward_list>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <span>

// Iterators & ranges
#include <iterator>
#include <ranges>

// Algorithms
#include <algorithm>
#include <numeric>
#include <execution>

// Mathematics
#include <cmath>
#include <numbers>
#include <random>
#include <ratio>
#include <complex>
#include <valarray>

// Time
#include <chrono>

// Concurrency
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <semaphore>
#include <latch>
#include <barrier>
#include <stop_token>
#include <pthread.h>

// Input / Output
#include <iostream>
#include <iomanip>
#include <istream>
#include <ostream>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <syncstream>

// Filesystem
#include <filesystem>

// Regular expressions
#include <regex>

// Error handling
#include <exception>
#include <stdexcept>
#include <system_error>

// Localization
#include <locale>
#include <codecvt>

// Coroutines
#include <coroutine>

// Diagnostics
#include <stacktrace>

// Containers adapters
#include <queue>
#include <stack>

// Hashing
#include <functional>

// Initializer lists
#include <initializer_list>

// Searchers
#include <functional>

// C++20/23 synchronization helpers
#include <syncstream>

// Miscellaneous
#include <limits>
#include <version>

// =============================================================================
// POSIX / Linux
// =============================================================================

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>

// =============================================================================
// SDL3
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_vulkan.h>

// =============================================================================
// Vulkan
// =============================================================================

#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    #define VOLK_IMPLEMENTATION
    #include <volk.h>
#endif

// =============================================================================
// Dear ImGui / ImPlot
// =============================================================================

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <implot.h>

// =============================================================================
// OpenGL / EGL
// =============================================================================

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GL/gl.h>
#include <GL/glext.h>

// =============================================================================
// MPV
// =============================================================================

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

// =============================================================================
// libplacebo
// =============================================================================

#include <libplacebo/common.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

// =============================================================================
// Networking
// =============================================================================

#include <curl/curl.h>

// =============================================================================
// Image Codecs
// =============================================================================

#include <webp/decode.h>

// =============================================================================
// Reflection / Serialization
// =============================================================================

#include <rfl.hpp>
#include <rfl/toml.hpp>
#include <rfl/toml/save.hpp>
#include <rfl/toml/write.hpp>

// =============================================================================
// Audio Metadata
// =============================================================================

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>