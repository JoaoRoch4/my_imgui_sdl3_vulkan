#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "file_browser_file_operations.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_counter{0};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("fbops_async_" + std::to_string(g_counter.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Poll a job to a terminal state, with a safety timeout. Returns the final status.
FileBrowserFileOperations::JobStatus
wait_done(FileBrowserFileOperations &ops, FileBrowserFileOperations::JobId id) {
    FileBrowserFileOperations::JobStatus st;
    for (int i = 0; i < 1000; ++i) { // up to ~10s
        if (ops.poll(id, st) && st.state != FileBrowserFileOperations::JobStatus::State::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return st;
}

} // namespace

TEST_CASE("FileBrowserFileOperations: sync create_directory and rename") {
    TempDir                   tmp;
    FileBrowserFileOperations ops;
    ops.start();

    CHECK(ops.create_directory(tmp.path / "newdir").ok);
    CHECK(fs::is_directory(tmp.path / "newdir"));

    std::ofstream(tmp.path / "a.txt") << "x";
    CHECK(ops.rename(tmp.path / "a.txt", tmp.path / "b.txt").ok);
    CHECK(fs::exists(tmp.path / "b.txt"));

    ops.shutdown();
}

TEST_CASE("FileBrowserFileOperations: async copy completes with progress") {
    TempDir tmp;
    const auto from = tmp.path / "src";
    const auto to   = tmp.path / "dst";
    fs::create_directories(from / "sub");
    std::ofstream(from / "a.txt") << "AAA";
    std::ofstream(from / "sub" / "b.txt") << "BBB";

    FileBrowserFileOperations ops;
    ops.start();
    const auto id = ops.submit_copy(from, to);
    const auto st = wait_done(ops, id);

    CHECK(st.state == FileBrowserFileOperations::JobStatus::State::Done);
    CHECK(st.total > 0);
    CHECK(st.done == st.total);
    CHECK(fs::exists(to / "sub" / "b.txt"));

    ops.shutdown();
}

TEST_CASE("FileBrowserFileOperations: async remove deletes a tree") {
    TempDir tmp;
    const auto root = tmp.path / "tree";
    fs::create_directories(root / "sub");
    std::ofstream(root / "f.txt") << "z";
    std::ofstream(root / "sub" / "g.txt") << "z";

    FileBrowserFileOperations ops;
    ops.start();
    const auto id = ops.submit_remove(root);
    const auto st = wait_done(ops, id);

    CHECK(st.state == FileBrowserFileOperations::JobStatus::State::Done);
    CHECK_FALSE(fs::exists(root));

    ops.shutdown();
}

TEST_CASE("FileBrowserFileOperations: poll on unknown id returns false") {
    FileBrowserFileOperations           ops;
    FileBrowserFileOperations::JobStatus st;
    CHECK_FALSE(ops.poll(99999, st));
}
