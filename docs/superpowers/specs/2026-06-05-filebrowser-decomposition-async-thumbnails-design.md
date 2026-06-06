# FileBrowser Decomposition: Async Thumbnails + File-Operations Service

- **Date:** 2026-06-05 (design approved 2026-06-03; original spec doc was lost when a
  session hit its limit — this is the reconstructed + updated version)
- **Status:** Approved design, **not yet implemented** (next phase after the engine)
- **Depends on:** the parallel image engine
  (`2026-06-05-parallel-image-library-design.md`), now implemented in
  `code/core/image` (`ImageJobSystem` + `JobQueue` + `image_ops`).

> **Update vs. the original design:** the original had the thumbnail worker own a
> private thread pool. Now that `ImageJobSystem` exists (the user chose
> "engine first, then file explorer uses it"), **image thumbnail generation
> submits to `ImageJobSystem`** instead of a second pool. Only the libmpv *video*
> thumbnail path keeps a dedicated worker (it is not image-codec work).

## 1. Goal

Split the 1328-line `ImGui::FileBrowser` into three focused units it owns, kill the
per-frame render-thread stalls that make thumbnails freeze/lag the app, and route
all background work through `ThreadOverwatch` (image work via `ImageJobSystem`'s
already-watched workers; the video worker registers itself).

| Requested name (todo.txt)               | Class                          | Files                                              |
| --------------------------------------- | ------------------------------ | -------------------------------------------------- |
| `file_browser_fileOperations_context`   | `FileBrowserFileOperations`    | `file_browser_file_operations.{hpp,cpp}`           |
| `file_browser_thumbnail_context`        | `FileBrowserThumbnailContext`  | `file_browser_thumbnail_context.{hpp,cpp}`         |
| `file_browser_thumbnail_context_thread` | `FileBrowserThumbnailThread`   | `file_browser_thumbnail_context_thread.{hpp,cpp}`  |

## 2. Why it lags today (all on the render thread)

1. `FileThumbnailCache::get()` calls `std::filesystem::weakly_canonical(path)` — a
   real disk syscall — every frame, for every visible file, under a mutex.
2. `get()` uploads the GPU texture (`VulkanTexture::load`) inline, re-reading and
   re-decoding the PNG it just wrote.
3. `evict()` calls `vkDeviceWaitIdle()` — a full GPU pipeline stall.

Generation is already async, so the fix is relocating/splitting the engine, killing
the synchronous render-thread work, and adding overwatch wiring.

## 3. Target architecture

```
ImGui::FileBrowser
├── FileBrowserFileOperations    m_file_ops;    // no ImGui, no Vulkan
│     └── serial ops worker (ManagedThread, KillOnly, heartbeat per entry)
└── FileBrowserThumbnailContext  m_thumbnails;  // render-thread front-end, owns Vulkan upload
      └── FileBrowserThumbnailThread            // image jobs -> ImageJobSystem; video -> own worker
```

Dependency direction is one-way: UI -> context -> thread/engine. `FileBrowserThumbnailContext`
is the only unit touching Vulkan; the thread/engine produce CPU `img::ImageBuffer` only.

## 4. FileBrowserFileOperations

Pure `std::filesystem` service. Fast metadata ops are synchronous; heavy ops run on a
single serial worker (concurrent copies on one disk only thrash — serial is faster).

```cpp
struct Result { bool ok = true; std::string error; };
Result create_directory(const std::filesystem::path&);
Result rename(const std::filesystem::path& from, const std::filesystem::path& to);

using JobId = std::uint64_t;
JobId submit_remove(const std::filesystem::path& target);                       // recursive
JobId submit_copy  (const std::filesystem::path& from, const std::filesystem::path& to);
JobId submit_move  (const std::filesystem::path& from, const std::filesystem::path& to);

struct Progress { enum class State { Running, Done, Failed } state; std::uint64_t done, total;
                  std::filesystem::path current; std::string error; };
bool poll(JobId, Progress& out);   // non-blocking, render thread
```

- Single `ManagedThread` worker (KillOnly, generous timeout), `watch(...)` per job,
  `heartbeat` per processed entry, checks an abort flag + `stop_token` between entries.
- `std::error_code` overloads only — never throws across the UI boundary.
- `move` falls back to copy-then-remove across mount points.
- Callers: new-dir popup -> `create_directory`; `FileBrowserContextMenu` delete ->
  `submit_remove`; status bar polls progress ("Copying 3/120").

## 5. FileBrowserThumbnailContext (render-thread front-end)

Owns the entry map + GPU upload. This is where the anti-lag fixes live.

```cpp
void setup(vulkan_context* vk, std::filesystem::path thumb_dir);
ImTextureID get(const std::filesystem::path&);   // non-blocking; 0 while pending
void evict(const std::filesystem::path&);          // deferred free, NO vkDeviceWaitIdle
void clear();
static bool is_thumbnailable(const std::filesystem::path&);  // moved out of app_coordinator
void begin_frame();                                // resets per-frame upload budget, ticks retire queue
```

Anti-lag changes vs. the old cache:
1. **Cheap key** — listing paths are already absolute (`currentDirectory_ / name`);
   drop the per-frame `weakly_canonical()` syscall, key on the normalized string.
2. **Upload budget** — cap GPU uploads to `k_max_uploads_per_frame` (4) so a fresh
   grid can't stall a frame.
3. **In-memory RGBA handoff** — fresh thumbnails come back as an `img::ImageBuffer`
   uploaded directly (skip the encode->reread->decode round-trip); the PNG is still
   written for cross-session persistence (next session starts at `DiskReady`).
4. **Deferred eviction** — replaced/evicted textures go to a retire queue freed after
   `k_retire_frames` (3) instead of `vkDeviceWaitIdle()`.

State: `Queued -> Generating -> PixelsReady|DiskReady -> (upload, budget) -> Ready`.

## 6. FileBrowserThumbnailThread (async generation)

- **Images** (jpg/png/webp): submit a `decode -> resize(320x180) -> encode_png`
  pipeline to **`ImageJobSystem`** at `Priority::Low`; the future/result delivers the
  RGBA buffer back to the context (in-memory handoff) and writes the PNG. No private
  image pool — uses the shared multicore engine (already wired to Overwatch).
- **Video** (libmpv SW-render): a small dedicated `ManagedThread` worker (KillOnly,
  heartbeat in the mpv wait-loops) — relocated from `FileThumbnailCache` verbatim.
  This stays separate because it is not image-codec work.
- **Cancellation/backpressure**: submit lazily from `get()` (only entries queried that
  frame), `Low` priority, cancel via `stop_token`/`ImageJobSystem::clear_pending()` on
  `evict()`/`clear()`/folder change — bounds in-flight work to what is on screen.

## 7. Integration & retirement

- `FileBrowser` gains `Setup(vulkan_context*, thumb_dir)`, `ClearThumbnailCache()`,
  `RebuildThumbnail(path)`; **removes** `thumbnailProvider_` + `SetThumbnailProvider`.
  Render paths call `m_thumbnails.get(...)`; new-dir popup calls
  `m_file_ops.create_directory(...)`; `Display()` calls `m_thumbnails.begin_frame()`.
- `app_coordinator.cpp`: replace the `SetThumbnailProvider`/`is_thumb_path` lambda +
  clear/rebuild callbacks with `GetMainFileExplorer().Setup(m_vk, thumb_dir)` and thin
  forwarding callbacks.
- **Retire** `FileThumbnailCache` and `AppContext::m_thumb_cache`/`ThumbCache()` (used
  only by the file-explorer wiring); delete `file_thumbnail_cache.{hpp,cpp}`.
- CMake: add the three new `.cpp`s to the executable; remove `file_thumbnail_cache.cpp`.

## 8. Testing

- `FileBrowserFileOperations` is pure `std::filesystem` -> unit-test in the `image_tests`
  pattern (create/rename/remove/copy/move + error paths + a large-tree copy progress).
- Thumbnail context/thread are Vulkan/mpv-coupled -> verify via the app (the image
  pipeline itself — decode/resize/encode — is already covered by the engine tests).

## 9. Risks / notes

- `FileBrowser` gains a `vulkan_context*` (needed to own GPU upload) — it had none
  before. This is the cost of "engine inside FileBrowser" and is contained to `Setup`.
- The big `file_browser_ui.cpp` (1328 lines) render-path edits are the riskiest part
  and have no existing tests — do them with the user present / under review, not
  unattended.
- Keep mpv video thumbnails on their own worker this iteration (different lifecycle).
