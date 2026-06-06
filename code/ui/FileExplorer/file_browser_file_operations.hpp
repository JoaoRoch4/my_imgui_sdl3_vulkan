#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// File-operations service for the file browser, split into:
//   * fileops::  — pure std::filesystem functions (no ImGui / Vulkan / ManagedThread
//                  / PCH), unit-tested in isolation. They never throw across the API
//                  boundary (std::error_code only) and report progress + honour an
//                  abort callback so long operations can be cancelled mid-flight.
//   * FileBrowserFileOperations (added in a later step) — a thin async wrapper that
//     runs the heavy ops on a single serial worker and exposes poll-based progress.

namespace fileops {

// Result of an operation. `error` is empty on success.
struct Result {
    bool        ok = true;
    std::string error;

    [[nodiscard]] static Result success() { return {}; }
    [[nodiscard]] static Result failure(std::string e) { return {false, std::move(e)}; }
};

// Live progress for a long-running op. `total` is filled during a counting phase,
// then `done` / `current` advance as entries are processed.
struct Progress {
    std::uint64_t         done  = 0;
    std::uint64_t         total = 0;
    std::filesystem::path current;
};

// Returns true to abort the operation (checked between entries).
using AbortFn = std::function<bool()>;

// Create a directory (and any missing parents). Success if it exists afterwards.
[[nodiscard]] Result create_directory(const std::filesystem::path &dir);

// Rename / move within a filesystem (no recursion, no copy fallback).
[[nodiscard]] Result rename(const std::filesystem::path &from, const std::filesystem::path &to);

// Recursively remove a file or directory tree. Counts entries into progress.total,
// then removes them updating progress.done / current. Honours abort().
[[nodiscard]] Result remove_recursive(const std::filesystem::path &target,
                                      Progress &progress, const AbortFn &abort);

// Recursively copy a file or directory tree to `to` (cross-device safe).
[[nodiscard]] Result copy_tree(const std::filesystem::path &from,
                               const std::filesystem::path &to, Progress &progress,
                               const AbortFn &abort);

// Move `from` to `to`: try a fast rename; on cross-device failure fall back to
// copy_tree + remove_recursive.
[[nodiscard]] Result move_path(const std::filesystem::path &from,
                               const std::filesystem::path &to, Progress &progress,
                               const AbortFn &abort);

} // namespace fileops
