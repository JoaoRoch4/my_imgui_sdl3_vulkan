# Memory management evaluation — `unique_ptr` vs `shared_ptr`

This note evaluates how the project manages object lifetimes and explains why
the central ownership class, [`AppContext`](../code/core/app_context.hpp), uses
`std::unique_ptr` rather than `std::shared_ptr`.

## How ownership is structured

`AppContext` is the **single owner** of every long-lived subsystem (image
viewer, video players, downloader, thumbnail cache, history, console, etc.).
Each subsystem is held in a `std::unique_ptr`; collaborators receive a
**non-owning raw pointer** through a typed getter (`Player()`, `ThumbCache()`,
…). `MainMenuBar` owns one `AppContext` and binds raw-pointer aliases to those
getters, so the rendering/wiring code is unchanged while ownership is
centralised.

```
App
 └── MainMenuBar  (owns 1)
      └── AppContext            ← sole owner, unique_ptr<each subsystem>
           ├── ImageViewerPanel
           ├── VideoPlayer / VideoPlayerPlacebo
           ├── FileThumbnailCache
           ├── HistoryPreview
           └── … (observers get raw pointers via getters)
```

## The decision: `unique_ptr`

`std::unique_ptr` is the correct default here, for three concrete reasons.

1. **Ownership is genuinely unique.** Each subsystem has exactly one owner —
   `AppContext`. Nothing else needs to keep it alive. `shared_ptr` models
   *shared* ownership; using it where ownership is single would add atomic
   refcount traffic on every copy and, worse, hide *who* is responsible for
   teardown.

2. **Deterministic, centralised destruction order.** `unique_ptr` members tear
   down in reverse declaration order, defined in one place
   ([`app_context.hpp`](../code/core/app_context.hpp)). GPU and worker-thread
   teardown is driven explicitly via `Shutdown()` *before* the object is
   destroyed, so order is predictable. `shared_ptr` would make the moment of
   destruction depend on whoever drops the last reference — exactly what you do
   **not** want for GPU resources that must be freed before
   `ImGui_ImplVulkan_Shutdown`.

3. **The threading model already guarantees the object outlives its threads.**
   The background workers in `FileThumbnailCache`, `VideoDownloader`,
   `HistoryPreview`, etc. are `std::jthread` *members*. A `jthread` requests
   stop and **joins on destruction**, so a subsystem is always destroyed only
   after its own threads have exited. There is no window where a thread runs
   against a freed `this` — so no reference count is needed to keep the object
   alive for the thread.

## When `shared_ptr` *is* the right tool here

There is exactly one `shared_ptr` in the codebase, and it is correct:
[`ImGuiConsole::Alive_`](../code/ui/console/imgui_console.hpp) —
`std::shared_ptr<std::atomic<bool>>`. The console spawns **detached**
`std::thread`s (not joined). A detached thread can outlive the console, so it
cannot hold a raw `this`. Instead both the console and the detached thread hold
a `shared_ptr` to a small "alive" flag; the thread checks the flag and the flag
stays valid until the last holder goes away. That is the textbook case for
`shared_ptr`: **a lifetime that must outlive its creator in an indeterminate
way.** None of the `AppContext` subsystems have that shape.

Rule of thumb used in this project:

| Situation | Choice |
| --- | --- |
| One clear owner, observers never extend lifetime | `unique_ptr` + raw observer pointers |
| Lifetime must outlive creator, shared across detached threads | `shared_ptr` (+ `weak_ptr` to observe) |
| Need to detect "did the owner die?" from a thread | `weak_ptr::lock()` / shared "alive" flag |

## Residual risk (not solved by pointer choice)

The real hazard is not *ownership* but *destruction sequencing*: subsystems hand
each other raw pointers in `MainMenuBar::Setup()` (e.g. `HistoryPreview` stores
`VideoPlayer*` and `ImageViewerPanel*`). This is safe **only because**
`AppContext` outlives all of them and `Shutdown()` stops the threads first.
Switching everything to `shared_ptr` would *not* make this safer — it would
merely turn a deterministic teardown into a non-deterministic one and risk
leaks via reference cycles. If a future subsystem needs to observe a sibling
from a *detached* thread, give it a `weak_ptr` (or a shared "alive" flag),
mirroring the console — do not promote the whole subsystem to `shared_ptr`.

## Summary

`unique_ptr` throughout `AppContext`, raw pointers for observers, and
`shared_ptr`/`weak_ptr` reserved for the detached-thread case. That matches the
project's actual ownership and threading model and keeps GPU/thread teardown
deterministic.
