#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Owning, move-only CPU pixel buffer (interleaved 8-bit channels, default RGBA8).
// This is the single currency of the image module: decoders produce it, resize
// consumes/produces it, and the render thread uploads it to a GPU texture. It
// NEVER touches Vulkan — that boundary is what keeps the async path safe.

namespace img {

struct ImageBuffer {
    std::vector<std::uint8_t> data;
    int                       width    = 0;
    int                       height   = 0;
    int                       channels = 4;

    ImageBuffer()                                  = default;
    ImageBuffer(ImageBuffer &&) noexcept           = default;
    ImageBuffer &operator=(ImageBuffer &&) noexcept = default;
    // Move-only: pixel buffers can be large; copies must be explicit and rare.
    ImageBuffer(const ImageBuffer &)            = delete;
    ImageBuffer &operator=(const ImageBuffer &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && channels > 0 &&
               data.size() == static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(channels);
    }

    // Bytes per pixel row.
    [[nodiscard]] std::size_t stride() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    }

    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept { return data; }
};

} // namespace img
