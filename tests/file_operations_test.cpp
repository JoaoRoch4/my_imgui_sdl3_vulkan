#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "file_browser_file_operations.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_counter{0};

// Unique temporary directory, removed on scope exit.
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("fbops_" + std::to_string(g_counter.fetch_add(1)) + "_test");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path &p, std::string_view content) {
    std::ofstream(p) << content;
}

const fileops::AbortFn no_abort     = [] { return false; };
const fileops::AbortFn always_abort = [] { return true; };

} // namespace

TEST_CASE("create_directory creates nested directories") {
    TempDir    tmp;
    const auto d = tmp.path / "a" / "b" / "c";
    const auto r = fileops::create_directory(d);
    CHECK(r.ok);
    CHECK(fs::is_directory(d));
}

TEST_CASE("create_directory on an existing directory succeeds") {
    TempDir    tmp;
    const auto r = fileops::create_directory(tmp.path);
    CHECK(r.ok);
}

TEST_CASE("create_directory fails when a parent component is a file") {
    TempDir tmp;
    write_file(tmp.path / "afile", "x");
    const auto r = fileops::create_directory(tmp.path / "afile" / "sub");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("rename moves a file") {
    TempDir    tmp;
    const auto src = tmp.path / "x.txt";
    const auto dst = tmp.path / "y.txt";
    write_file(src, "hi");
    const auto r = fileops::rename(src, dst);
    CHECK(r.ok);
    CHECK_FALSE(fs::exists(src));
    CHECK(fs::exists(dst));
}

TEST_CASE("remove_recursive deletes a tree and reports progress") {
    TempDir    tmp;
    const auto root = tmp.path / "tree";
    fs::create_directories(root / "sub1");
    fs::create_directories(root / "sub2");
    write_file(root / "f0.txt", "a");
    write_file(root / "sub1" / "f1.txt", "b");
    write_file(root / "sub2" / "f2.txt", "c");

    fileops::Progress p;
    const auto        r = fileops::remove_recursive(root, p, no_abort);
    CHECK(r.ok);
    CHECK_FALSE(fs::exists(root));
    CHECK(p.total > 0);
    CHECK(p.done == p.total);
}

TEST_CASE("copy_tree duplicates a directory tree with contents") {
    TempDir    tmp;
    const auto from = tmp.path / "src";
    const auto to   = tmp.path / "dst";
    fs::create_directories(from / "sub");
    write_file(from / "a.txt", "AAA");
    write_file(from / "sub" / "b.txt", "BBB");

    fileops::Progress p;
    const auto        r = fileops::copy_tree(from, to, p, no_abort);
    CHECK(r.ok);
    REQUIRE(fs::exists(to / "sub" / "b.txt"));
    std::ifstream in(to / "sub" / "b.txt");
    std::string   s;
    in >> s;
    CHECK(s == "BBB");
}

TEST_CASE("move_path relocates a tree") {
    TempDir    tmp;
    const auto from = tmp.path / "m_src";
    const auto to   = tmp.path / "m_dst";
    fs::create_directories(from);
    write_file(from / "a.txt", "Z");

    fileops::Progress p;
    const auto        r = fileops::move_path(from, to, p, no_abort);
    CHECK(r.ok);
    CHECK_FALSE(fs::exists(from));
    CHECK(fs::exists(to / "a.txt"));
}

TEST_CASE("copy_tree honours abort before doing work") {
    TempDir    tmp;
    const auto from = tmp.path / "big";
    const auto to   = tmp.path / "big_copy";
    fs::create_directories(from);
    write_file(from / "f.txt", "x");

    fileops::Progress p;
    const auto        r = fileops::copy_tree(from, to, p, always_abort);
    CHECK_FALSE(r.ok);
}
