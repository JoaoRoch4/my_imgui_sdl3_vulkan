#pragma once

// Plain aggregates / enums for the parallel image module. Deliberately free of any
// ImGui / Vulkan / project-PCH dependency so the module compiles standalone and is
// unit-testable in isolation. Kept as simple types so the deferred reflection layer
// can adopt them later with zero churn.

namespace img {

// Scheduling hint for ImageJobSystem submissions.
enum class Priority { Low, Normal, High };

// Failure reasons for the fallible image operations. Returned via std::expected.
enum class ImageError {
    FileNotFound,
    UnsupportedFormat,
    DecodeFailed,
    ResizeFailed,
    EncodeFailed,
    Cancelled,
    OutOfMemory,
};

} // namespace img
