# FileBrowser Decomposition: Async Thumbnails + File-Operations Service

**Date:** 2026-06-03
**Status:** Approved (pending spec review)
**Scope:** Refactor `ImGui::FileBrowser` into focused sub-components, move thumbnail
generation into a browser-owned async engine wired to `ThreadOverwatch`, and extract
all filesystem mutations into a dedicated file-operations service with an async worker.

---

## 1. Goal

Split the 1328-line `ImGui::FileBrowser` "god class" into three focused units it owns,
eliminate the per-frame render-thread stalls that make thumbnails lag/freeze the app,
and route every long-running background thread through the existing `ThreadOverwatch`
watchdog.

The three units (file stems match the names requested in `todo.txt`; classes are
PascalCased to match the existing `FileBrowserScanner` / `FileThumbnailCache` /
`FileBrowserContextMenu` convention):

| Requested name (todo.txt)              | Class                          | Files                                              |
| -------------------------------------- | ------------------------------ | -------------------------------------------------- |
| `file_browser_fileOperations_context`  | `FileBrowserFileOperations`    | `file_browser_file_operations.{hpp,cpp}`           |
| `file_browser_thumbnail_context`       | `FileBrowserThumbnailContext`  | `file_browser_thumbnail_context.{hpp,cpp}`         |
| `file_browser_thumbnail_context_thread`| `FileBrowserThumbnailThread`   | `file_browser_thumbnail_context_thread.{hpp,cpp}`  |

---

## 2. Starting point (current architecture)

- `ImGui::FileBrowser` (`code/ui/FileExplorer/file_browser_ui.{hpp,cpp}`) does
  everything: window/popup management, navigation, sorting, filtering, list + grid
  rendering, thumbnail *display*, and the inline `create_directory` op.
- Thumbnails reach the browser through a `thumbnailProvider_` callback
  (`std::function<ImTextureID(const std::filesystem::path&)>`), set in
  `app_coordinator.cpp:523`. The browser itself has **no Vulkan dependency**.
- `FileThumbnailCache` (`code/ui/FileExplorer/file_thumbnail_cache.{hpp,cpp}`) is the
  real async engine: image pool (4 jthreads, stb_image/webp) + video pool (2 jthreads,
  libmpv SW render). Owned by `AppContext::m_thumb_cache`, shared via the provider
  callback. Used **only** by the file-explorer wiring (verified by grep).
- `FileBrowserScanner` (`code/ui/FileExplorer/file_browser_thread.{hpp,cpp}`) is the
  reference pattern to copy: a persistent worker, per-job `ThreadOverwatch::watch(...,
  KillOnly)`, heartbeat per directory entry, latest-wins generation counter.
- `FileBrowserContextMenu` (`code/ui/FileExplorer/file_browser_context_menu.{hpp,cpp}`)
  owns the right-click Delete (with a confirmation modal), Copy Path, Open Folder.
- CMake lists sources **explicitly** (no glob) at `CMakeLists.txt:185-188`.

### Why it lags today (the freeze sources — all on the render thread)

1. `FileThumbnailCache::get()` calls `std::filesystem::weakly_canonical(path)` — a real
   disk syscall — **every frame, for every visible file**, under a mutex.
2. `get()` uploads the GPU texture (`VulkanTexture::load`) inline on the render thread,
   re-reading and re-decoding the PNG it just wrote.
3. `evict()` calls `vkDeviceWaitIdle()` — a full GPU pipeline stall.

Generation is already async, so the heavy decode is *not* the freeze — the synchronous
render-thread work is.

---

## 3. Target architecture

```
ImGui::FileBrowser
├── FileBrowserFileOperations    m_file_ops;    // no ImGui, no Vulkan
│     └── owns ── serial ops worker thread (ThreadOverwatch-wired)
└── FileBrowserThumbnailContext  m_thumbnails;  // render-thread front-end, owns Vulkan upload
      └── owns ── FileBrowserThumbnailThread    // persistent decode pool (ThreadOverwatch-wired)
```

Dependency direction is strictly one-way: **UI → context → thread**, never backwards.

- `FileBrowserFileOperations` includes neither ImGui nor Vulkan (pure `std::filesystem`
  + `<thread>`), so it compiles fast and is unit-testable in isolation.
- `FileBrowserThumbnailContext` is the **only** unit that touches Vulkan.
- `FileBrowserThumbnailThread` touches neither ImGui nor Vulkan — generator threads must
  never call into the device. This mirrors the existing rule that `FileRecord` is "pure
  data, no ImGui dependency."

---

## 4. Component: `FileBrowserFileOperations`

Pure filesystem mutation service. Fast metadata ops run synchronously; heavy/unbounded
ops are queued on a single persistent worker thread.

```cpp
class FileBrowserFileOperations {
public:
    FileBrowserFileOperations();
    ~FileBrowserFileOperations();                 // joins worker
    FileBrowserFileOperations(const FileBrowserFileOperations&) = delete;
    FileBrowserFileOperations& operator=(const FileBrowserFileOperations&) = delete;

    struct Result { bool ok = true; std::string error; };   // error empty on success

    // Fast metadata ops — synchronous, instant feedback (new-dir popup, rename).
    Result create_directory(const std::filesystem::path& dir);
    Result rename(const std::filesystem::path& from, const std::filesystem::path& to);

    // Heavy ops — queued on the worker, never block the UI. Returns a job id.
    using JobId = std::uint64_t;
    JobId submit_remove(const std::filesystem::path& target);                       // recursive
    JobId submit_copy  (const std::filesystem::path& from, const std::filesystem::path& to);
    JobId submit_move  (const std::filesystem::path& from, const std::filesystem::path& to);

    struct Progress {
        enum class State { Running, Done, Failed };
        State                 state = State::Running;
        std::uint64_t         done  = 0;
        std::uint64_t         total = 0;
        std::filesystem::path current;
        std::string           error;             // populated on Failed
    };
    // Non-blocking, render thread. Returns false if `id` is unknown.
    bool poll(JobId id, Progress& out);

    void shutdown();                              // stop + join worker

private:
    struct Job { JobId id; /* op kind, from, to, abort flag */ };
    void worker_loop(std::stop_token st);

    std::mutex                          m_mutex;
    std::condition_variable_any         m_cv;
    std::deque<Job>                     m_queue;
    std::unordered_map<JobId, Progress> m_progress;   // polled by UI
    std::atomic<JobId>                  m_next_id{1};
    std::atomic<bool>                   m_kill_requested{false};  // set by overwatch
    std::jthread                        m_worker;
};
```

### Behaviour

- **Single serial worker** (one queue). Concurrent copies on the same disk only thrash
  the head/controller; serial is both simpler and faster in the common case ("fastest
  and efficient" per the design decision).
- **Two-phase heavy ops**: phase 1 counts entries (sets `total`), phase 2 performs the
  op entry-by-entry updating `done` / `current`. Uses `std::error_code` overloads
  exclusively — never throws across the UI boundary. On failure the job goes to
  `Failed` with `error` set.
- **Cross-device `copy`/`move`**: `std::filesystem::rename` fails across mount points;
  `move` detects that and falls back to copy-then-remove on the worker so it can't
  silently block.
- **ThreadOverwatch wiring** (per the `FileBrowserScanner` template):
  - On dequeuing a job, `watch("FileBrowserFileOps::<op>", 10s, kill=set
    m_kill_requested, restart=nullptr, KillOnly)`; `unwatch()` on completion.
  - `heartbeat(watch_id)` once per processed entry.
  - The per-entry loop checks `m_kill_requested` and `st.stop_requested()` and aborts
    cleanly (→ `Failed`, error "operation aborted") so a wedged op can't hang forever.
  - An idle worker (blocked on the queue) holds **no** watch — idle waiting never trips
    the watchdog (same as the scanner).

### Callers

- New-dir popup (`file_browser_ui.cpp:344`): `create_directory(...)` (synchronous).
- `FileBrowserContextMenu` Delete: becomes `submit_remove(...)`; the menu's confirmation
  modal stays, but on confirm it submits instead of deleting inline.
- The browser status bar polls active job ids and shows e.g. `Copying… 3/120` and
  surfaces `error` on failure.

---

## 5. Component: `FileBrowserThumbnailContext`

Render-thread-facing cache + GPU upload front-end. Owns the entry map and the
`FileBrowserThumbnailThread`. This is where the anti-lag fixes live.

```cpp
class FileBrowserThumbnailContext {
public:
    FileBrowserThumbnailContext();
    ~FileBrowserThumbnailContext();
    FileBrowserThumbnailContext(const FileBrowserThumbnailContext&) = delete;
    FileBrowserThumbnailContext& operator=(const FileBrowserThumbnailContext&) = delete;

    // Render thread, before any get(). Attaches Vulkan + on-disk PNG cache dir.
    void setup(vulkan_context* vk, std::filesystem::path thumb_dir);
    void shutdown();                                  // render thread
    [[nodiscard]] bool is_setup() const noexcept;     // UI guard before get()

    // Render thread, top of Display(): resets the per-frame upload budget and
    // ticks the deferred-free retire queue.
    void new_frame();

    // Render thread, non-blocking. Returns 0 while generating/uploading.
    [[nodiscard]] ImTextureID get(const std::filesystem::path& path);

    void evict(const std::filesystem::path& path);    // deferred free — NO vkDeviceWaitIdle
    void clear();                                      // stop pool, delete PNGs, flush

    // Moved out of app_coordinator's `is_thumb_path` lambda.
    [[nodiscard]] static bool is_thumbnailable(const std::filesystem::path& p);

private:
    enum class State { Queued, Generating, PixelsReady, DiskReady, Ready, Failed };
    struct Entry {
        State                          state = State::Queued;
        std::filesystem::path          png_path;
        std::vector<std::uint8_t>      pixels;        // RGBA handed back by the worker
        std::unique_ptr<VulkanTexture> texture;
    };

    std::string key_for(const std::filesystem::path& path) const;  // cheap, no syscall

    std::mutex                             m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<std::unique_ptr<VulkanTexture>> m_retire;          // deferred-free queue
    int                                    m_uploads_this_frame = 0;
    vulkan_context*                        m_vk = nullptr;
    std::filesystem::path                  m_thumb_dir;
    bool                                   m_setup = false;
    FileBrowserThumbnailThread             m_thread;                // owned worker pool
};
```

### Anti-lag changes vs. the old `FileThumbnailCache`

1. **Cheap key** (`key_for`): paths come from a directory listing and are already
   absolute (`currentDirectory_ / rsc.name`), so drop the per-frame
   `weakly_canonical()` syscall and key on the normalized path string instead.
2. **Upload budget**: cap GPU uploads to `k_max_uploads_per_frame` (4) so scrolling a
   freshly-listed grid can't stall a single frame; `m_uploads_this_frame` resets at the
   top of each `Display()` via a small `begin_frame()` hook (or a frame counter).
3. **In-memory RGBA handoff**: when the worker finishes it hands back the decoded RGBA
   buffer (`Entry::pixels`, state `PixelsReady`) which `get()` uploads directly —
   skipping the PNG encode→re-read→decode round-trip on the hot path. The worker still
   writes the PNG for cross-session persistence; on a later session the entry starts at
   `DiskReady` and uploads from disk.
4. **Deferred eviction**: `evict()`/replaced textures move to `m_retire` and are freed
   after `k_retire_frames` frames (enough for the GPU to finish sampling) instead of
   calling `vkDeviceWaitIdle()`.

### State machine

`Queued` → (worker picks up) `Generating` → `PixelsReady` (fresh) **or** `DiskReady`
(PNG already existed) → (upload, budget permitting) `Ready` → return `texture->imgui_id()`.
Decode failure → `Failed` (returns 0, no placeholder texture).

### Constants (carried over / new)

- `k_thumb_w = 320`, `k_thumb_h = 180` (unchanged from `FileThumbnailCache`).
- `k_max_uploads_per_frame = 4` (new).
- `k_retire_frames = 3` (new).

---

## 6. Component: `FileBrowserThumbnailThread`

Persistent decode pool. Reuses the existing decode code (stb_image / WebP / libmpv SW
render) verbatim, just relocated. No Vulkan, no ImGui.

```cpp
class FileBrowserThumbnailThread {
public:
    // Callback invoked on the worker thread when a thumbnail's RGBA is ready.
    // The context locks its map and stores the pixels. `ok=false` → Failed.
    using DoneFn = std::function<void(const std::string& key,
                                      std::vector<std::uint8_t> rgba, bool ok)>;

    FileBrowserThumbnailThread();
    ~FileBrowserThumbnailThread();                    // stop + join all workers
    FileBrowserThumbnailThread(const FileBrowserThumbnailThread&) = delete;
    FileBrowserThumbnailThread& operator=(const FileBrowserThumbnailThread&) = delete;

    void start(DoneFn on_done);                       // spawn the pool
    // Enqueue a generation job. `is_image` selects the image vs. video sub-pool budget.
    void submit(std::string key, std::filesystem::path file,
                std::filesystem::path out_png, bool is_image);
    void clear_pending();                             // drop queued jobs (e.g. dir change)
    void shutdown();

private:
    struct Job { std::string key; std::filesystem::path file, out_png; bool is_image; };
    void worker_loop(std::stop_token st);

    std::mutex                  m_mutex;
    std::condition_variable_any m_cv;
    std::deque<Job>             m_image_queue;         // visible-first priority
    std::deque<Job>             m_video_queue;
    std::atomic<bool>           m_kill_requested{false};
    DoneFn                      m_on_done;
    std::vector<std::jthread>   m_workers;             // 4 image + 2 video (see below)

public:
    // Carried over from the retired FileThumbnailCache's proven concurrency.
    static constexpr int k_image_workers = 4;
    static constexpr int k_video_workers = 2;
};
```

### Behaviour

- **Persistent pool** (no thread-per-thumbnail churn): `k_image_workers` image workers
  pull from `m_image_queue`, `k_video_workers` video workers pull from `m_video_queue`.
  Workers block on `m_cv` when idle (near-zero cost — no busy-wait), so holding 6
  always-alive threads is cheaper than the old spawn-per-thumbnail churn.
- **Priority**: jobs for the currently visible directory are pushed front; off-screen
  prefetch (if any) goes to the back. `clear_pending()` drops queued (not in-flight)
  jobs when the user navigates away, so a new directory's thumbnails aren't stuck behind
  a stale backlog.
- **Decode**: relocate `generate_image_thumbnail` (stb_image + `stbir_resize` / WebP)
  and the full libmpv SW-render path from `file_thumbnail_cache.cpp` unchanged, but have
  them return the RGBA buffer (for the in-memory handoff) **and** write the PNG.
- **ThreadOverwatch wiring** (per the `FileBrowserScanner` template):
  - Each worker registers `watch("FileBrowserThumb::<img|vid>", 8s, kill=set
    m_kill_requested, restart=nullptr, KillOnly)` for the duration of a single job and
    `unwatch()`es when the job finishes. Idle workers hold no watch.
  - `heartbeat(watch_id)` inside the mpv wait-loops and after each decode phase.
  - `m_kill_requested` / `st.stop_requested()` checked in the mpv loops (which already
    have 8s + 5s deadlines) so a hung video decode is abandoned without killing the
    worker permanently.

---

## 7. FileBrowser integration

### `file_browser_ui.hpp`

- Add includes/forward decls: forward-declare `vulkan_context`; include the three new
  headers (or forward-declare + hold by value — value members, so include).
- Add public:
  ```cpp
  // Attach Vulkan + thumbnail cache dir. Call once after construction.
  void Setup(vulkan_context* vk, std::filesystem::path thumb_dir);
  void ClearThumbnailCache();                         // -> m_thumbnails.clear()
  void RebuildThumbnail(const std::filesystem::path&);// -> m_thumbnails.evict()
  ```
- **Remove**: `thumbnailProvider_` member and `SetThumbnailProvider(...)`.
  (`SetRebuildThumbnailCallback` / `SetHoverFileCallback` / `SetContextMenuCallback`
  stay — they are unrelated UI hooks.)
- Add members: `FileBrowserFileOperations m_file_ops;`
  `FileBrowserThumbnailContext m_thumbnails;`

### `file_browser_ui.cpp`

- Render paths at lines ~508 and ~626 call `m_thumbnails.get(currentDirectory_ / rsc.name)`
  instead of `thumbnailProvider_(...)`. The `thumbnailProvider_ &&` guards become
  `m_thumbnails.is_setup() && showThumbnails_`. Media-type gating uses
  `FileBrowserThumbnailContext::is_thumbnailable(...)`.
- New-dir popup uses `m_file_ops.create_directory(...)`.
- Status bar polls `m_file_ops` for active job progress and renders it.
- `Display()` calls `m_thumbnails.new_frame()` once at the top (resets the per-frame
  upload budget, ticks the retire queue) — right next to the existing `PollScan()` call.

---

## 8. Wiring changes (retire the old shared cache)

### `app_coordinator.cpp`
- Replace the `SetThumbnailProvider` lambda + `is_thumb_path` static + the
  `SetClearFileExplorerCacheCallback` / `SetRebuildThumbnailCallback` block
  (~lines 519-540) with:
  ```cpp
  GetMainFileExplorer().Setup(m_vk, dir);
  m_config_runtime->SetClearFileExplorerCacheCallback(
      [] { GetMainFileExplorer().ClearThumbnailCache(); });
  GetMainFileExplorer().SetRebuildThumbnailCallback(
      [](const std::filesystem::path& p) { GetMainFileExplorer().RebuildThumbnail(p); });
  ```
- Remove the `app_coordinator.hpp` `FileThumbnailCache* m_thumb_cache` field and its
  forward declaration.

### `app_context.{hpp,cpp}`
- Remove `m_thumb_cache` member, the `ThumbCache()` accessor, the
  `std::make_unique<FileThumbnailCache>()` init, and the `FileThumbnailCache` forward
  declaration / include.

### Delete
- `code/ui/FileExplorer/file_thumbnail_cache.{hpp,cpp}` — its decode code is relocated
  into `FileBrowserThumbnailThread`; nothing else references it.

### `CMakeLists.txt`
- Remove line 188 (`file_thumbnail_cache.cpp`).
- Add:
  ```
  code/ui/FileExplorer/file_browser_file_operations.cpp
  code/ui/FileExplorer/file_browser_thumbnail_context.cpp
  code/ui/FileExplorer/file_browser_thumbnail_context_thread.cpp
  ```

---

## 9. Files summary

**Create**
- `code/ui/FileExplorer/file_browser_file_operations.{hpp,cpp}`
- `code/ui/FileExplorer/file_browser_thumbnail_context.{hpp,cpp}`
- `code/ui/FileExplorer/file_browser_thumbnail_context_thread.{hpp,cpp}`

**Modify**
- `code/ui/FileExplorer/file_browser_ui.{hpp,cpp}`
- `code/ui/FileExplorer/file_browser_context_menu.cpp` (Delete → `submit_remove`)
- `code/ui/window/widgets/app_coordinator.{hpp,cpp}`
- `code/core/app_context.{hpp,cpp}`
- `CMakeLists.txt`

**Delete**
- `code/ui/FileExplorer/file_thumbnail_cache.{hpp,cpp}`

---

## 10. Threading & ThreadOverwatch summary

| Thread                        | Model                 | Watch name                     | Policy   | Heartbeat            |
| ----------------------------- | --------------------- | ------------------------------ | -------- | -------------------- |
| `FileBrowserScanner` (exists) | 1 persistent          | `FileBrowserScanner::scan`     | KillOnly | per dir entry        |
| Thumbnail image pool (new)    | 4 persistent          | `FileBrowserThumb::img`        | KillOnly | per decode phase     |
| Thumbnail video pool (new)    | 2 persistent          | `FileBrowserThumb::vid`        | KillOnly | inside mpv wait-loop |
| File-ops worker (new)         | 1 persistent (serial) | `FileBrowserFileOps::<op>`     | KillOnly | per processed entry  |

All new watches: registered only while a job is in flight, `KillOnly` (set an abort
flag + unwatch; the worker survives to serve the next job), idle workers unwatched.

---

## 11. Constraints / conventions

- C++23, clang/clang++ + LLD (per project memory). `static_cast`/`std::bit_cast` only,
  smart pointers, `std::span`/`string_view`, `constexpr`, attached braces, `std::println`
  for logs.
- `setup()` / `shutdown()` / `get()` on the thumbnail context are render-thread-only
  (they touch Vulkan), exactly like the retired `FileThumbnailCache`.
- Generator/ops worker threads never call Vulkan or ImGui.

---

## 12. Out of scope (explicit YAGNI)

- Parallel file-ops (serial worker only).
- Off-screen thumbnail prefetch beyond the visible directory (priority hook exists, no
  speculative scanning).
- A progress UI richer than the status-bar line + error surfacing (no modal progress
  dialog, no cancel button) — can be a follow-up now that `poll()`/`JobId` exist.
- Migrating non-file-explorer thumbnail users — there are none.
