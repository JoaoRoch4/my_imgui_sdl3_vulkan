# FileBrowser Async Thumbnails + File-Ops Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `ImGui::FileBrowser` into three browser-owned units (`FileBrowserFileOperations`, `FileBrowserThumbnailContext`, `FileBrowserThumbnailThread`), move thumbnail generation into a browser-owned async engine wired to `ThreadOverwatch`, kill the per-frame render-thread stalls, and retire the old `AppContext`-owned `FileThumbnailCache`.

**Architecture:** UI → context → thread, strictly one-way. `FileBrowserFileOperations` is pure `std::filesystem` + a serial worker. `FileBrowserThumbnailContext` is the only Vulkan-touching unit (render-thread cache + budgeted GPU upload + deferred-free eviction); it owns `FileBrowserThumbnailThread`, a persistent decode pool that returns RGBA in-memory and writes PNGs for persistence. Both new workers register per-job `ThreadOverwatch` watches (KillOnly) exactly like the existing `FileBrowserScanner`.

**Tech Stack:** C++23, clang/clang++ + LLD, Ninja Multi-Config, Vulkan, Dear ImGui, libmpv (SW render), stb_image / stb_image_resize2 / stb_image_write, libwebp.

---

## Conventions & Verification Model (read first)

- **Code style (project memory):** strict modern C++23 — `static_cast`/`std::bit_cast` only (no C casts), smart pointers, `std::span`/`string_view`, `constexpr`, attached braces, `std::println`/`std::format` for logs. Every new `.cpp`/`.hpp` starts with `#include "pch.hpp"`.
- **No unit-test harness exists** in this repo (verified: no `enable_testing`/`add_test`, no test targets, no Catch2/gtest). Per writing-plans "follow established patterns," each task is verified by a **green Debug build** and, for behavioral tasks, a **manual smoke checklist** — these replace the standard TDD test-runner steps.
- **Configure (once):** `cmake --preset all`
- **Build (Debug) — the per-task verification command:**
  ```bash
  cmake --build --preset build-debug
  ```
  Expected: `ninja: build stopped` only on error; success ends with the link step for the app target and exit code 0.
- **Run (manual smoke):** launch the Debug binary in `build/debug/` (the same binary you normally debug via lldb-dap).
- **Keep every commit green:** the task order below guarantees the build compiles and links after each task. Do not reorder.

### Shared type/name contract (used across tasks — keep consistent)

| Symbol | Defined in | Signature |
| --- | --- | --- |
| `VulkanTexture::load_from_rgba` | Task 1 | `bool load_from_rgba(std::span<const std::uint8_t> rgba, int w, int h, vulkan_context& vk)` |
| `FileBrowserFileOperations::Result` | Task 2 | `struct { bool ok = true; std::string error; }` |
| `FileBrowserFileOperations::JobId` | Task 2 | `std::uint64_t` |
| `FileBrowserFileOperations::Progress` | Task 2 | `{ State state; uint64_t done,total; path current; string error; }` |
| `FileBrowserThumbnailThread::DoneFn` | Task 3 | `std::function<void(const std::string& key, std::vector<std::uint8_t> rgba, bool ok)>` |
| `FileBrowserThumbnailThread::k_thumb_w / k_thumb_h` | Task 3 | `constexpr int` = 320 / 180 |
| `FileBrowserThumbnailThread::is_image_ext` | Task 3 | `static bool is_image_ext(const std::filesystem::path&)` |
| `FileBrowserThumbnailContext::get` | Task 4 | `ImTextureID get(const std::filesystem::path&)` |
| `FileBrowser::Setup` | Task 5 | `void Setup(vulkan_context*, std::filesystem::path)` |
| `FileBrowser::ShutdownThumbnails` | Task 5 | `void ShutdownThumbnails()` |
| `FileBrowser::FileOps` | Task 5 | `FileBrowserFileOperations& FileOps() noexcept` |

---

## Task 1: `VulkanTexture::load_from_rgba` (refactor `load`)

Add a raw-RGBA upload path so freshly-decoded thumbnails skip the PNG round-trip. Refactor the existing `load(path)` to decode-then-delegate, so the proven GPU upload code has exactly one copy.

**Files:**
- Modify: `code/rendering/vulkan/vulkan_texture.hpp` (add declaration)
- Modify: `code/rendering/vulkan/vulkan_texture.cpp:96-274` (split `load`)

- [ ] **Step 1: Declare `load_from_rgba` in the header**

In `vulkan_texture.hpp`, after the `load` declaration (line 27), add:

```cpp
    // Upload an already-decoded RGBA8 buffer (w*h*4 bytes) to the GPU.
    // Returns false on failure. Use this to avoid a disk round-trip.
    bool load_from_rgba(std::span<const std::uint8_t> rgba, int w, int h, vulkan_context& vk);
```

- [ ] **Step 2: Refactor `load` to decode then call `load_from_rgba`**

In `vulkan_texture.cpp`, replace the body of `load` (lines 96-274) so that it only performs decode (current step 1), then delegates. Keep the existing decode block verbatim, then:

```cpp
bool VulkanTexture::load(const std::filesystem::path &path, vulkan_context &vk) {
    constexpr int k_channels = 4;
    int ch = 0, w = 0, h = 0;
    unsigned char *pixels = nullptr;
    bool is_webp = false;

    if (path.extension() == ".webp" || path.extension() == ".WEBP") {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::vector<std::uint8_t> buf(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(std::bit_cast<char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
        pixels = WebPDecodeRGBA(buf.data(), buf.size(), &w, &h);
        is_webp = true;
    } else {
        pixels = stbi_load(path.string().c_str(), &w, &h, &ch, k_channels);
    }
    if (!pixels) return false;

    const bool ok = load_from_rgba(
        std::span<const std::uint8_t>(pixels, static_cast<size_t>(w) * h * k_channels), w, h, vk);

    if (is_webp) WebPFree(pixels); else stbi_image_free(pixels);
    return ok;
}
```

- [ ] **Step 3: Add `load_from_rgba` containing the moved GPU-upload code**

Immediately after `load`, add `load_from_rgba` built from the **current** `load` GPU code (old lines 131-273 — "Create GPU Image" through "Execute Transfer"), with `pixels` now coming from the span and **no** `cleanup_pixels()` calls (the caller owns the buffer):

```cpp
bool VulkanTexture::load_from_rgba(std::span<const std::uint8_t> rgba, int w, int h, vulkan_context &vk) {
    constexpr int k_channels = 4;
    if (rgba.size() < static_cast<size_t>(w) * h * k_channels) return false;
    width = w;
    height = h;
    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * k_channels;
    VkResult err;

    // 2. Create GPU Image  — copy old vulkan_texture.cpp lines 135-169 verbatim,
    //    but on the find_memory_type failure / vkCreateImage failure paths return false
    //    WITHOUT calling cleanup_pixels() (the caller owns the pixels).
    // 3. Create View & Sampler — copy old lines 172-187 verbatim.
    // 4. ImGui Registration — copy old line 190 verbatim.
    // 5. Staging & Upload — copy old lines 192-215 verbatim, but the memcpy source
    //    becomes rgba.data():  std::memcpy(map_ptr, rgba.data(), static_cast<size_t>(image_size));
    // 6. Execute Transfer — copy old lines 217-266 verbatim.
    // 7. Cleanup temporary staging — copy old lines 269-270 (destroy staging_buf/mem),
    //    but DROP the cleanup_pixels() call.
    return true;
}
```

Engineer note: this is a mechanical move. The only behavioral changes are (a) `pixels` → `rgba.data()` in the staging memcpy, and (b) remove all three `cleanup_pixels()` call sites (the failure-path ones return false directly). Everything else — barriers, fence wait, command buffer — is unchanged.

- [ ] **Step 4: Build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0. (`load` behavior is unchanged for existing callers; only the internal structure changed.)

- [ ] **Step 5: Commit**

```bash
git add code/rendering/vulkan/vulkan_texture.hpp code/rendering/vulkan/vulkan_texture.cpp
git commit -m "feat(vulkan): add VulkanTexture::load_from_rgba; load() delegates to it

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `FileBrowserFileOperations`

Pure-filesystem mutation service. Fast metadata ops are synchronous; heavy ops (recursive remove, copy, move) run on one serial worker with progress + `ThreadOverwatch`.

**Files:**
- Create: `code/ui/FileExplorer/file_browser_file_operations.hpp`
- Create: `code/ui/FileExplorer/file_browser_file_operations.cpp`
- Modify: `CMakeLists.txt:185-188` (add the `.cpp`)

- [ ] **Step 1: Write the header**

Create `code/ui/FileExplorer/file_browser_file_operations.hpp`:

```cpp
#pragma once

#include "pch.hpp"

#include <condition_variable>
#include <deque>
#include <stop_token>

/// Filesystem mutation service for the file browser.
///
/// Fast metadata ops (create_directory, rename) run synchronously. Heavy ops
/// (recursive remove, copy, move) are queued on a single serial worker thread so
/// they never block the render thread; progress is polled via poll(). The worker
/// registers a per-job ThreadOverwatch watch (KillOnly) and heartbeats per entry,
/// mirroring FileBrowserScanner.
class FileBrowserFileOperations {
public:
    FileBrowserFileOperations();
    ~FileBrowserFileOperations();

    FileBrowserFileOperations(const FileBrowserFileOperations&)            = delete;
    FileBrowserFileOperations& operator=(const FileBrowserFileOperations&) = delete;

    struct Result {
        bool        ok = true;
        std::string error; // empty on success
    };

    // Fast metadata ops — synchronous, instant feedback.
    Result create_directory(const std::filesystem::path& dir);
    Result rename(const std::filesystem::path& from, const std::filesystem::path& to);

    using JobId = std::uint64_t;

    // Heavy ops — queued on the worker, never block the UI. Returns the job id.
    JobId submit_remove(const std::filesystem::path& target);                      // recursive
    JobId submit_copy(const std::filesystem::path& from, const std::filesystem::path& to);
    JobId submit_move(const std::filesystem::path& from, const std::filesystem::path& to);

    struct Progress {
        enum class State { Running, Done, Failed };
        State                 state = State::Running;
        std::uint64_t         done  = 0;
        std::uint64_t         total = 0;
        std::filesystem::path current;
        std::string           error; // populated on Failed
    };

    // Non-blocking (render thread). false if `id` is unknown. A Done/Failed job is
    // reported once then erased, so poll it until it returns a terminal state.
    bool poll(JobId id, Progress& out);

    void shutdown(); // stop + join the worker

private:
    enum class Op { Remove, Copy, Move };
    struct Job {
        JobId                 id;
        Op                    op;
        std::filesystem::path from;
        std::filesystem::path to;
    };

    JobId enqueue(Op op, std::filesystem::path from, std::filesystem::path to);
    void  worker_loop(std::stop_token st);
    void  run_job(const Job& job, std::uint64_t watch_id);
    std::uint64_t count_entries(const std::filesystem::path& root) const;

    std::mutex                          m_mutex;
    std::condition_variable_any         m_cv;
    std::deque<Job>                     m_queue;
    std::unordered_map<JobId, Progress> m_progress;
    std::atomic<JobId>                  m_next_id{1};
    std::atomic<bool>                   m_kill_requested{false};
    std::jthread                        m_worker;
};
```

- [ ] **Step 2: Write the implementation**

Create `code/ui/FileExplorer/file_browser_file_operations.cpp`:

```cpp
#include "pch.hpp"

#include "file_browser_file_operations.hpp"

#include "core/thread/thread_overwatch.hpp"

FileBrowserFileOperations::FileBrowserFileOperations()
    : m_worker{[this](std::stop_token st) { worker_loop(std::move(st)); }} {}

FileBrowserFileOperations::~FileBrowserFileOperations() { shutdown(); }

void FileBrowserFileOperations::shutdown() {
    m_worker.request_stop();
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

// ---- synchronous fast ops --------------------------------------------------

FileBrowserFileOperations::Result
FileBrowserFileOperations::create_directory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {false, ec.message()};
    return {};
}

FileBrowserFileOperations::Result
FileBrowserFileOperations::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
        return {false, ec.message()};
    return {};
}

// ---- async heavy ops -------------------------------------------------------

FileBrowserFileOperations::JobId
FileBrowserFileOperations::enqueue(Op op, std::filesystem::path from, std::filesystem::path to) {
    const JobId id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        m_progress[id] = Progress{Progress::State::Running, 0, 0, from, {}};
        m_queue.push_back(Job{id, op, std::move(from), std::move(to)});
    }
    m_cv.notify_one();
    return id;
}

FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_remove(const std::filesystem::path& target) {
    return enqueue(Op::Remove, target, {});
}
FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    return enqueue(Op::Copy, from, to);
}
FileBrowserFileOperations::JobId
FileBrowserFileOperations::submit_move(const std::filesystem::path& from, const std::filesystem::path& to) {
    return enqueue(Op::Move, from, to);
}

bool FileBrowserFileOperations::poll(JobId id, Progress& out) {
    std::lock_guard lock(m_mutex);
    auto it = m_progress.find(id);
    if (it == m_progress.end())
        return false;
    out = it->second;
    if (it->second.state != Progress::State::Running)
        m_progress.erase(it); // terminal state reported once
    return true;
}

std::uint64_t FileBrowserFileOperations::count_entries(const std::filesystem::path& root) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return 1;
    std::uint64_t n = 1; // count root itself
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

void FileBrowserFileOperations::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, st, [this] { return !m_queue.empty(); });
            if (st.stop_requested())
                break;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }

        m_kill_requested.store(false, std::memory_order_release);

        const std::uint64_t watch_id = ThreadOverwatch::instance().watch(
            "FileBrowserFileOps", std::chrono::milliseconds(10000),
            [this] { m_kill_requested.store(true, std::memory_order_release); }, nullptr,
            ThreadOverwatch::RecoveryPolicy::KillOnly);

        run_job(job, watch_id);

        ThreadOverwatch::instance().unwatch(watch_id);
    }
}

void FileBrowserFileOperations::run_job(const Job& job, std::uint64_t watch_id) {
    const auto aborted = [&] {
        return m_kill_requested.load(std::memory_order_acquire);
    };
    const auto set_progress = [&](std::uint64_t done, std::uint64_t total,
                                  const std::filesystem::path& current) {
        std::lock_guard lock(m_mutex);
        if (auto it = m_progress.find(job.id); it != m_progress.end()) {
            it->second.done = done;
            it->second.total = total;
            it->second.current = current;
        }
    };
    const auto finish = [&](bool ok, std::string err) {
        std::lock_guard lock(m_mutex);
        if (auto it = m_progress.find(job.id); it != m_progress.end()) {
            it->second.state = ok ? Progress::State::Done : Progress::State::Failed;
            it->second.error = std::move(err);
        }
    };

    const std::uint64_t total = count_entries(job.from);
    std::uint64_t done = 0;
    std::error_code ec;

    switch (job.op) {
    case Op::Remove: {
        // Recursive, entry-by-entry so we can heartbeat + honour aborts.
        if (std::filesystem::is_directory(job.from, ec)) {
            std::vector<std::filesystem::path> entries;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     job.from, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                entries.push_back(it->path());
            }
            // Remove deepest-first so directories are empty when removed.
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.string().size() > b.string().size(); });
            for (const auto& p : entries) {
                if (aborted()) { finish(false, "operation aborted"); return; }
                std::filesystem::remove(p, ec);
                ThreadOverwatch::instance().heartbeat(watch_id);
                set_progress(++done, total, p);
            }
        }
        std::filesystem::remove(job.from, ec);
        if (ec) { finish(false, ec.message()); return; }
        finish(true, {});
        return;
    }
    case Op::Copy:
    case Op::Move: {
        // Move fast-path: same-filesystem rename is atomic + instant.
        if (job.op == Op::Move) {
            std::error_code rec;
            std::filesystem::rename(job.from, job.to, rec);
            if (!rec) { finish(true, {}); return; }
            // else fall through to copy-then-remove (cross-device).
        }
        if (std::filesystem::is_directory(job.from, ec)) {
            std::filesystem::create_directories(job.to, ec);
            for (auto it = std::filesystem::recursive_directory_iterator(
                     job.from, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (aborted()) { finish(false, "operation aborted"); return; }
                const auto rel = std::filesystem::relative(it->path(), job.from, ec);
                const auto dst = job.to / rel;
                std::error_code cec;
                if (it->is_directory())
                    std::filesystem::create_directories(dst, cec);
                else
                    std::filesystem::copy_file(it->path(), dst,
                        std::filesystem::copy_options::overwrite_existing, cec);
                if (cec) { finish(false, cec.message()); return; }
                ThreadOverwatch::instance().heartbeat(watch_id);
                set_progress(++done, total, it->path());
            }
        } else {
            std::filesystem::copy_file(job.from, job.to,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { finish(false, ec.message()); return; }
            set_progress(++done, total, job.from);
        }
        if (job.op == Op::Move) {
            std::error_code rmec;
            std::filesystem::remove_all(job.from, rmec); // cross-device move cleanup
        }
        finish(true, {});
        return;
    }
    }
}
```

- [ ] **Step 3: Add the `.cpp` to CMake**

In `CMakeLists.txt`, after line 185 (`code/ui/FileExplorer/file_browser_ui.cpp`), add:

```
    code/ui/FileExplorer/file_browser_file_operations.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --preset all && cmake --build --preset build-debug`
Expected: exit code 0. The class is not yet used; it compiles and links (the worker thread starts but idles).

- [ ] **Step 5: Commit**

```bash
git add code/ui/FileExplorer/file_browser_file_operations.hpp code/ui/FileExplorer/file_browser_file_operations.cpp CMakeLists.txt
git commit -m "feat(filebrowser): add FileBrowserFileOperations service (sync + async worker)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `FileBrowserThumbnailThread`

Persistent decode pool. Relocates the existing image (stb/webp) and video (libmpv SW-render) decode code from `file_thumbnail_cache.cpp`, adapted to **return** the RGBA buffer (in-memory handoff) **and** write the PNG (persistence). Wired to `ThreadOverwatch`.

**Files:**
- Create: `code/ui/FileExplorer/file_browser_thumbnail_context_thread.hpp`
- Create: `code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp`
- Modify: `CMakeLists.txt` (add the `.cpp`)

- [ ] **Step 1: Write the header**

Create `code/ui/FileExplorer/file_browser_thumbnail_context_thread.hpp`:

```cpp
#pragma once

#include "pch.hpp"

#include <condition_variable>
#include <deque>
#include <stop_token>

/// Persistent off-thread thumbnail decode pool for the file browser.
///
/// Image jobs (stb/webp) and video jobs (libmpv SW render) run in separate
/// worker pools so a slow video decode never blocks image thumbnails. Each
/// finished job invokes the DoneFn with the decoded RGBA buffer (k_thumb_w x
/// k_thumb_h x 4) so the context can upload it without a disk round-trip; the
/// worker also writes the PNG for cross-session persistence. Each in-flight job
/// holds a per-job ThreadOverwatch watch (KillOnly); idle workers hold none.
///
/// No Vulkan, no ImGui — generator threads must never touch the GPU.
class FileBrowserThumbnailThread {
public:
    /// Invoked on a worker thread when a job finishes. `rgba` is empty + ok=false
    /// on failure. The callback must take its own locks; it runs off the render thread.
    using DoneFn = std::function<void(const std::string& key,
                                      std::vector<std::uint8_t> rgba, bool ok)>;

    FileBrowserThumbnailThread();
    ~FileBrowserThumbnailThread();

    FileBrowserThumbnailThread(const FileBrowserThumbnailThread&)            = delete;
    FileBrowserThumbnailThread& operator=(const FileBrowserThumbnailThread&) = delete;

    void start(DoneFn on_done);                              // spawn the pools
    void submit(std::string key, std::filesystem::path file,
                std::filesystem::path out_png, bool is_image);
    void clear_pending();                                    // drop queued (not in-flight) jobs
    void shutdown();                                         // stop + join all workers

    [[nodiscard]] static bool is_image_ext(const std::filesystem::path& p);

    /// On-disk + in-memory thumbnail dimensions (pixels).
    static constexpr int k_thumb_w = 320;
    static constexpr int k_thumb_h = 180;

    static constexpr int k_image_workers = 4; // matches retired FileThumbnailCache
    static constexpr int k_video_workers = 2;

private:
    struct Job {
        std::string           key;
        std::filesystem::path file;
        std::filesystem::path out_png;
    };

    void worker_loop(std::stop_token st, bool image_pool);
    /// Decode `file` to k_thumb_w x k_thumb_h RGBA, write `out_png`, return pixels.
    /// Empty vector on failure. Honours st + the per-job abort flag; heartbeats
    /// `watch_id` so a long (but progressing) video decode never trips the watchdog.
    std::vector<std::uint8_t> generate(const std::filesystem::path& file,
                                       const std::filesystem::path& out_png,
                                       const std::stop_token& st,
                                       std::uint64_t watch_id,
                                       const std::atomic<bool>& abort);

    std::mutex                  m_mutex;
    std::condition_variable_any m_cv;
    std::deque<Job>             m_image_queue;
    std::deque<Job>             m_video_queue;
    DoneFn                      m_on_done;
    std::vector<std::jthread>   m_workers;
};
```

- [ ] **Step 2: Write the implementation**

Create `code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp`. The anonymous-namespace decode helpers are **moved from `file_thumbnail_cache.cpp`** with one adaptation each (return the RGBA buffer instead of `void`):

```cpp
#include "pch.hpp"

#include "file_browser_thumbnail_context_thread.hpp"

#include "core/thread/thread_overwatch.hpp"

#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <stb_image_write.h>

namespace {

// Copy is_image_ext from file_thumbnail_cache.cpp:22-27 verbatim.
bool is_image_ext_impl(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp";
}

// Adapted from file_thumbnail_cache.cpp generate_image_thumbnail (lines 29-80):
// same decode + resize, but RETURN the dst RGBA (and still write the PNG).
// Returns empty on failure.
std::vector<std::uint8_t> gen_image(const std::filesystem::path& file,
                                    const std::filesystem::path& out_png) {
    constexpr int k_channels = 4;
    int src_w = 0, src_h = 0;
    std::uint8_t* pixels = nullptr;
    bool is_webp = false;
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".webp") {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return {};
        std::vector<std::uint8_t> buf(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(std::bit_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        pixels = WebPDecodeRGBA(buf.data(), buf.size(), &src_w, &src_h);
        is_webp = true;
    } else {
        int ch = 0;
        pixels = stbi_load(file.string().c_str(), &src_w, &src_h, &ch, k_channels);
    }
    if (!pixels) return {};

    std::vector<std::uint8_t> dst(static_cast<size_t>(FileBrowserThumbnailThread::k_thumb_w) *
                                  FileBrowserThumbnailThread::k_thumb_h * k_channels);
    stbir_resize_uint8_linear(pixels, src_w, src_h, 0, dst.data(),
                              FileBrowserThumbnailThread::k_thumb_w,
                              FileBrowserThumbnailThread::k_thumb_h, 0, STBIR_RGBA);
    if (is_webp) WebPFree(pixels); else stbi_image_free(pixels);

    std::error_code ec;
    std::filesystem::create_directories(out_png.parent_path(), ec);
    stbi_write_png(out_png.string().c_str(), FileBrowserThumbnailThread::k_thumb_w,
                   FileBrowserThumbnailThread::k_thumb_h, k_channels, dst.data(),
                   FileBrowserThumbnailThread::k_thumb_w * k_channels);
    return dst;
}

} // namespace

bool FileBrowserThumbnailThread::is_image_ext(const std::filesystem::path& p) {
    return is_image_ext_impl(p);
}

FileBrowserThumbnailThread::FileBrowserThumbnailThread() = default;
FileBrowserThumbnailThread::~FileBrowserThumbnailThread() { shutdown(); }

void FileBrowserThumbnailThread::start(DoneFn on_done) {
    m_on_done = std::move(on_done);
    for (int i = 0; i < k_image_workers; ++i)
        m_workers.emplace_back([this](std::stop_token st) { worker_loop(std::move(st), true); });
    for (int i = 0; i < k_video_workers; ++i)
        m_workers.emplace_back([this](std::stop_token st) { worker_loop(std::move(st), false); });
}

void FileBrowserThumbnailThread::submit(std::string key, std::filesystem::path file,
                                        std::filesystem::path out_png, bool is_image) {
    {
        std::lock_guard lock(m_mutex);
        Job job{std::move(key), std::move(file), std::move(out_png)};
        // Visible-first: push front so the latest request is serviced first.
        if (is_image) m_image_queue.push_front(std::move(job));
        else          m_video_queue.push_front(std::move(job));
    }
    m_cv.notify_all();
}

void FileBrowserThumbnailThread::clear_pending() {
    std::lock_guard lock(m_mutex);
    m_image_queue.clear();
    m_video_queue.clear();
}

void FileBrowserThumbnailThread::shutdown() {
    for (auto& t : m_workers)
        t.request_stop();
    m_cv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable())
            t.join();
    m_workers.clear();
}

void FileBrowserThumbnailThread::worker_loop(std::stop_token st, bool image_pool) {
    auto& queue = image_pool ? m_image_queue : m_video_queue;
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, st, [&] { return !queue.empty(); });
            if (st.stop_requested())
                break;
            job = std::move(queue.front());
            queue.pop_front();
        }

        // Per-job abort flag, kept alive via shared_ptr captured BY VALUE in the
        // kill lambda. ThreadOverwatch copies kill_request and may call it AFTER
        // unwatch() returns (it calls outside its lock), so a stack-local flag
        // would be a use-after-scope. shared_ptr keeps it alive as long as the
        // monitor's copied std::function lives.
        auto abort = std::make_shared<std::atomic<bool>>(false);
        const std::uint64_t watch_id = ThreadOverwatch::instance().watch(
            image_pool ? "FileBrowserThumb::img" : "FileBrowserThumb::vid",
            std::chrono::milliseconds(8000),
            [abort] { abort->store(true, std::memory_order_release); }, nullptr,
            ThreadOverwatch::RecoveryPolicy::KillOnly);
        ThreadOverwatch::instance().heartbeat(watch_id);

        std::vector<std::uint8_t> rgba = generate(job.file, job.out_png, st, watch_id, *abort);

        ThreadOverwatch::instance().unwatch(watch_id);

        if (st.stop_requested())
            break;
        const bool ok = !rgba.empty();
        if (m_on_done)
            m_on_done(job.key, std::move(rgba), ok);
    }
}

std::vector<std::uint8_t> FileBrowserThumbnailThread::generate(
    const std::filesystem::path& file, const std::filesystem::path& out_png,
    const std::stop_token& st, std::uint64_t watch_id, const std::atomic<bool>& abort) {
    if (is_image_ext(file))
        return gen_image(file, out_png); // fast; watch_id/abort unused

    // --- Video path: paste the mpv body from file_thumbnail_cache.cpp:296-410 here,
    //     with these exact adaptations:
    //       * declare the output buffer:
    //             std::vector<std::uint8_t> buf(static_cast<size_t>(k_thumb_w) * k_thumb_h * 4);
    //       * every early `return;` becomes `return {};`
    //       * in BOTH while-loops, replace the loop condition guard
    //             while (!st.stop_requested() && ...)
    //         with
    //             while (!st.stop_requested()
    //                    && !abort.load(std::memory_order_acquire)
    //                    && ...)
    //         and call ThreadOverwatch::instance().heartbeat(watch_id); once per iteration.
    //       * the standalone `if (st.stop_requested())` checks also OR-in
    //             abort.load(std::memory_order_acquire)
    //       * on success (after the stbi_write_png call) `return buf;`
    return {};
}
```

Engineer note on the video path: this is a verbatim paste of `file_thumbnail_cache.cpp:296-410` with only the five mechanical edits listed in the comment above. The heartbeat-per-iteration is **not optional** — the 8 s watch timeout is shorter than the worst-case decode (8 s reconfig wait + 5 s render wait = 13 s), so without heartbeats a perfectly healthy video decode would be killed mid-flight.

- [ ] **Step 3: Add the `.cpp` to CMake**

In `CMakeLists.txt`, after the `file_browser_file_operations.cpp` line added in Task 2, add:

```
    code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0.

⚠️ **`STB_IMAGE_RESIZE2_IMPLEMENTATION` must be defined in exactly one TU.** It currently lives in `file_thumbnail_cache.cpp:7`. That file is still in the build until Task 6, so for **this** task define it here only if a link error for duplicate `stbir_*` does NOT occur. If the build reports duplicate `stbir_resize_uint8_linear`, temporarily remove the `#define STB_IMAGE_RESIZE2_IMPLEMENTATION` from `file_thumbnail_cache.cpp:7` now (it is deleted in Task 6 anyway) and rebuild. Document whichever you did in the commit message.

- [ ] **Step 5: Commit**

```bash
git add code/ui/FileExplorer/file_browser_thumbnail_context_thread.hpp code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp CMakeLists.txt
# include file_thumbnail_cache.cpp only if you had to move the STB define
git commit -m "feat(filebrowser): add FileBrowserThumbnailThread persistent decode pool

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: `FileBrowserThumbnailContext`

Render-thread front-end: owns the entry map + the decode thread, does budgeted GPU upload and deferred-free eviction, exposes the cheap-key `get()`.

**Files:**
- Create: `code/ui/FileExplorer/file_browser_thumbnail_context.hpp`
- Create: `code/ui/FileExplorer/file_browser_thumbnail_context.cpp`
- Modify: `CMakeLists.txt` (add the `.cpp`)

- [ ] **Step 1: Write the header**

Create `code/ui/FileExplorer/file_browser_thumbnail_context.hpp`:

```cpp
#pragma once

#include "pch.hpp"

#include "file_browser_thumbnail_context_thread.hpp"

class vulkan_context;
class VulkanTexture;

/// Render-thread thumbnail cache + GPU upload for the file browser.
///
/// Owns a FileBrowserThumbnailThread. get() returns a ready ImTextureID or 0 while
/// the thumbnail is being generated/uploaded. Anti-lag: cheap lexical key (no
/// per-frame syscall), an upload budget (k_max_uploads_per_frame), and deferred-free
/// eviction (no vkDeviceWaitIdle). setup()/get()/evict()/clear()/shutdown() are
/// render-thread only (they touch Vulkan).
class FileBrowserThumbnailContext {
public:
    FileBrowserThumbnailContext();
    ~FileBrowserThumbnailContext();

    FileBrowserThumbnailContext(const FileBrowserThumbnailContext&)            = delete;
    FileBrowserThumbnailContext& operator=(const FileBrowserThumbnailContext&) = delete;

    void setup(vulkan_context* vk, std::filesystem::path thumb_dir);
    void shutdown();
    [[nodiscard]] bool is_setup() const noexcept { return m_setup; }

    /// Top of Display(): reset the per-frame upload budget + free retired textures.
    void new_frame();

    [[nodiscard]] ImTextureID get(const std::filesystem::path& path);
    void evict(const std::filesystem::path& path);
    void clear();

    /// True for the media types the browser shows thumbnails for (images + videos).
    [[nodiscard]] static bool is_thumbnailable(const std::filesystem::path& p);

    static constexpr int k_max_uploads_per_frame = 4;
    static constexpr int k_retire_frames         = 3;

private:
    enum class State { Queued, Generating, PixelsReady, DiskReady, Ready, Failed };
    struct Entry {
        State                          state = State::Queued;
        std::filesystem::path          png_path;
        std::vector<std::uint8_t>      pixels;   // RGBA handed back by the worker
        std::unique_ptr<VulkanTexture> texture;
    };
    struct Retired { std::unique_ptr<VulkanTexture> texture; int frames_left; };

    static std::string           key_for(const std::filesystem::path& path);
    std::filesystem::path        png_for(const std::filesystem::path& file) const;
    void                         on_generated(const std::string& key,
                                              std::vector<std::uint8_t> rgba, bool ok);

    std::mutex                             m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<Retired>                   m_retire;
    int                                    m_uploads_this_frame = 0;
    vulkan_context*                        m_vk = nullptr;
    std::filesystem::path                  m_thumb_dir;
    bool                                   m_setup = false;
    FileBrowserThumbnailThread             m_thread; // declared last → joined first on destroy
};
```

- [ ] **Step 2: Write the implementation**

Create `code/ui/FileExplorer/file_browser_thumbnail_context.cpp`. The fnv1a hash + `png_for` are **moved from `file_thumbnail_cache.cpp`** (lines 13-20, 192-198):

```cpp
#include "pch.hpp"

#include "file_browser_thumbnail_context.hpp"

#include "rendering/vulkan/vulkan_texture.hpp"
#include "rendering/vulkan/vulkan_context.hpp"
#include "ui/media/video/player/video_player.hpp"

namespace {
std::uint64_t fnv1a_hash(const std::string& s) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
} // namespace

FileBrowserThumbnailContext::FileBrowserThumbnailContext() = default;
FileBrowserThumbnailContext::~FileBrowserThumbnailContext() { shutdown(); }

bool FileBrowserThumbnailContext::is_thumbnailable(const std::filesystem::path& p) {
    return FileBrowserThumbnailThread::is_image_ext(p) || VideoPlayer::is_video_path(p);
}

std::string FileBrowserThumbnailContext::key_for(const std::filesystem::path& path) {
    // Cheap, no filesystem syscall (paths from the listing are already absolute).
    return path.lexically_normal().string();
}

std::filesystem::path FileBrowserThumbnailContext::png_for(const std::filesystem::path& file) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a_hash(key_for(file))));
    return m_thumb_dir / (std::string(hex) + ".png");
}

void FileBrowserThumbnailContext::setup(vulkan_context* vk, std::filesystem::path thumb_dir) {
    m_vk = vk;
    m_thumb_dir = std::move(thumb_dir);
    m_setup = true;
    std::error_code ec;
    std::filesystem::create_directories(m_thumb_dir, ec);
    m_thread.start([this](const std::string& key, std::vector<std::uint8_t> rgba, bool ok) {
        on_generated(key, std::move(rgba), ok);
    });
}

void FileBrowserThumbnailContext::shutdown() {
    m_thread.shutdown(); // join workers before touching m_entries / Vulkan
    m_setup = false;
    if (!m_vk) { m_entries.clear(); m_retire.clear(); return; }
    std::lock_guard lock(m_mutex);
    for (auto& [k, e] : m_entries)
        if (e.texture && e.texture->is_loaded())
            e.texture->unload(*m_vk);
    for (auto& r : m_retire)
        if (r.texture && r.texture->is_loaded())
            r.texture->unload(*m_vk);
    m_entries.clear();
    m_retire.clear();
    m_vk = nullptr;
}

void FileBrowserThumbnailContext::new_frame() {
    m_uploads_this_frame = 0;
    if (!m_vk) return;
    std::lock_guard lock(m_mutex);
    for (auto it = m_retire.begin(); it != m_retire.end();) {
        if (--it->frames_left <= 0) {
            if (it->texture && it->texture->is_loaded())
                it->texture->unload(*m_vk);
            it = m_retire.erase(it);
        } else {
            ++it;
        }
    }
}

void FileBrowserThumbnailContext::on_generated(const std::string& key,
                                               std::vector<std::uint8_t> rgba, bool ok) {
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (ok && !rgba.empty()) {
        it->second.pixels = std::move(rgba);
        it->second.state = State::PixelsReady;
    } else {
        it->second.state = State::Failed;
    }
}

ImTextureID FileBrowserThumbnailContext::get(const std::filesystem::path& path) {
    if (!m_setup || !m_vk)
        return 0;
    const std::string key = key_for(path);

    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        Entry e;
        e.png_path = png_for(path);
        std::error_code ec;
        e.state = (std::filesystem::exists(e.png_path, ec) && !ec) ? State::DiskReady : State::Queued;
        it = m_entries.emplace(key, std::move(e)).first;
        if (it->second.state == State::Queued) {
            it->second.state = State::Generating;
            m_thread.submit(key, path, it->second.png_path,
                            FileBrowserThumbnailThread::is_image_ext(path));
        }
    }

    Entry& e = it->second;

    // Budgeted GPU upload (render thread).
    if ((e.state == State::PixelsReady || e.state == State::DiskReady)
        && m_uploads_this_frame < k_max_uploads_per_frame) {
        e.texture = std::make_unique<VulkanTexture>();
        bool ok = false;
        if (e.state == State::PixelsReady) {
            ok = e.texture->load_from_rgba(e.pixels, FileBrowserThumbnailThread::k_thumb_w,
                                           FileBrowserThumbnailThread::k_thumb_h, *m_vk);
            e.pixels.clear();
            e.pixels.shrink_to_fit();
        } else {
            ok = e.texture->load(e.png_path, *m_vk);
        }
        ++m_uploads_this_frame;
        e.state = ok ? State::Ready : State::Failed;
        if (!ok)
            e.texture.reset();
    }

    if (e.state == State::Ready && e.texture)
        return e.texture->imgui_id();
    return 0;
}

void FileBrowserThumbnailContext::evict(const std::filesystem::path& path) {
    if (!m_setup)
        return;
    const std::string key = key_for(path);
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (it->second.texture)
        m_retire.push_back({std::move(it->second.texture), k_retire_frames}); // deferred free
    if (!it->second.png_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(it->second.png_path, ec);
    }
    m_entries.erase(it);
}

void FileBrowserThumbnailContext::clear() {
    if (!m_setup)
        return;
    m_thread.clear_pending();
    std::lock_guard lock(m_mutex);
    for (auto& [k, e] : m_entries) {
        if (e.texture)
            m_retire.push_back({std::move(e.texture), k_retire_frames});
        if (!e.png_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(e.png_path, ec);
        }
    }
    m_entries.clear();
}
```

- [ ] **Step 3: Add the `.cpp` to CMake**

In `CMakeLists.txt`, after the thread `.cpp` line, add:

```
    code/ui/FileExplorer/file_browser_thumbnail_context.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0. (Still unused; compiles + links.)

- [ ] **Step 5: Commit**

```bash
git add code/ui/FileExplorer/file_browser_thumbnail_context.hpp code/ui/FileExplorer/file_browser_thumbnail_context.cpp CMakeLists.txt
git commit -m "feat(filebrowser): add FileBrowserThumbnailContext (budgeted upload, deferred evict)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Integrate the engine into `FileBrowser`

Add the two members + the new public API, swap the render paths to the internal engine, route the new-dir popup through file-ops, and show file-ops progress in the status bar. Keep `SetThumbnailProvider`/`thumbnailProvider_` temporarily (removed in Task 6) so `app_coordinator` still compiles → build stays green.

**Files:**
- Modify: `code/ui/FileExplorer/file_browser_ui.hpp`
- Modify: `code/ui/FileExplorer/file_browser_ui.cpp`

- [ ] **Step 1: Header — includes, public API, members**

In `file_browser_ui.hpp`:

1. After `#include "file_browser_thread.hpp"` (line 5), add:
```cpp
#include "file_browser_file_operations.hpp"
#include "file_browser_thumbnail_context.hpp"

class vulkan_context;
```
2. In the `public:` section (near `Open()`/`Close()`), add:
```cpp
    // Attach Vulkan + the on-disk thumbnail cache dir. Call once after construction.
    void Setup(vulkan_context* vk, std::filesystem::path thumb_dir);
    // Release thumbnail GPU textures + join decode threads. Call before Vulkan teardown.
    void ShutdownThumbnails();
    // Drop all cached thumbnails (memory + on-disk PNGs); regenerated on next Display().
    void ClearThumbnailCache();
    // Drop one file's thumbnail so it regenerates on next Display().
    void RebuildThumbnail(const std::filesystem::path& path);
    // Access the file-operations service (used by the context menu for async delete).
    [[nodiscard]] FileBrowserFileOperations& FileOps() noexcept { return m_file_ops; }
```
3. In the private members block (near `FileBrowserScanner m_scanner;`, line 270), add:
```cpp
    FileBrowserFileOperations   m_file_ops;
    FileBrowserThumbnailContext m_thumbnails;
    std::vector<FileBrowserFileOperations::JobId> m_activeFileOps_; // polled in the status bar
```

- [ ] **Step 2: cpp — `Setup`/`ShutdownThumbnails`/`ClearThumbnailCache`/`RebuildThumbnail`**

In `file_browser_ui.cpp`, near the existing thumbnail setters (around line 984), add:

```cpp
void ImGui::FileBrowser::Setup(vulkan_context* vk, std::filesystem::path thumb_dir) {
    m_thumbnails.setup(vk, std::move(thumb_dir));
}
void ImGui::FileBrowser::ShutdownThumbnails() { m_thumbnails.shutdown(); }
void ImGui::FileBrowser::ClearThumbnailCache() { m_thumbnails.clear(); }
void ImGui::FileBrowser::RebuildThumbnail(const std::filesystem::path& path) { m_thumbnails.evict(path); }
```

- [ ] **Step 3: cpp — tick the thumbnail engine each frame**

In `Display()`, right after `PollScan();` (line 100), add:

```cpp
    m_thumbnails.new_frame();
```

- [ ] **Step 4: cpp — swap render paths to the internal engine**

Replace the thumbnail-provider checks with the engine. There are three sites (verified line numbers ~409, ~438/469, ~508, ~625):

- The toolbar "Thumbnails" checkbox guard `if (thumbnailProvider_) {` (line 409) → `if (m_thumbnails.is_setup()) {`
- The shift-wheel scale guard `thumbnailProvider_ && showThumbnails_ && ...` (line 438) → `m_thumbnails.is_setup() && showThumbnails_ && ...`
- `const bool useGridView = (viewMode_ == ViewMode::Grid) && thumbnailProvider_ && showThumbnails_;` (line 469) → replace `thumbnailProvider_` with `m_thumbnails.is_setup()`
- Grid thumbnail fetch (line 508) `const ImTextureID thumb = thumbnailProvider_(currentDirectory_ / rsc.name);` → `const ImTextureID thumb = m_thumbnails.get(currentDirectory_ / rsc.name);`
- Inline thumbnail guard + fetch (lines 625-626): `if (thumbnailProvider_ && showThumbnails_ && !rsc.isDir) {` → `if (m_thumbnails.is_setup() && showThumbnails_ && !rsc.isDir) {`; `const ImTextureID thumb = thumbnailProvider_(currentDirectory_ / rsc.name);` → `const ImTextureID thumb = m_thumbnails.get(currentDirectory_ / rsc.name);`

- [ ] **Step 5: cpp — route new-dir creation through file-ops**

At the new-dir popup (line 344), replace:
```cpp
		if (create_directory(currentDirectory_ / u8StrToPath(newDirNameBuffer_.data()))) {
```
with:
```cpp
		if (m_file_ops.create_directory(currentDirectory_ / u8StrToPath(newDirNameBuffer_.data())).ok) {
```

- [ ] **Step 6: cpp — show file-ops progress in the status bar**

In `Display()`, just before the existing status-bar block (line 794), add a poll that folds active job progress into `statusStr_`:

```cpp
    // Surface any in-flight async file operation in the status bar.
    for (auto it = m_activeFileOps_.begin(); it != m_activeFileOps_.end();) {
        FileBrowserFileOperations::Progress pr;
        if (!m_file_ops.poll(*it, pr)) { it = m_activeFileOps_.erase(it); continue; }
        using St = FileBrowserFileOperations::Progress::State;
        if (pr.state == St::Running) {
            statusStr_ = "Working " + std::to_string(pr.done) + "/" + std::to_string(pr.total);
            ++it;
        } else {
            statusStr_ = (pr.state == St::Failed) ? ("error: " + pr.error) : std::string("Done");
            RequestReload(); // refresh the listing after the op finishes
            it = m_activeFileOps_.erase(it);
        }
    }
```

Engineer note: callers that submit a heavy op (e.g. the context menu in Task 7) push the returned `JobId` into `m_activeFileOps_` via a small helper. Add this public method to the header (Step 1 block) and cpp:
```cpp
    void TrackFileOp(FileBrowserFileOperations::JobId id) { m_activeFileOps_.push_back(id); }
```

- [ ] **Step 7: Build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0. `thumbnailProvider_`/`SetThumbnailProvider` are now unused inside the browser but still declared/defined (so `app_coordinator` compiles). Thumbnails render only once `Setup()` is wired (Task 6); until then `is_setup()` is false and the list renders without thumbnails.

- [ ] **Step 8: Commit**

```bash
git add code/ui/FileExplorer/file_browser_ui.hpp code/ui/FileExplorer/file_browser_ui.cpp
git commit -m "feat(filebrowser): own thumbnail engine + file-ops; render via internal context

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Rewire `app_coordinator`, retire the old cache

Point the coordinator at `FileBrowser::Setup`, route clear/rebuild/shutdown to the browser, remove the dead provider API, drop `AppContext::m_thumb_cache`, and delete `FileThumbnailCache`.

**Files:**
- Modify: `code/ui/window/widgets/app_coordinator.cpp` (lines 116, 408-409, 507-545)
- Modify: `code/ui/window/widgets/app_coordinator.hpp:25,125`
- Modify: `code/core/app_context.hpp:43,78,108`
- Modify: `code/core/app_context.cpp:16,42,63`
- Modify: `code/ui/FileExplorer/file_browser_ui.hpp` / `.cpp` (remove `thumbnailProvider_` + `SetThumbnailProvider`)
- Delete: `code/ui/FileExplorer/file_thumbnail_cache.hpp` / `.cpp`
- Modify: `CMakeLists.txt:188` (remove `file_thumbnail_cache.cpp`)

- [ ] **Step 1: `app_coordinator.cpp` — replace the setup block**

Replace the body of `SetThumbDir` thumbnail block (lines 513-545, from `if (m_vk && m_thumb_cache) {` through the `SetRebuildThumbnailCallback(...)` closing, **keeping** the `SetExtraItemsCallback` that follows but editing it) with:

```cpp
  if (m_vk) {
    GetMainFileExplorer().Setup(m_vk, dir);

    m_config_runtime->SetClearFileExplorerCacheCallback(
        []() { GetMainFileExplorer().ClearThumbnailCache(); });

    GetMainFileExplorer().SetRebuildThumbnailCallback(
        [](const std::filesystem::path &path) { GetMainFileExplorer().RebuildThumbnail(path); });

    m_fb_context_menu->SetExtraItemsCallback(
        [this](const std::filesystem::path &path) {
          if (ImGui::MenuItem("Rebuild Thumbnail"))
            GetMainFileExplorer().RebuildThumbnail(path);
          // ... keep the existing "Edit Tags…" block below unchanged ...
```

(Leave the rest of the `SetExtraItemsCallback` lambda — the "Edit Tags…" handling — exactly as-is.)

- [ ] **Step 2: `app_coordinator.cpp` — shutdown + member init**

- Line 116 (`m_thumb_cache = m_ctx->ThumbCache();`): delete it.
- Lines 408-409 (`if (m_thumb_cache) m_thumb_cache->shutdown();`): replace with:
```cpp
  GetMainFileExplorer().ShutdownThumbnails();
```

- [ ] **Step 3: `app_coordinator.hpp` — drop the field**

- Line 25 (`class FileThumbnailCache;`): delete.
- Line 125 (`FileThumbnailCache *m_thumb_cache = nullptr;`): delete.

- [ ] **Step 4: `app_context.{hpp,cpp}` — drop the cache**

- `app_context.hpp`: delete line 43 (`class FileThumbnailCache;`), line 78 (`ThumbCache()` decl), line 108 (`m_thumb_cache` member).
- `app_context.cpp`: delete line 16 (`#include "file_thumbnail_cache.hpp"`), line 42 (`, m_thumb_cache {std::make_unique<FileThumbnailCache>()}`), line 63 (`ThumbCache()` definition).

- [ ] **Step 5: Remove the dead provider API from `FileBrowser`**

- `file_browser_ui.hpp`: delete the `SetThumbnailProvider` declaration (line 145) and the `thumbnailProvider_` member (line 286).
- `file_browser_ui.cpp`: delete the `SetThumbnailProvider` definition (lines 984-986).

- [ ] **Step 6: Delete `FileThumbnailCache` + CMake entry**

```bash
git rm code/ui/FileExplorer/file_thumbnail_cache.hpp code/ui/FileExplorer/file_thumbnail_cache.cpp
```
In `CMakeLists.txt`, delete the `code/ui/FileExplorer/file_thumbnail_cache.cpp` line (was line 188).

- [ ] **Step 7: Build**

Run: `cmake --preset all && cmake --build --preset build-debug`
Expected: exit code 0, no references to `FileThumbnailCache`/`m_thumb_cache`/`thumbnailProvider_` remain.

- [ ] **Step 8: Manual smoke**

Run the Debug binary, open the file explorer, browse a folder of images + videos:
- Thumbnails appear progressively; **scrolling a large grid stays smooth** (no per-frame hitch).
- Toggle the "Thumbnails" checkbox; switch List/Grid.
- "Rebuild Thumbnail" + "Clear cache" regenerate thumbnails.
- Console shows no `ThreadOverwatch` timeout/kill spam during normal browsing.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor(filebrowser): retire AppContext FileThumbnailCache; browser owns engine

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Context-menu async delete

Route the confirmation-modal delete through the browser's `FileBrowserFileOperations` so deleting a folder never freezes the UI.

**Files:**
- Modify: `code/ui/FileExplorer/file_browser_context_menu.hpp`
- Modify: `code/ui/FileExplorer/file_browser_context_menu.cpp:110-117`
- Modify: `code/ui/window/widgets/app_coordinator.cpp` (inject the file-ops pointer)

- [ ] **Step 1: Header — accept a file-ops pointer + a submit callback**

In `file_browser_context_menu.hpp`, add a forward decl `class FileBrowserFileOperations;` and:
```cpp
    // Inject the async file-ops service used by Delete. Call once after setup().
    void set_file_ops(FileBrowserFileOperations* ops) { m_file_ops = ops; }
    // Optional: invoked with the JobId after a delete is submitted (for status tracking).
    void SetDeleteSubmittedCallback(std::function<void(std::uint64_t)> cb) { m_on_delete = std::move(cb); }
```
and the members:
```cpp
    FileBrowserFileOperations*          m_file_ops = nullptr;
    std::function<void(std::uint64_t)>  m_on_delete;
```

- [ ] **Step 2: cpp — submit instead of inline remove**

In `file_browser_context_menu.cpp`, add `#include "file_browser_file_operations.hpp"` at the top, then replace the Delete button body (lines 110-117):
```cpp
    if (ImGui::Button("Delete", {120, 0})) {
      if (m_file_ops) {
        const std::uint64_t id = m_file_ops->submit_remove(m_pending_delete_path);
        if (m_on_delete)
          m_on_delete(id);
      }
      m_pending_delete_path.clear();
      ImGui::CloseCurrentPopup();
    }
```

- [ ] **Step 3: app_coordinator — inject the pointer + track the job**

Where `m_fb_context_menu` is set up (near the `SetExtraItemsCallback` block in `SetThumbDir`), add:
```cpp
    m_fb_context_menu->set_file_ops(&GetMainFileExplorer().FileOps());
    m_fb_context_menu->SetDeleteSubmittedCallback(
        [](std::uint64_t id) { GetMainFileExplorer().TrackFileOp(id); });
```

- [ ] **Step 4: Build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0.

- [ ] **Step 5: Manual smoke**

- Right-click a file → Delete → confirm: file disappears, status bar shows `Working …/…` then `Done`, listing refreshes, UI never freezes.
- Delete a large folder (if your menu allows directory delete): progress counts up; the app stays responsive throughout.

- [ ] **Step 6: Commit**

```bash
git add code/ui/FileExplorer/file_browser_context_menu.hpp code/ui/FileExplorer/file_browser_context_menu.cpp code/ui/window/widgets/app_coordinator.cpp
git commit -m "feat(filebrowser): context-menu Delete runs async via FileBrowserFileOperations

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 8: Final verification

- [ ] **Step 1: Clean Debug build**

Run: `cmake --build --preset build-debug`
Expected: exit code 0, zero warnings about unused `FileThumbnailCache`/`thumbnailProvider_`.

- [ ] **Step 2: Release build sanity (per LLVM-toolchain memory, all configs build clean)**

Run: `cmake --build --preset build-release`
Expected: exit code 0.

- [ ] **Step 3: Full smoke checklist**

Launch the Debug binary and verify:
- [ ] Browse a directory with 50+ images/videos: thumbnails fill in progressively, GPU memory stable.
- [ ] Scroll the grid fast: no frame hitching (upload budget working).
- [ ] Switch directories rapidly: stale thumbnails don't block the new directory (`clear_pending` working).
- [ ] Rebuild Thumbnail / Clear cache work; no flicker or stall on evict (deferred-free working).
- [ ] Delete a folder: progress shown, no UI freeze.
- [ ] Watch the console for `ThreadOverwatch` messages — none under normal use; if you point a video thumbnail at a pathological file, you should see at most a single KillOnly notice and the app recovers.
- [ ] Quit cleanly: no Vulkan validation errors about textures destroyed after device (confirms `ShutdownThumbnails()` ordering).

- [ ] **Step 4: Update todo.txt**

Mark the FileExplorer refactor line (todo.txt:37-38) done.

```bash
git add todo.txt
git commit -m "docs: mark filebrowser async-thumbnail refactor done

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Spec coverage check

| Spec section | Task(s) |
| --- | --- |
| §4 FileBrowserFileOperations (sync + async worker, progress, overwatch) | 2, 5 (status bar), 7 (delete) |
| §5 FileBrowserThumbnailContext (cheap key, upload budget, in-memory handoff, deferred evict) | 1 (load_from_rgba), 4 |
| §6 FileBrowserThumbnailThread (persistent pool, relocated decode, overwatch, priority/clear) | 3 |
| §7 FileBrowser integration (Setup, get swap, new-dir, status bar, remove provider) | 5, 6 |
| §8 Retire old cache (app_coordinator, app_context, delete files, CMake) | 6 |
| §10 ThreadOverwatch wiring (both new workers) | 2, 3 |
| Shutdown ordering (GPU textures freed before Vulkan teardown) | 5 (ShutdownThumbnails), 6 (wire at line 408) |
```
