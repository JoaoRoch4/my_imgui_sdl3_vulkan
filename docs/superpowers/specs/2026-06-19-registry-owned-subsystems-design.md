# Design: Delete `AppContext`, registry owns all subsystems

**Date:** 2026-06-19
**Branch:** `refactor/registry-owned-subsystems`

## Goal

The original ask was "in `app_coordinator.hpp` use memory management templates
instead of raw pointers." Through brainstorming the scope settled on the full
form: **move every top-level subsystem out of `AppContext`'s `unique_ptr`
members into the `MemoryManagement` registry (`PushGet`/`Release`/`GetInstance`),
and delete `AppContext` entirely.** The coordinator keeps thin *cached observer*
pointers, but their binding source moves from `m_ctx->Xxx()` to
`MemoryManagement::GetInstance<T>()`. mpv per-video handles are explicitly out of
scope (see Non-Goals).

## Decisions (from brainstorming)

1. **Everything via registry** — the 17 subsystems become registry-owned.
2. **Remove `AppContext` entirely** — `App` (the central allocation point)
   `PushGet`s them directly.
3. **Rebind cached observers in `Setup()`** — keep the coordinator's observer
   pointer members, but bind them from `GetInstance<T>()`. Lowest churn; the
   `.cpp` call sites (`m_video_player->` …) are unchanged, and `m_vk` keeps its
   `nullptr`-until-`Setup()` sentinel semantics.
4. **mpv deferred** — see Non-Goals.

## Non-Goals

- The per-video mpv handles (`mpv_handle*` / `mpv_render_context*` in
  `VideoEntry` / `PlaceboEntry`) are **not** touched. They cannot live in the
  type-keyed registry (N instances of one type; opaque C handles created by
  `mpv_create` / destroyed by `mpv_terminate_destroy`, not `make_unique`-able).
  They are already RAII-managed inside `std::vector<std::unique_ptr<…Entry>>`.
  Wrapping the raw handles in custom `unique_ptr` deleters is a possible future
  follow-up.

## Load-bearing invariants to preserve

- **Deterministic teardown order.** `AppContext` destroys its members in reverse
  declaration order. The registry's per-type `Release` must run in the exact
  reverse of the `PushGet` order, and the 17 subsystem destructors must run at
  the same point in `App::destroy()` they do today (i.e. after `m_Vk->cleanup()`,
  via `Release<AppCoordinator>()`'s neighbourhood).
- **GPU teardown before ImGui Vulkan shutdown.** Explicit GPU/thread teardown
  happens in `AppCoordinator::Shutdown()` while Vulkan is alive; the C++
  destructors are plain cleanup + thread joins afterwards.
- **`VulkanEmojiAtlas` two-phase lifecycle.** Needs `vulkan_context&`, so built
  in `Setup()` and freed in `Shutdown()` *before* ImGui's Vulkan backend dies.
- **Reopen-safety.** The registry is a global singleton and `App::run()` re-runs
  `Alloc()` each reopen iteration, so every `PushGet` must have a matching
  `Release` in `destroy()` to avoid duplicate type entries.
- **`m_vk` sentinel.** `if (!m_vk)` guards mean "has `Setup()` run"; binding in
  `Setup()` preserves that exactly.

## Subsystem order (authoritative)

`PushGet` in `App::Alloc()` in this order, immediately before
`PushGet<AppCoordinator>`:

1. ImageViewerPanel
2. OpenImageDialogs
3. BulkImageOpenQueue
4. VideoPlayer
5. VideoPlayerPlacebo
6. VideoDownloader
7. ConfigRuntime
8. HistoryPreview
9. OpenedFilesWindow
10. VideoContextMenu
11. FileBrowserContextMenu
12. FileThumbnailCache
13. MetadataEditor
14. MediaHistoryManager
15. MediaLoadHandler
16. AppStateCoordinator
17. ConsoleCommands

`VulkanEmojiAtlas` is created later in `AppCoordinator::Setup()` (not in `Alloc`).

`Release` in `App::destroy()` in the exact reverse (ConsoleCommands → …
→ ImageViewerPanel), inserted right after `Release<AppCoordinator>()` and before
`Release<StyleEditor>()`.

## Per-file changes

### `code/core/app_context.hpp` / `app_context.cpp`  — DELETE
Remove from `CMakeLists.txt` sources.

### `code/main/app.hpp`
Remove `class AppContext;` forward decl and `AppContext* m_AppContext` member.

### `code/main/app.cpp`
- Remove `#include "app_context.hpp"` and `m_AppContext = AppContext::GetInstance();`.
- Add the 17 subsystem headers (the include set currently in `app_context.cpp`).
- `Alloc()`: `PushGet` the 17 (order above) before `AppCoordinator`; extend the
  return-value `&&` chain (or assert non-null).
- `destroy()`: `Release` the 17 in reverse in the registry-teardown block.

### `code/ui/window/widgets/app_coordinator.hpp`
- Remove `std::unique_ptr<AppContext> m_ctx;` and `class AppContext;`.
- Keep all observer pointer members (18 aliases + 5 externals); update the
  comment block to say they are bound from the `MemoryManagement` registry in
  `Setup()`.

### `code/ui/window/widgets/app_coordinator.cpp`
- Constructor: drop `m_ctx{make_unique<AppContext>()}` and the alias-binding
  block. Members stay null-initialised (header default initialisers).
- `Setup()`: bind every alias via `GetInstance<T>()` at the top, alongside the
  externals already resolved there.
- EmojiAtlas: replace `m_ctx->CreateEmojiAtlas(*m_vk)` with
  `MemoryManagement::Get().PushGet<VulkanEmojiAtlas>("EmojiAtlas", *m_vk)` (bind
  `m_emoji_atlas` from it); replace `m_ctx->DestroyEmojiAtlas()` in `Shutdown()`
  with `MemoryManagement::Get().Release<VulkanEmojiAtlas>()`.
- Remove `mc.ctx = m_ctx.get();` in `Build()`.
- Remove `#include "app_context.hpp"`. All other call sites unchanged.

### `code/ui/window/widgets/main_menu_bar.hpp` / `.cpp`
- Drop `AppContext *ctx` from `MenuContext`.
- `Draw()` resolves its 9 subsystems via `GetInstance<T>()` instead of
  `ctx.Xxx()`. Replace `#include "app_context.hpp"` with `Memory_management.hpp`.

### `code/ui/config/config_runtime_ui_context.hpp`
Fix the stale comment referencing `AppContext::GetInstance()`.

## Cleanup side effect

Deleting `AppContext` removes a latent duplicate-instance footgun: today
`AppContext::GetInstance()` is a Meyers singleton that `App` builds into an
otherwise-unused `m_AppContext`, while `AppCoordinator` builds a *separate*
`AppContext` via `make_unique` and does all real work through that.

## Verification

- Clean build with clang/clang++ + LLD (Debug).
- Launch-and-reopen smoke test (registry teardown + fresh `Alloc` each reopen).
- Adversarial multi-agent review of the diff: call-site completeness, teardown
  ordering / reopen-safety, residual `AppContext` references, EmojiAtlas
  lifecycle, header/include hygiene, build/link correctness.
