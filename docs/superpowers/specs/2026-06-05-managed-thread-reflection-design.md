# ManagedThread + Thread Reflection System — Design

Date: 2026-06-05
Status: Approved (pending implementation plan)

## Goal

Provide a single thread wrapper, `ManagedThread`, that every background thread in the
app flows through. The wrapper:

1. Owns a `std::jthread`.
2. Names the OS thread via `pthread_setname_np` (validated and truncated to the Linux
   15-char limit).
3. Auto-registers the thread with `ThreadOverwatch` (existing watchdog) for the thread's
   whole lifetime and heartbeats once per loop iteration.
4. Self-registers richer live metadata with a new `ThreadRegistry` for runtime
   introspection ("thread reflection"), surfaced in an ImGui debug panel.
5. Enforces a configurable recovery policy (`KillOnly` / `RestartOnTimeout`) and a
   restart-storm guard that hard-aborts the process when a `RestartOnTimeout` thread
   respawns too many times in sequence.

All six existing hand-rolled threads are migrated onto the wrapper.

## Motivation

Today thread setup is copy-pasted and inconsistent, with two confirmed defects:

- **Broken name (bug):** `code/ui/FileExplorer/file_browser_thread.cpp:116` calls
  `pthread_setname_np(static_cast<pthread_t>(watch_id), ...)`. `watch_id` is a
  `ThreadOverwatch::WatchId` counter (1, 2, 3…), not a `pthread_t`. The name is applied to
  a garbage handle and never reaches the worker thread.
- **Silently dropped names (latent):** Linux caps thread names at 15 chars + NUL.
  `"FileBrowserScannerThread"` (24) and `"BulkImageOpenQueueThread"` (24) exceed it, so
  `pthread_setname_np` returns `ERANGE` and sets nothing.

Naming is also done three different ways (`pthread_self()` inside the lambda,
`native_handle()` from the spawner, and the buggy `watch_id` cast). Overwatch registration
is hand-wired per call site with subtly different lifetimes. Centralizing all of this in
one RAII type makes correct behavior the default and removes the duplication.

## Architecture

```
ManagedThread (owns std::jthread)
   ├── names itself        → pthread_setname_np (truncated, validated)
   ├── registers liveness  → ThreadOverwatch   (existing — recovery/watchdog)
   └── registers metadata  → ThreadRegistry    (new — read-only introspection)
                                   ↑
                          ThreadReflectionPanel (new ImGui debug window)
```

`ThreadOverwatch` and `ThreadRegistry` are kept **separate** by single responsibility:
Overwatch owns *liveness and recovery* (it kills and restarts); the registry owns
*read-only introspection* (it never acts). `ManagedThread` links them by storing the
Overwatch `WatchId` inside its `ThreadInfo`, so the panel can still display watch status.

### File layout

New:
- `code/core/thread/managed_thread.hpp`
- `code/core/thread/managed_thread.cpp`
- `code/core/thread/thread_registry.hpp`
- `code/core/thread/thread_registry.cpp`
- `code/ui/window/widgets/thread_reflection_panel.hpp`
- `code/ui/window/widgets/thread_reflection_panel.cpp`

Modified:
- The six consumers (see Migration).
- `code/main/app.cpp` (panel instantiation, per-frame draw, debug-menu toggle).
- `CMakeLists.txt` (new source files).

Code style: strict modern C++23 to match the repo memory — `static_cast`/`bit_cast` only,
smart pointers, `std::span`/`std::string_view`, `constexpr`, `std::println`-style logging
via the project's `APP_DEBUG_LOG`, attached braces, 4-space indentation matching
`thread_overwatch.cpp`.

## Component 1 — `ManagedThread`

The wrapper **owns the loop**. The caller supplies a per-iteration body; the wrapper runs
it repeatedly, heartbeating once per iteration.

```cpp
class ManagedThread {
public:
    // Called repeatedly until stop is requested. Receives the stop_token and a self-ref
    // so the body can publish status or stop itself (one-shot tasks call request_stop()).
    using IterationFn = std::function<void(const std::stop_token&, ManagedThread&)>;

    struct Config {
        std::string                     name;                  // truncated to 15 chars
        std::chrono::milliseconds       timeout{5000};
        ThreadOverwatch::RecoveryPolicy policy =
            ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
        bool                            watch = true;          // opt out of Overwatch
        // Restart-storm guard (RestartOnTimeout only):
        uint32_t                        max_consecutive_restarts = 5;
        std::chrono::milliseconds       restart_reset_window{60'000};
    };

    ManagedThread(Config cfg, IterationFn body);
    ~ManagedThread();                          // request_stop + join + unwatch + unregister

    ManagedThread(const ManagedThread&)            = delete;
    ManagedThread& operator=(const ManagedThread&) = delete;

    void request_stop();
    bool joinable() const;

    void set_status(std::string key, std::string value);  // custom reflection status
};
```

### Lifecycle and identity

The registry entry and the Overwatch watch live for the **`ManagedThread` object's**
lifetime, not for a single OS-thread run. This is what makes restart clean: a respawn
reuses the same registry id and `WatchId`, so the panel shows one stable row whose
`restart_count` ticks up, and Overwatch keeps a single watch (it expects the watch to
persist and simply re-invokes `restart_request`). Registering inside `run()` instead would
create a duplicate registry entry and a fresh `WatchId` on every restart.

- **Constructor:** truncate the name; `register_thread` (tid unknown yet); create the
  Overwatch watch (stable kill/restart callbacks); start the first `jthread` running
  `run`.
- **`run(st)`:** name the OS thread; publish the real tid; loop, heartbeating and counting
  iterations; on exit, mark `Stopped`. Does **not** register/unregister or create/destroy
  the watch.
- **`respawn()`:** request_stop + join the old thread, bump `restart_count`, start a fresh
  `jthread` running `run`. Watch and registry entry persist.
- **Destructor:** request_stop + join, `unwatch`, `unregister_thread`.

```cpp
ManagedThread::ManagedThread(Config cfg, IterationFn body)
    : m_cfg{std::move(cfg)}, m_body{std::move(body)} {
    m_name        = truncate_to_15(m_cfg.name);                  // warns if truncated
    m_registry_id = ThreadRegistry::instance().register_thread(m_name, m_cfg.policy);
    start();
}

void ManagedThread::start() {
    m_thread = std::jthread{[this](std::stop_token st) { run(std::move(st)); }};
    if (m_cfg.watch) {
        m_watch_id = ThreadOverwatch::instance().watch(
            m_name, m_cfg.timeout,
            /*kill*/    [this] { request_stop(); },
            /*restart*/ [this] { respawn(); },
            m_cfg.policy);
        ThreadRegistry::instance().set_watch_id(m_registry_id, m_watch_id);
    }
}

void ManagedThread::run(std::stop_token st) {
    pthread_setname_np(pthread_self(), m_name.c_str());     // m_name already ≤15 chars
    ThreadRegistry::instance().set_tid(m_registry_id, gettid());
    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Running);

    while (!st.stop_requested()) {
        if (m_cfg.watch)
            ThreadOverwatch::instance().heartbeat(m_watch_id);
        ThreadRegistry::instance().note_iteration(m_registry_id);
        try {
            m_body(st, *this);
        } catch (const std::exception& e) {
            APP_DEBUG_LOG("[ManagedThread] {} body threw: {}", m_name, e.what());
            // log and continue to the next iteration; one bad job does not kill the thread
        }
    }
    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Stopped);
}
```

### Load-bearing contract: bounded idle

Because the watch lives for the thread's whole lifetime, **the body must return within
`timeout` even when idle**. Condvar workers use `cv.wait_for(lk, timeout/2, pred)` instead
of an unbounded `wait`. This replaces the current "drop the watch while idle" trick with
"keep the watch, heartbeat on a bounded cadence." It is simpler and still trips on a
genuine hang (a stuck iteration stops heartbeating past `timeout`).

This contract is documented in the `ManagedThread` header so every caller sees it.

### Name truncation

`pthread_setname_np` fails with `ERANGE` for names longer than 15 chars. The constructor
truncates `Config::name` to 15 chars, stores the truncated form in `m_name`, and logs a
debug warning when truncation occurs (e.g. `"VideoDownloaderThread"` →
`"VideoDownloader"`). This is the single fix for the latent silent-failure defect.

### Recovery policy

- `KillOnly`: on timeout, Overwatch invokes the kill callback (`request_stop()`) and drops
  the watch. The loop exits at its next `stop_token` check. No respawn.
- `RestartOnTimeout`: on timeout, Overwatch invokes the restart callback (`respawn()`).

A cooperative hung thread (one that observes `stop_token`) exits and is cleaned up. A
truly wedged thread (ignoring `stop_token`) leaks — the same limitation as today, now
documented in one place.

### Restart-storm guard

`respawn()` runs on the Overwatch monitor thread. It uses a **time-windowed consecutive
counter** so only genuinely back-to-back failures trip the breaker; a thread that recovers
and runs healthy for `restart_reset_window` has its streak reset.

```cpp
void ManagedThread::respawn() {                  // on the Overwatch monitor thread
    std::lock_guard lk(m_respawn_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_restart >= m_cfg.restart_reset_window)
        m_consecutive_restarts = 0;              // streak broken — thread had recovered
    ++m_consecutive_restarts;
    m_last_restart = now;

    if (m_consecutive_restarts > m_cfg.max_consecutive_restarts) {
        escalate();                              // hard abort — see below
        return;                                  // do NOT recreate the thread
    }

    ThreadRegistry::instance().note_restart(m_registry_id);  // ++restart_count, state=Restarting
    m_thread.request_stop();
    if (m_thread.joinable())
        m_thread.join();                         // drains a cooperative thread; a wedged one leaks
    start_thread_only();                         // fresh jthread; watch + registry entry persist
}
```

`start_thread_only()` is the thread-spawning half of `start()` (it does **not** re-create
the watch, which already persists). The consecutive/total restart counters live on the
`ManagedThread`; the registry's `restart_count` is bumped via `note_restart`.
```

`m_respawn_mutex` serializes `respawn()` (monitor thread) against `request_stop()` and the
destructor (owning thread), since both touch the `jthread` member.

### Escalation: hard abort

When `max_consecutive_restarts` is exceeded, `escalate()` treats the restart storm as an
unrecoverable logic fault and fails fast:

```cpp
void ManagedThread::escalate() {
    const std::string reason = std::format(
        "restart storm: {} consecutive restarts within {}ms",
        m_consecutive_restarts, m_cfg.restart_reset_window.count());
    APP_DEBUG_LOG("[ManagedThread] FATAL {}: {}", m_name, reason);
    if (m_cfg.watch)
        ThreadOverwatch::instance().unwatch(m_watch_id);     // stop the restart cycle
    ThreadRegistry::instance().set_failed(m_registry_id, reason);
    APP_DEBUG_LOG("[ManagedThread] stacktrace:\n{}",
                  std::to_string(std::stacktrace::current()));
    std::abort();
}
```

Unwatching from inside `restart_request` is safe: Overwatch's monitor handles a
now-missing watch gracefully (`thread_overwatch.cpp:220-228`, "recovery finished but watch
gone"). The registry is updated to `Failed` (with reason) before aborting so any flushed
log/panel state reflects the cause.

`<stacktrace>` is already included via the PCH, so the project already depends on it.

## Component 2 — `ThreadRegistry`

Singleton with a `std::shared_mutex` (many concurrent `snapshot()` readers, single writer
for mutations). `snapshot()` returns a value copy so the UI never holds the lock while
rendering.

```cpp
enum class ThreadState { Starting, Running, Stopping, Stopped, Restarting, Failed };

struct ThreadInfo {
    uint64_t                              id;             // registry id
    std::string                           name;
    pid_t                                 tid;            // gettid() — real OS thread id
    ThreadState                           state;
    ThreadOverwatch::RecoveryPolicy       policy;
    uint64_t                              iterations;
    uint64_t                              restart_count;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point last_heartbeat;
    uint64_t                              overwatch_watch_id;
    std::string                           failed_reason;  // set when state == Failed
    std::vector<std::pair<std::string, std::string>> status;  // custom kv
};

class ThreadRegistry {
public:
    static ThreadRegistry& instance();

    uint64_t register_thread(std::string name,            // tid unknown until run() starts
                             ThreadOverwatch::RecoveryPolicy policy);
    void     unregister_thread(uint64_t id);
    void     set_tid(uint64_t id, pid_t tid);             // published by run() on each start
    void     note_iteration(uint64_t id);                 // ++iterations, stamp heartbeat
    void     note_restart(uint64_t id);                   // ++restart_count, state=Restarting
    void     set_state(uint64_t id, ThreadState s);
    void     set_failed(uint64_t id, std::string reason);
    void     set_status(uint64_t id, std::string key, std::string value);
    void     set_watch_id(uint64_t id, uint64_t watch_id);

    std::vector<ThreadInfo> snapshot() const;             // value copy under shared lock
};
```

`note_iteration` is called every loop iteration, so it must be cheap: a single
unique-lock, integer increment, and timestamp stamp. `set_status` replaces or appends a kv
pair by key.

## Component 3 — `ThreadReflectionPanel`

Mirrors `FpsPlot` / `StyleEditor`: instantiated in `app.cpp`, drawn each frame, toggled by
a checkbox in the existing debug menu (`app.cpp:245-247`).

```cpp
class ThreadReflectionPanel {
public:
    void draw(bool* open);   // sortable ImGui table from ThreadRegistry::snapshot()
};
```

Columns: **name · tid · state · policy · iterations · restarts · last-HB age (ms) · watch
id · status**. Read-only. `Failed` (and optionally `Restarting`) rows are tinted red. The
status column joins the kv pairs into a compact `k=v k=v` string.

## Component 4 — Migration

Each hand-rolled `while (!stop) { wait; work; }` loop becomes a per-iteration
`IterationFn` body. The wrapper supplies naming, watch registration, and heartbeats.

| Consumer            | Body shape                         | Policy   | Notes                                            |
|---------------------|------------------------------------|----------|--------------------------------------------------|
| `VideoDownloader`   | bounded-wait queue worker          | KillOnly | `wait_for(timeout/2)` for a job                  |
| `HistoryPreview`    | bounded-wait queue worker          | Restart  | already uses restart-on-recovery                 |
| `VideoHoverPreview` | bounded-wait worker                | KillOnly |                                                  |
| `VideoSeekPreview`  | bounded-wait worker                | KillOnly |                                                  |
| `BulkImageOpenQueue`| one-shot                           | KillOnly | body does the job, then `self.request_stop()`    |
| `FileBrowserScanner`| bounded-wait + per-entry status    | KillOnly | fixes the `watch_id`-as-`pthread_t` bug          |

Migration rules:

- Replace unbounded condvar `wait` with `wait_for(timeout/2, pred)` to honor the
  bounded-idle contract.
- Move per-unit progress reporting (e.g. per-directory-entry in `FileBrowserScanner`) to
  `self.set_status(...)` so it appears in the panel.
- One-shot bodies (e.g. `BulkImageOpenQueue`) perform their full job in a single iteration
  and then call `request_stop()`.
- All thread names must be ≤15 chars; the wrapper truncates regardless, but names are
  chosen to be meaningful within the limit (e.g. `VideoDownloader`, `HistoryPreview`,
  `FileBrowserScan`).

## Error handling

- Exceptions escaping the body are caught, logged, and the loop continues; one-shots stop
  themselves.
- Name too long: truncated + warned, never silently dropped.
- Restart storm: hard abort with stacktrace (see Escalation).
- Overwatch restart/destroy overlap: serialized by `m_respawn_mutex`; a wedged
  non-cooperative thread leaks (documented limitation).
- `snapshot()` never throws and never blocks writers longer than a copy.

## Testing

This code is hard to unit-test in isolation (singletons, OS threads, ImGui), so testing is
a mix of a self-test command and manual verification:

- **`managed_thread_selftest`** — a console command registered via
  `ImGuiConsole::RegisterCommand`, which:
  1. Spawns a `ManagedThread`, then reads the name back with `pthread_getname_np` and
     asserts it matches the truncated name.
  2. Asserts the iteration counter advances in the registry snapshot.
  3. Publishes a status via `set_status` and confirms it appears in `snapshot()`.
  4. Deliberately hangs a single iteration to confirm the watchdog fires and
     `restart_count` increments by one (a single restart, not a full storm).
- **Manual** — open the reflection panel and confirm all migrated threads appear with
  correct ≤15-char names, advancing iteration counts, and live status.

The self-test deliberately exercises only a single restart. A test that actually exceeded
`max_consecutive_restarts` would `std::abort()` the app, which is the intended production
behavior but not something to trigger from an in-app self-test.

## Out of scope

- No change to `ThreadOverwatch`'s public API or recovery semantics.
- No unrelated refactoring of the consumers beyond what migration requires.
- No persistence of reflection data across runs; the panel shows live state only.
