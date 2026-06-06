# Parallel Image Library — Design Spec

- **Date:** 2026-06-05
- **Status:** Approved (design), pending implementation plan
- **Author:** brainstormed with Claude

## 1. Problem

Profiling shows image work (stb_image decode, stb_image_resize2, stb_image_write,
plus libwebp) is a bottleneck running on **one core**, in two distinct ways:

- **Task-level serialization** — batches of images (folder/thumbnail flood, bulk
  open) are processed through one or too few threads while other cores sit idle.
  - Bulk open uses a *single* sequential `ManagedThread` worker
    (`code/ui/media/image/dialog/bulk_image_open_queue.cpp`).
  - The viewer's full-resolution decode runs on the render thread
    (`VulkanTexture::load`, `code/rendering/vulkan/vulkan_texture.cpp:96`).
  - The thumbnail cache already spreads work over up to 4 **raw `std::jthread`s**
    (`code/ui/FileExplorer/file_thumbnail_cache.cpp`), but those bypass the
    project's `ManagedThread`/`ThreadOverwatch`/`ThreadRegistry` infrastructure.
- **Operation-level (intra-image)** — a single *large* image is slow because one
  `stbi_load` / `stbir_resize` call runs on a single core.

Both confirmed by the user ("Both").

## 2. Goal

Build an in-house parallel image-processing module that:

1. Wraps **stb + libwebp** as the real codecs (no codec reimplementation).
2. Uses the project's **own thread wrappers** (`ManagedThread`, so workers register
   with `ThreadRegistry`/`ThreadOverwatch`).
3. Provides **multicore** execution at both levels: many images across cores
   (task-level) and a single large resize split across cores (operation-level).
4. Leaves a clean seam for a **reflection/observability** layer later — explicitly
   deferred this iteration.

### Non-goals (deferred)

- Reflection/observability job panel; making param structs `rfl`-reflectable
  (they stay plain aggregates for now).
- mpv **video** thumbnails on the pool (kept on their existing 2-thread path).
- Parallel PNG **encode** (zlib is sequential; thumbnails are tiny).
- Intra-image parallel **decode** (stb decode of one JPEG/PNG is not splittable).
- stb `-march`/SIMD tuning (a separate build concern).
- Codec reimplementation.

## 3. Chosen approach

**Approach A — Job system on `ManagedThread` workers + tiled resize.** A single
persistent `ImageJobSystem` with N workers (each a `ManagedThread`) pulling typed
jobs from a priority queue. Task-level parallelism comes from many workers;
intra-image parallelism comes from `stb_image_resize2`'s native split API. The
job system only ever produces CPU pixel buffers; all Vulkan upload stays on the
render thread.

Rejected:
- **B (raw `jthread` workers + thin supervisor):** dodges the watchdog/long-job
  tension but loses the Overwatch/Registry integration that was a stated goal.
- **C (parallel STL / `std::async`):** least code, but no persistent pool,
  priorities, cancellation, or wrapper integration.

## 4. Architecture

### 4.1 Module layout — `code/core/image/` (infrastructure, beside `core/thread`)

| File | Responsibility |
|---|---|
| `image_buffer.hpp` | `img::ImageBuffer` — owning, move-only CPU pixel buffer (RGBA8: `data`, `width`, `height`, `channels`, `stride()`). Unifies stb/webp ownership behind one type. |
| `image_ops.hpp/.cpp` | Synchronous, thread-free wrappers over stb + libwebp: `decode_file`, `resize` (single-shot + split-aware), `encode_png`. Pure and unit-testable. |
| `image_job_system.hpp/.cpp` | `img::ImageJobSystem` — persistent pool of `ManagedThread` workers + priority queue + cooperative `parallel_for`. The only place threads live. |
| `image_types.hpp` | Plain param/result aggregates (`ResizeRequest`, `JobResult`, `Priority`, `ImageError`) — kept as aggregates so the deferred reflection layer can adopt them with zero churn. |

### 4.2 Layering (one direction only)

```
UI (thumbnail cache, bulk open, viewer)
      │ submits jobs / awaits futures
      ▼
ImageJobSystem ── parallel_for ──┐   (workers = ManagedThread, registered in ThreadRegistry)
      │ calls                    │
      ▼                          ▼
   image_ops ──────────────► stb_image / stb_image_resize2 / libwebp
      ▲
ImageBuffer (CPU pixels only — NEVER touches Vulkan)
```

### 4.3 CPU/GPU boundary (invariant)

The job system produces only `ImageBuffer`. Vulkan resource creation/upload stays
on the render thread. A new `VulkanTexture::upload(ImageBuffer&&, vulkan_context&)`
consumes a finished buffer on the render thread; the existing synchronous
`VulkanTexture::load()` is retained as a wrapper for cold callers (e.g. emoji
atlas) to keep the change contained. This preserves today's "generator threads
never touch Vulkan" contract.

## 5. Public API (sketch)

```cpp
namespace img {

enum class Priority { Low, Normal, High };
enum class ImageError { FileNotFound, UnsupportedFormat, DecodeFailed,
                        ResizeFailed, Cancelled, OutOfMemory };

class ImageJobSystem {
public:
    struct Config {
        unsigned worker_count  = 0;   // 0 => hardware_concurrency - reserve_cores
        unsigned reserve_cores = 2;   // leave cores for render + main
        std::chrono::milliseconds worker_timeout{30'000};
    };

    static ImageJobSystem& instance();   // process-wide, like ThreadRegistry
    void start(Config = {});
    void shutdown();                     // cancel pending, drain in-flight, join

    template <class F> [[nodiscard]]
    std::future<std::invoke_result_t<F>> submit(F&&, Priority = Priority::Normal,
                                                std::stop_token = {});

    // High-level ops, each runs on the pool:
    [[nodiscard]] std::future<std::expected<ImageBuffer, ImageError>>
        decode(std::filesystem::path, Priority = Priority::Normal, std::stop_token = {});
    [[nodiscard]] std::future<std::expected<ImageBuffer, ImageError>>
        resize(ImageBuffer, int w, int h, Priority = Priority::Normal, std::stop_token = {}); // tiled
    [[nodiscard]] std::future<std::expected<bool, ImageError>>
        encode_png(ImageBuffer, std::filesystem::path, Priority = Priority::Normal, std::stop_token = {});

    // Cooperative parallel-for: the CALLING thread helps execute chunks while it
    // waits, so nesting (a job calling parallel_for) cannot deadlock the pool.
    void parallel_for(int begin, int end, int min_chunk,
                      const std::function<void(int /*lo*/, int /*hi*/)>& body);

    [[nodiscard]] unsigned worker_count() const;
};

} // namespace img
```

Deliberate choices:
- `std::expected<T, ImageError>` for fallible ops (matches modern-C++23 house
  style); exceptions reserved for the truly exceptional.
- Optional `std::stop_token` per submission for cancellation (see §8).

## 6. Concurrency model

### 6.1 Worker loop (each worker is a `ManagedThread`)

```
ManagedThread body (RecoveryPolicy::KillOnly, timeout 30s):
  lock; cv.wait_for(timeout/2, [queue not empty || stopping]);
  pop highest-priority job;  unlock;
  if (high-level job && job.stop_token.stop_requested()) -> fulfill expected{Cancelled}; continue;
  self.set_status("queue", depth);  self.heartbeat();
  run job (decode / resize-split / encode / generic);  // heartbeat() between stages
  fulfill its std::promise (value or std::expected error)
```

- **`KillOnly` is deliberate:** a legitimately long resize must never trip the
  restart-storm `abort()` in `ManagedThread`. Liveness stays visible via
  `heartbeat()` per stage/tile. Trade-off: a *genuinely* hung op won't auto-restart
  — it surfaces as a stuck worker in `ThreadRegistry` rather than crashing the
  process. Acceptable for CPU-bound work.
- **Worker count** = `hardware_concurrency − reserve_cores` (default reserve 2),
  floored at 1.
- Workers auto-register in `ThreadRegistry` via `ManagedThread` construction, so
  the deferred observability panel is essentially free.

### 6.2 Cooperative `parallel_for` (avoids nested-pool deadlock)

A worker running `resize` calls `parallel_for`, which submits sub-tasks to the
*same* pool. If it waited idle while all other workers were also blocked, the pool
would deadlock. `parallel_for` therefore has the **calling thread help execute**:
while waiting on the completion latch it pops and runs pending chunks itself, so
forward progress is guaranteed even when every other worker is busy. This is why
`parallel_for` is a pool method, not a free `std::async`.

### 6.3 Intra-image resize (the "one big image" fix)

Uses `stb_image_resize2`'s native split API
(`external/stb/stb_image_resize2.h:676-689`):

```
resize(src, w, h):
  if dst_area < k_tile_threshold:
      return image_ops::resize(...)                       // single-shot, no overhead (thumbnails)
  STBIR_RESIZE r; stbir_resize_init(&r, ...); set buffers/layouts/filters;
  int S = stbir_build_samplers_with_splits(&r, worker_count);   // stb decides real split count
  parallel_for(0, S, 1, [&](lo, hi){
      stbir_resize_extended_split(&r, lo, hi - lo);
  });
  stbir_free_samplers(&r);
```

- **Decode stays single-threaded per image** (stb limitation). Batch decodes
  parallelize at the task level instead.
- **PNG encode stays single-threaded** (zlib sequential; thumbnails are tiny).

## 7. Data flow & migration (iteration 1)

- **Thumbnail cache** (`code/ui/FileExplorer/file_thumbnail_cache.*`): remove the
  4 raw `jthread`s and `k_max_img_generators`. `get()` submits a
  `decode → resize → encode` pipeline at `Low` priority, stores the `std::future`
  in `Entry`, and polls `future.wait_for(0)`; on ready → `DiskReady`, GPU upload on
  the render thread unchanged. **Video (mpv) thumbnails stay on their existing
  2-thread path** this iteration.
- **Bulk open** (`code/ui/media/image/dialog/bulk_image_open_queue.cpp`): replace
  the single sequential worker with per-path job submission; results stream into
  `m_ready_paths` as futures complete.
- **Viewer full load** (`code/rendering/vulkan/vulkan_texture.cpp:96`): split into
  pool `decode()` + render-thread `VulkanTexture::upload(ImageBuffer&&)`. Existing
  `load()` retained as a wrapper for cold callers.

**Lifecycle:** `ImageJobSystem::instance().start()` during app init (after the
thread system is up); `shutdown()` before Vulkan teardown **and** before
`ThreadRegistry`/`ThreadOverwatch` teardown — wired in `code/main/app.cpp`.

## 8. Error handling & cancellation

- stb returning `nullptr` → `DecodeFailed`; missing path → `FileNotFound`.
- Generic `submit(F&&)` wraps the callable in `std::packaged_task`: an exception
  inside a job is captured into its `std::future` and the **worker keeps running**.
  High-level ops do not throw — expected failures travel as `std::expected`.
- **Cancellation** via optional `std::stop_token`, checked at two points: when a
  job is **popped** (pending → `Cancelled`, near-free) and **between pipeline
  stages / resize splits** (in-flight). stb decode is opaque, so granularity is
  per stage, not mid-call.
- **Backpressure:** the thumbnail cache submits **lazily from `get()`** (only
  entries queried that frame) at `Low` priority and requests stop on
  `evict()`/`clear()`. This bounds the queue to roughly what is on screen — no
  pre-enqueuing a 10k-file folder.
- **`shutdown()`:** set stopping → notify → cancel pending (`Cancelled`) → let
  short in-flight jobs finish / observe the token → join all workers.

### Memory note

The decode wrapper copies the codec buffer once into the `ImageBuffer`'s storage
and frees the stb/webp allocation immediately, so ownership is uniform. A later
optimization can use a custom-deleter buffer to avoid that single copy.

## 9. Testing (no harness exists today — add one)

- Add **doctest** (header-only, vendored under `external/` like other deps) and a
  separate `image_tests` executable target, not linked into the app.
- **Pure unit tests** on `image_ops` (no threads): decode fixtures (PNG/JPG/WebP)
  → expected dims; resize → dims; `encode_png` round-trip; error paths
  (`FileNotFound`, `DecodeFailed`).
- **Equivalence test (key guarantee):** tiled resize (`S > 1`) output is
  **byte-for-byte identical** to single-shot `stbir`. Pins the parallel path to
  the serial one.
- **Concurrency tests under ThreadSanitizer:** `parallel_for` range-sum under
  contention; N submissions all complete with correct results; cancellation
  runs/doesn't-run as expected; `shutdown()` drains without hang; stress thousands
  of tiny jobs → no deadlock, workers stay registered.

## 10. Success criteria

1. Folder/bulk flood saturates all worker cores (CPU graph: N busy, not 1).
2. A single large resize scales ~`S×` up to the split limit.
3. No render-thread stall on viewer decode.
4. Tiled resize == single-shot resize (bit-exact).
5. Clean shutdown under TSan; workers visible in `ThreadRegistry`.

## 11. Open questions / risks

- **Watchdog tuning:** the 30s worker timeout + per-stage heartbeat must be
  validated against the largest expected resize so `KillOnly` workers never look
  hung during legitimate work.
- **`stbir` split determinism:** the equivalence test must confirm split output
  matches single-shot exactly for the project's filters/edge modes; if a filter
  ever diverges, fall back to single-shot for that case.
- **doctest dependency:** introduces the first test target; confirm it should be
  vendored under `external/` and excluded from the app build.
