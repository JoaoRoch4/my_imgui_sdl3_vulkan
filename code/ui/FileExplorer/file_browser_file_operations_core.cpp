#include "file_browser_file_operations.hpp"

#include <algorithm>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace fileops {

namespace {

// Collect a path and all its descendants (no throw). `target` itself is included.
std::vector<fs::path> collect_tree(const fs::path &target) {
    std::vector<fs::path> out;
    out.push_back(target);

    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(target, ec)) && !ec) {
        for (auto it = fs::recursive_directory_iterator(
                 target, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            out.push_back(it->path());
        }
    }
    return out;
}

bool aborted(const AbortFn &abort) { return abort && abort(); }

} // namespace

Result create_directory(const fs::path &dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (fs::is_directory(dir))
        return Result::success();
    return Result::failure(ec ? ec.message() : "failed to create directory");
}

Result rename(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec)
        return Result::failure(ec.message());
    return Result::success();
}

Result remove_recursive(const fs::path &target, Progress &progress, const AbortFn &abort) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(target, ec)) || ec)
        return Result::failure("path does not exist");

    std::vector<fs::path> entries = collect_tree(target);
    // Deepest paths first: a descendant's path string is strictly longer than any
    // ancestor's, so this guarantees children are removed before their parent dir.
    std::sort(entries.begin(), entries.end(),
              [](const fs::path &a, const fs::path &b) {
                  return a.native().size() > b.native().size();
              });

    progress.total = entries.size();
    progress.done  = 0;

    for (const auto &e : entries) {
        if (aborted(abort))
            return Result::failure("operation aborted");
        progress.current = e;
        std::error_code rec;
        fs::remove(e, rec);
        if (rec)
            return Result::failure(rec.message());
        ++progress.done;
    }
    return Result::success();
}

Result copy_tree(const fs::path &from, const fs::path &to, Progress &progress,
                 const AbortFn &abort) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(from, ec)) || ec)
        return Result::failure("source does not exist");

    std::vector<fs::path> entries = collect_tree(from);
    // Shallowest first so a directory is created before anything inside it.
    std::sort(entries.begin(), entries.end(),
              [](const fs::path &a, const fs::path &b) {
                  return a.native().size() < b.native().size();
              });

    progress.total = entries.size();
    progress.done  = 0;

    for (const auto &src : entries) {
        if (aborted(abort))
            return Result::failure("operation aborted");
        progress.current = src;

        std::error_code rel_ec;
        const fs::path  rel = fs::relative(src, from, rel_ec);
        const fs::path  dst = (src == from) ? to : (to / rel);

        std::error_code cec;
        if (fs::is_directory(fs::symlink_status(src))) {
            fs::create_directories(dst, cec);
        } else {
            fs::create_directories(dst.parent_path(), cec);
            if (!cec)
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
        }
        if (cec)
            return Result::failure(cec.message());
        ++progress.done;
    }
    return Result::success();
}

Result move_path(const fs::path &from, const fs::path &to, Progress &progress,
                 const AbortFn &abort) {
    // Fast path: a plain rename works within the same filesystem.
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        progress.total = 1;
        progress.done  = 1;
        return Result::success();
    }

    // Cross-device (or other) failure: copy then remove the source.
    const Result copied = copy_tree(from, to, progress, abort);
    if (!copied.ok)
        return copied;

    Progress     rm_progress;
    const Result removed = remove_recursive(from, rm_progress, abort);
    if (!removed.ok)
        return removed;
    return Result::success();
}

} // namespace fileops
