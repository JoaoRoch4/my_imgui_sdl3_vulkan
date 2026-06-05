# ManagedThread + Thread Reflection System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `ManagedThread` — a `std::jthread` wrapper that names its OS thread, auto-registers with `ThreadOverwatch` and a new `ThreadRegistry`, and enforces a configurable recovery policy with a restart-storm guard — plus an ImGui reflection panel that displays all managed threads live.

**Architecture:** Three new units in `code/core/thread/` (`ThreadRegistry`, `ManagedThread`) and `code/ui/window/widgets/` (`ThreadReflectionPanel`). The thread sources are moved into a `core_thread` static library so a headless `thread_selftest` executable can unit-test `ThreadRegistry` and `ManagedThread` without the GUI. `ManagedThread` owns the loop and heartbeats once per iteration; the registry/watch live for the object's lifetime so restarts reuse one stable identity.

**Tech Stack:** C++23 (Clang + LLD), CMake (Ninja Multi-Config presets), Dear ImGui, `std::jthread`/`std::stop_token`, pthreads, `std::stacktrace`.

**Spec:** `docs/superpowers/specs/2026-06-05-managed-thread-reflection-design.md`

**Scope of this plan:** Infrastructure only — the wrapper, registry, panel, self-test exe, app wiring, and an in-app self-test command. **Migration of the six existing consumers is a separate plan** (`Phase 2`, see the final section) to be written against the compiled `ManagedThread` API after this plan merges.

**Conventions for every task:**
- Configure once (if `build/all` is missing): `cmake --preset all`
- Build the headless test exe: `cmake --build --preset build-debug --target thread_selftest`
- Run it: `./build/debug/thread_selftest; echo "exit=$?"` (exit 0 = pass)
- Build the app (compile check for GUI tasks): `cmake --build --preset build-debug --target example_sdl3_vulkan`
- Code style: strict C++23 — `static_cast`/`bit_cast` only, smart pointers, `std::span`/`string_view`, `constexpr`, attached braces, 4-space indent matching `thread_overwatch.cpp`.

> Note: the test exe path is `build/debug/thread_selftest`. If the Ninja Multi-Config layout places it elsewhere, locate with `find build -name thread_selftest -type f`.

---

### Task 1: CMake — `core_thread` library + headless `thread_selftest` exe

Establishes the test vehicle before any testable code exists. Moves the existing `thread_overwatch.cpp` into a static library shared by the app and the test exe, and adds an empty test exe that builds and returns 0.

**Files:**
- Modify: `CMakeLists.txt`
- Create: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Create the self-test stub**

`code/core/thread/thread_selftest.cpp`:
```cpp
// Headless unit tests for the core_thread library (ThreadRegistry, ManagedThread).
// No GUI/Vulkan dependencies — runs as a plain console executable.
// Convention: each check prints a line; main returns non-zero on first failure.
#include "pch.hpp"

namespace {

int g_failures = 0;

void check(bool cond, std::string_view what)
{
    if (cond) {
        std::println("[ok]   {}", what);
    } else {
        std::println("[FAIL] {}", what);
        ++g_failures;
    }
}

} // namespace

int main()
{
    std::println("== thread_selftest ==");
    // Tasks below append their checks here.
    std::println("== done: {} failure(s) ==", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add the library + test exe to CMakeLists.txt**

In `CMakeLists.txt`, **remove** the line `    code/core/thread/thread_overwatch.cpp` from the `add_executable(example_sdl3_vulkan ...)` source list (around line 130). Then, immediately **before** the `add_executable(example_sdl3_vulkan` block, add:

```cmake
# ── core_thread: thread primitives, shared by the app and the headless test ────
add_library(core_thread STATIC
    code/core/thread/thread_overwatch.cpp
    code/core/thread/thread_registry.cpp
    code/core/thread/managed_thread.cpp
)
target_include_directories(core_thread PUBLIC
    ${CMAKE_SOURCE_DIR}/code
    ${CMAKE_SOURCE_DIR}/code/pch
)
target_precompile_headers(core_thread PRIVATE code/pch/pch.hpp)
target_compile_features(core_thread PUBLIC cxx_std_23)
# std::stacktrace backend (libstdc++ >= 13). Harmless if unused on other stdlibs.
target_link_libraries(core_thread PUBLIC pthread stdc++exp)
# core_thread compiles the heavy PCH headers; reuse the app's include dirs.
target_include_directories(core_thread PRIVATE
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
)
```

> The `thread_registry.cpp` and `managed_thread.cpp` files don't exist yet; Tasks 2 and 4 create them. To make Task 1 build on its own, create empty stubs now: `printf '#include "pch.hpp"\n' > code/core/thread/thread_registry.cpp` and the same for `managed_thread.cpp`.

After the `add_executable(example_sdl3_vulkan ...)` block, link the library and add the test exe (place near the other target config, after `target_link_libraries(example_sdl3_vulkan ...)`):

```cmake
target_link_libraries(example_sdl3_vulkan PRIVATE core_thread)

# ── Headless thread self-test ──────────────────────────────────────────────────
add_executable(thread_selftest code/core/thread/thread_selftest.cpp)
target_link_libraries(thread_selftest PRIVATE core_thread imgui)
target_precompile_headers(thread_selftest REUSE_FROM example_sdl3_vulkan)
```

> If `target_link_libraries(example_sdl3_vulkan ...)` does not already exist verbatim, append `core_thread` to whatever links into `example_sdl3_vulkan`. Inspect with `grep -n "target_link_libraries(example_sdl3_vulkan" CMakeLists.txt`.

- [ ] **Step 3: Configure and build the test exe**

Run:
```bash
printf '#include "pch.hpp"\n' > code/core/thread/thread_registry.cpp
printf '#include "pch.hpp"\n' > code/core/thread/managed_thread.cpp
cmake --preset all
cmake --build --preset build-debug --target thread_selftest
```
Expected: configures and links; produces `thread_selftest`.

- [ ] **Step 4: Run it**

Run: `./build/debug/thread_selftest; echo "exit=$?"`
Expected output ends with `== done: 0 failure(s) ==` and `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt code/core/thread/thread_selftest.cpp code/core/thread/thread_registry.cpp code/core/thread/managed_thread.cpp
git commit -m "build: core_thread static lib + headless thread_selftest exe"
```

---

### Task 2: `ThreadRegistry` — register / snapshot / unregister (TDD)

**Files:**
- Create: `code/core/thread/thread_registry.hpp`
- Modify: `code/core/thread/thread_registry.cpp`
- Test: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Write the failing test**

In `thread_selftest.cpp`, add a function and call it from `main()` before the `done` line:
```cpp
#include "core/thread/thread_registry.hpp"

void test_registry_register_snapshot()
{
    auto& reg = ThreadRegistry::instance();
    const uint64_t id = reg.register_thread("worker-a", ThreadOverwatch::RecoveryPolicy::KillOnly);
    check(id != 0, "register returns non-zero id");

    auto snap = reg.snapshot();
    const auto it = std::ranges::find_if(snap, [&](const ThreadInfo& t) { return t.id == id; });
    check(it != snap.end(), "registered thread appears in snapshot");
    check(it != snap.end() && it->name == "worker-a", "snapshot has correct name");
    check(it != snap.end() && it->state == ThreadState::Starting, "initial state is Starting");

    reg.unregister_thread(id);
    snap = reg.snapshot();
    check(std::ranges::none_of(snap, [&](const ThreadInfo& t) { return t.id == id; }),
          "unregister removes from snapshot");
}
```
Add `test_registry_register_snapshot();` in `main()`. (`<ranges>` is in the PCH.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build --preset build-debug --target thread_selftest`
Expected: FAIL — `thread_registry.hpp` not found / `ThreadRegistry` undefined.

- [ ] **Step 3: Write the header**

`code/core/thread/thread_registry.hpp`:
```cpp
#pragma once
#include "pch.hpp"
#include "core/thread/thread_overwatch.hpp"

enum class ThreadState { Starting, Running, Stopping, Stopped, Restarting, Failed };

struct ThreadInfo {
    uint64_t                              id            = 0;
    std::string                           name;
    pid_t                                 tid           = 0;
    ThreadState                           state         = ThreadState::Starting;
    ThreadOverwatch::RecoveryPolicy       policy        = ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
    uint64_t                              iterations    = 0;
    uint64_t                              restart_count = 0;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point last_heartbeat{};
    uint64_t                              overwatch_watch_id = 0;
    std::string                           failed_reason;
    std::vector<std::pair<std::string, std::string>> status;
};

// Process-wide registry of every ManagedThread, for read-only introspection.
// Separate from ThreadOverwatch (which owns liveness/recovery); linked by watch id.
class ThreadRegistry {
public:
    static ThreadRegistry& instance();

    ThreadRegistry(const ThreadRegistry&)            = delete;
    ThreadRegistry& operator=(const ThreadRegistry&) = delete;

    uint64_t register_thread(std::string name, ThreadOverwatch::RecoveryPolicy policy);
    void     unregister_thread(uint64_t id);
    void     set_tid(uint64_t id, pid_t tid);
    void     note_iteration(uint64_t id);
    void     note_restart(uint64_t id);
    void     set_state(uint64_t id, ThreadState state);
    void     set_failed(uint64_t id, std::string reason);
    void     set_status(uint64_t id, std::string key, std::string value);
    void     set_watch_id(uint64_t id, uint64_t watch_id);

    std::vector<ThreadInfo> snapshot() const;

private:
    ThreadRegistry() = default;

    mutable std::shared_mutex               m_mutex;
    std::unordered_map<uint64_t, ThreadInfo> m_threads;
    std::atomic<uint64_t>                    m_next_id{1};
};
```

- [ ] **Step 4: Implement register/snapshot/unregister**

Replace the stub `code/core/thread/thread_registry.cpp` with:
```cpp
#include "pch.hpp"
#include "core/thread/thread_registry.hpp"

ThreadRegistry& ThreadRegistry::instance()
{
    static ThreadRegistry g_instance;
    return g_instance;
}

uint64_t ThreadRegistry::register_thread(std::string name, ThreadOverwatch::RecoveryPolicy policy)
{
    const uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    ThreadInfo info{};
    info.id         = id;
    info.name       = std::move(name);
    info.policy     = policy;
    info.state      = ThreadState::Starting;
    info.started_at = std::chrono::steady_clock::now();
    info.last_heartbeat = info.started_at;

    std::unique_lock lock(m_mutex);
    m_threads.emplace(id, std::move(info));
    return id;
}

void ThreadRegistry::unregister_thread(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    m_threads.erase(id);
}

std::vector<ThreadInfo> ThreadRegistry::snapshot() const
{
    std::shared_lock lock(m_mutex);
    std::vector<ThreadInfo> out;
    out.reserve(m_threads.size());
    for (const auto& [id, info] : m_threads)
        out.push_back(info);
    return out;
}

// The remaining mutators are implemented in Task 3.
```

- [ ] **Step 5: Run test to verify it passes**

Run:
```bash
cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"
```
Expected: the four `test_registry_*` checks print `[ok]`, `exit=0`.

- [ ] **Step 6: Commit**

```bash
git add code/core/thread/thread_registry.hpp code/core/thread/thread_registry.cpp code/core/thread/thread_selftest.cpp
git commit -m "feat: ThreadRegistry register/snapshot/unregister"
```

---

### Task 3: `ThreadRegistry` — mutators (TDD)

**Files:**
- Modify: `code/core/thread/thread_registry.cpp`
- Test: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Write the failing test**

Add to `thread_selftest.cpp` and call from `main()`:
```cpp
void test_registry_mutators()
{
    auto& reg = ThreadRegistry::instance();
    const uint64_t id = reg.register_thread("mut", ThreadOverwatch::RecoveryPolicy::RestartOnTimeout);

    reg.set_tid(id, 4242);
    reg.set_state(id, ThreadState::Running);
    reg.note_iteration(id);
    reg.note_iteration(id);
    reg.note_restart(id);
    reg.set_status(id, "job", "download");
    reg.set_status(id, "job", "encode");   // same key replaces
    reg.set_status(id, "pct", "42");
    reg.set_watch_id(id, 99);
    reg.set_failed(id, "boom");

    const auto snap = reg.snapshot();
    const auto it = std::ranges::find_if(snap, [&](const ThreadInfo& t) { return t.id == id; });
    check(it != snap.end(), "mutator thread present");
    if (it != snap.end()) {
        check(it->tid == 4242, "set_tid");
        check(it->iterations == 2, "note_iteration counts");
        check(it->restart_count == 1, "note_restart counts");
        check(it->overwatch_watch_id == 99, "set_watch_id");
        check(it->state == ThreadState::Failed, "set_failed sets Failed state");
        check(it->failed_reason == "boom", "set_failed records reason");
        check(it->status.size() == 2, "status replaces by key (job,pct)");
        const auto j = std::ranges::find_if(it->status, [](auto& kv) { return kv.first == "job"; });
        check(j != it->status.end() && j->second == "encode", "status value replaced");
    }
    reg.unregister_thread(id);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build --preset build-debug --target thread_selftest`
Expected: FAIL — `set_tid`/`note_iteration`/etc. unresolved (only declared, not defined).

- [ ] **Step 3: Implement the mutators**

Replace the trailing comment in `thread_registry.cpp` with:
```cpp
void ThreadRegistry::set_tid(uint64_t id, pid_t tid)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end())
        it->second.tid = tid;
}

void ThreadRegistry::note_iteration(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end()) {
        ++it->second.iterations;
        it->second.last_heartbeat = std::chrono::steady_clock::now();
    }
}

void ThreadRegistry::note_restart(uint64_t id)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end()) {
        ++it->second.restart_count;
        it->second.state = ThreadState::Restarting;
    }
}

void ThreadRegistry::set_state(uint64_t id, ThreadState state)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end())
        it->second.state = state;
}

void ThreadRegistry::set_failed(uint64_t id, std::string reason)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end()) {
        it->second.state         = ThreadState::Failed;
        it->second.failed_reason = std::move(reason);
    }
}

void ThreadRegistry::set_status(uint64_t id, std::string key, std::string value)
{
    std::unique_lock lock(m_mutex);
    auto it = m_threads.find(id);
    if (it == m_threads.end())
        return;
    auto& status = it->second.status;
    const auto kv = std::ranges::find_if(status, [&](auto& p) { return p.first == key; });
    if (kv != status.end())
        kv->second = std::move(value);
    else
        status.emplace_back(std::move(key), std::move(value));
}

void ThreadRegistry::set_watch_id(uint64_t id, uint64_t watch_id)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_threads.find(id); it != m_threads.end())
        it->second.overwatch_watch_id = watch_id;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: all `test_registry_mutators` checks `[ok]`, `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add code/core/thread/thread_registry.cpp code/core/thread/thread_selftest.cpp
git commit -m "feat: ThreadRegistry mutators (tid/iteration/restart/state/status/watch)"
```

---

### Task 4: `ManagedThread` — construction, naming, loop, iteration counting (TDD)

This task builds the wrapper with Overwatch **disabled** (`watch=false`) so the test is deterministic. Overwatch + restart are added in Task 6.

**Files:**
- Create: `code/core/thread/managed_thread.hpp`
- Modify: `code/core/thread/managed_thread.cpp`
- Test: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Write the failing test**

Add to `thread_selftest.cpp` and call from `main()`:
```cpp
#include "core/thread/managed_thread.hpp"

void test_managed_thread_basic()
{
    std::atomic<int> ticks{0};
    uint64_t reg_id = 0;

    {
        ManagedThread::Config cfg;
        cfg.name  = "selftest-basic";
        cfg.watch = false;          // no Overwatch in this test
        ManagedThread mt(cfg, [&](const std::stop_token& st, ManagedThread& self) {
            if (ticks.fetch_add(1) == 0) {
                char buf[32] = {};
                pthread_getname_np(pthread_self(), buf, sizeof(buf));
                self.set_status("osname", buf);   // publish the real OS thread name
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            (void)st;
        });

        // Let it spin a bit, then inspect the registry.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto snap = ThreadRegistry::instance().snapshot();
        const auto it = std::ranges::find_if(snap, [](const ThreadInfo& t) { return t.name == "selftest-basic"; });
        check(it != snap.end(), "managed thread is registered");
        if (it != snap.end()) {
            reg_id = it->id;
            check(it->state == ThreadState::Running, "state Running while looping");
            check(it->iterations >= 1, "iterations advance");
            check(it->tid != 0, "tid published");
            const auto os = std::ranges::find_if(it->status, [](auto& kv) { return kv.first == "osname"; });
            check(os != it->status.end() && os->second == "selftest-basic", "OS thread name set via pthread");
        }
    } // ManagedThread destructor: request_stop + join + unregister

    const auto after = ThreadRegistry::instance().snapshot();
    check(std::ranges::none_of(after, [&](const ThreadInfo& t) { return t.id == reg_id; }),
          "destructor unregisters the thread");
    check(ticks.load() > 0, "loop body ran");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build --preset build-debug --target thread_selftest`
Expected: FAIL — `managed_thread.hpp` not found.

- [ ] **Step 3: Write the header**

`code/core/thread/managed_thread.hpp`:
```cpp
#pragma once
#include "pch.hpp"
#include "core/thread/thread_overwatch.hpp"

// RAII wrapper around std::jthread. Owns the loop, names the OS thread, registers
// with ThreadOverwatch (watchdog) and ThreadRegistry (introspection), and enforces
// a recovery policy with a restart-storm guard.
//
// Contract: the body runs once per iteration and MUST return within `timeout` even
// when idle. Condvar workers wait with `cv.wait_for(lk, timeout/2, pred)`, never an
// unbounded wait — the watch lives for the whole thread lifetime.
class ManagedThread {
public:
    // Repeated body. `self` lets the body publish status or stop itself (one-shots
    // call self.request_stop()).
    using IterationFn = std::function<void(const std::stop_token&, ManagedThread&)>;

    struct Config {
        std::string                     name;                          // truncated to 15 chars
        std::chrono::milliseconds       timeout{5000};
        ThreadOverwatch::RecoveryPolicy policy = ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
        bool                            watch  = true;
        uint32_t                        max_consecutive_restarts = 5;  // RestartOnTimeout only
        std::chrono::milliseconds       restart_reset_window{60'000};
    };

    ManagedThread(Config cfg, IterationFn body);
    ~ManagedThread();

    ManagedThread(const ManagedThread&)            = delete;
    ManagedThread& operator=(const ManagedThread&) = delete;

    void request_stop();
    bool joinable() const;
    void set_status(std::string key, std::string value);

private:
    void start_thread_only();   // spawn a fresh jthread running run()
    void run(std::stop_token st);
    void respawn();             // Overwatch restart callback (monitor thread)
    void escalate();            // restart-storm: log + abort

    static std::string truncate_name(std::string_view name);

    Config        m_cfg;
    std::string   m_name;        // truncated, <= 15 chars
    IterationFn   m_body;
    uint64_t      m_registry_id = 0;
    uint64_t      m_watch_id    = 0;
    std::jthread  m_thread;

    std::mutex                            m_respawn_mutex;
    bool                                  m_shutting_down       = false;
    uint32_t                              m_consecutive_restarts = 0;
    std::chrono::steady_clock::time_point m_last_restart{};
};
```

- [ ] **Step 4: Implement construction, naming, loop (no Overwatch yet)**

Replace the stub `code/core/thread/managed_thread.cpp` with:
```cpp
#include "pch.hpp"
#include "core/thread/managed_thread.hpp"
#include "core/thread/thread_registry.hpp"
#include "core/log/debug_log.hpp"

namespace { constexpr std::size_t k_max_thread_name = 15; }  // Linux pthread limit

std::string ManagedThread::truncate_name(std::string_view name)
{
    if (name.size() <= k_max_thread_name)
        return std::string(name);
    APP_DEBUG_LOG("[ManagedThread] name '{}' exceeds {} chars; truncating", name, k_max_thread_name);
    return std::string(name.substr(0, k_max_thread_name));
}

ManagedThread::ManagedThread(Config cfg, IterationFn body)
    : m_cfg(std::move(cfg))
    , m_name(truncate_name(m_cfg.name))
    , m_body(std::move(body))
{
    m_registry_id = ThreadRegistry::instance().register_thread(m_name, m_cfg.policy);
    start_thread_only();
    // Overwatch registration is added in Task 6 (guarded by m_cfg.watch).
}

ManagedThread::~ManagedThread()
{
    {
        std::lock_guard lk(m_respawn_mutex);
        m_shutting_down = true;
    }
    request_stop();
    if (m_thread.joinable())
        m_thread.join();
    ThreadRegistry::instance().unregister_thread(m_registry_id);
}

void ManagedThread::request_stop()
{
    m_thread.request_stop();
}

bool ManagedThread::joinable() const
{
    return m_thread.joinable();
}

void ManagedThread::set_status(std::string key, std::string value)
{
    ThreadRegistry::instance().set_status(m_registry_id, std::move(key), std::move(value));
}

void ManagedThread::start_thread_only()
{
    m_thread = std::jthread{[this](std::stop_token st) { run(std::move(st)); }};
}

void ManagedThread::run(std::stop_token st)
{
    pthread_setname_np(pthread_self(), m_name.c_str());
    ThreadRegistry::instance().set_tid(m_registry_id, ::gettid());
    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Running);

    while (!st.stop_requested()) {
        ThreadRegistry::instance().note_iteration(m_registry_id);
        try {
            m_body(st, *this);
        } catch (const std::exception& e) {
            APP_DEBUG_LOG("[ManagedThread] {} body threw: {}", m_name, e.what());
        }
    }
    ThreadRegistry::instance().set_state(m_registry_id, ThreadState::Stopped);
}

// respawn()/escalate() are added in Task 6.
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: `test_managed_thread_basic` checks `[ok]`, `exit=0`.

- [ ] **Step 6: Commit**

```bash
git add code/core/thread/managed_thread.hpp code/core/thread/managed_thread.cpp code/core/thread/thread_selftest.cpp
git commit -m "feat: ManagedThread construction, naming, loop, iteration counting"
```

---

### Task 5: `ManagedThread` — name truncation to 15 chars (TDD)

**Files:**
- Modify: `code/core/thread/managed_thread.cpp` (only if test fails)
- Test: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Write the failing test**

Add to `thread_selftest.cpp` and call from `main()`:
```cpp
void test_managed_thread_name_truncation()
{
    std::atomic<bool> ran{false};
    ManagedThread::Config cfg;
    cfg.name  = "ThisNameIsWayTooLongForLinux";   // 28 chars
    cfg.watch = false;
    ManagedThread mt(cfg, [&](const std::stop_token&, ManagedThread& self) {
        if (!ran.exchange(true)) {
            char buf[32] = {};
            pthread_getname_np(pthread_self(), buf, sizeof(buf));
            self.set_status("osname", buf);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto snap = ThreadRegistry::instance().snapshot();
    const auto it = std::ranges::find_if(snap, [](const ThreadInfo& t) { return t.name == "ThisNameIsWayT"; });
    // "ThisNameIsWayTooLongForLinux" truncated to 15 chars = "ThisNameIsWayTo"
    const auto it15 = std::ranges::find_if(snap, [](const ThreadInfo& t) { return t.name.size() == 15; });
    check(it15 != snap.end() && it15->name == "ThisNameIsWayTo", "registry name truncated to 15 chars");
    if (it15 != snap.end()) {
        const auto os = std::ranges::find_if(it15->status, [](auto& kv) { return kv.first == "osname"; });
        check(os != it15->status.end() && os->second == "ThisNameIsWayTo", "OS name truncated to 15 chars");
    }
    (void)it;
}
```

- [ ] **Step 2: Run test to verify it passes immediately**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: PASS — `truncate_name` from Task 4 already enforces 15 chars. (This task is a guard test; if it fails, fix `truncate_name`/`k_max_thread_name`.)

- [ ] **Step 3: Commit**

```bash
git add code/core/thread/thread_selftest.cpp
git commit -m "test: ManagedThread 15-char name truncation"
```

---

### Task 6: `ManagedThread` — Overwatch integration, respawn, restart-storm guard (TDD)

**Files:**
- Modify: `code/core/thread/managed_thread.cpp`
- Test: `code/core/thread/thread_selftest.cpp`

- [ ] **Step 1: Write the failing test (restart counting)**

The watchdog polls every 500 ms (`thread_overwatch.cpp:143`). Use a 300 ms timeout and a body that hangs exactly once, then runs fast — so it restarts once and recovers (never reaching `max_consecutive_restarts`). Add to `thread_selftest.cpp`, call from `main()`:
```cpp
void test_managed_thread_restart_once()
{
    std::atomic<int> iteration{0};
    ManagedThread::Config cfg;
    cfg.name    = "selftest-rst";
    cfg.timeout = std::chrono::milliseconds(300);
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::RestartOnTimeout;
    cfg.watch   = true;
    uint64_t reg_id = 0;

    {
        ManagedThread mt(cfg, [&](const std::stop_token& st, ManagedThread&) {
            const int n = iteration.fetch_add(1);
            if (n == 0) {
                // First iteration of the FIRST thread hangs long enough to trip the
                // 300 ms watchdog (which detects on its next <=500 ms poll).
                for (int i = 0; i < 20 && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); // healthy
            }
        });

        // Wait long enough for: hang detect (~0.8 s) + respawn + a few healthy iters.
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        const auto snap = ThreadRegistry::instance().snapshot();
        const auto it = std::ranges::find_if(snap, [](const ThreadInfo& t) { return t.name == "selftest-rst"; });
        check(it != snap.end(), "restart thread present");
        if (it != snap.end()) {
            reg_id = it->id;
            check(it->restart_count >= 1, "restart_count incremented after watchdog fired");
            check(it->overwatch_watch_id != 0, "watch id recorded");
            check(it->state == ThreadState::Running, "recovered to Running");
        }
    }
    check(std::ranges::none_of(ThreadRegistry::instance().snapshot(),
          [&](const ThreadInfo& t) { return t.id == reg_id; }), "restart thread unregistered on destroy");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: FAIL — `restart_count` stays 0 because Overwatch isn't wired yet (Task 4 left `watch` unused).

- [ ] **Step 3: Wire Overwatch into construction**

In `managed_thread.cpp`, replace `start_thread_only();` inside the constructor with:
```cpp
    start_thread_only();
    if (m_cfg.watch) {
        m_watch_id = ThreadOverwatch::instance().watch(
            m_name, m_cfg.timeout,
            /*kill*/    [this] { request_stop(); },
            /*restart*/ [this] { respawn(); },
            m_cfg.policy);
        ThreadRegistry::instance().set_watch_id(m_registry_id, m_watch_id);
    }
```

In `run()`, add a heartbeat at the top of the loop body (before `note_iteration`):
```cpp
    while (!st.stop_requested()) {
        if (m_cfg.watch)
            ThreadOverwatch::instance().heartbeat(m_watch_id);
        ThreadRegistry::instance().note_iteration(m_registry_id);
```

In the destructor, unwatch **before** join so the monitor can't fire mid-teardown. Change the destructor body to:
```cpp
ManagedThread::~ManagedThread()
{
    {
        std::lock_guard lk(m_respawn_mutex);
        m_shutting_down = true;
    }
    if (m_cfg.watch)
        ThreadOverwatch::instance().unwatch(m_watch_id);
    request_stop();
    if (m_thread.joinable())
        m_thread.join();
    ThreadRegistry::instance().unregister_thread(m_registry_id);
}
```

- [ ] **Step 4: Implement respawn() and escalate()**

Replace the `// respawn()/escalate() are added in Task 6.` comment with:
```cpp
void ManagedThread::respawn()   // runs on the Overwatch monitor thread
{
    std::lock_guard lk(m_respawn_mutex);
    if (m_shutting_down)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_restart >= m_cfg.restart_reset_window)
        m_consecutive_restarts = 0;           // streak broken — thread had recovered
    ++m_consecutive_restarts;
    m_last_restart = now;

    if (m_consecutive_restarts > m_cfg.max_consecutive_restarts) {
        escalate();
        return;                               // do NOT recreate the thread
    }

    ThreadRegistry::instance().note_restart(m_registry_id);  // ++restart_count, state=Restarting
    m_thread.request_stop();
    if (m_thread.joinable())
        m_thread.join();                      // drains a cooperative thread; a wedged one leaks
    start_thread_only();                      // watch + registry entry persist
}

void ManagedThread::escalate()
{
    const std::string reason = std::format(
        "restart storm: {} consecutive restarts within {} ms",
        m_consecutive_restarts, m_cfg.restart_reset_window.count());
    std::println(stderr, "[ManagedThread] FATAL {}: {}", m_name, reason);
    if (m_cfg.watch)
        ThreadOverwatch::instance().unwatch(m_watch_id);
    ThreadRegistry::instance().set_failed(m_registry_id, reason);
    std::println(stderr, "[ManagedThread] stacktrace:\n{}",
                 std::to_string(std::stacktrace::current()));
    std::fflush(stderr);
    std::abort();
}
```

> `std::println(stderr, ...)` (not `APP_DEBUG_LOG`) so the fatal path is visible in Release too. `std::stacktrace` links via `stdc++exp` (added in Task 1).

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: `test_managed_thread_restart_once` checks `[ok]`, `exit=0`. (Runtime ~3 s due to watchdog timing.)

- [ ] **Step 6: Commit**

```bash
git add code/core/thread/managed_thread.cpp code/core/thread/thread_selftest.cpp
git commit -m "feat: ManagedThread Overwatch integration + restart-storm guard"
```

---

### Task 7: `ThreadReflectionPanel` — ImGui debug window

No automated test (ImGui requires a live context); verified by compiling the app and manual inspection.

**Files:**
- Create: `code/ui/window/widgets/thread_reflection_panel.hpp`
- Create: `code/ui/window/widgets/thread_reflection_panel.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the header**

`code/ui/window/widgets/thread_reflection_panel.hpp`:
```cpp
#pragma once
#include "pch.hpp"

// Live, read-only ImGui table of every ManagedThread (from ThreadRegistry::snapshot()).
class ThreadReflectionPanel {
public:
    void draw(bool* open);
};
```

- [ ] **Step 2: Write the implementation**

`code/ui/window/widgets/thread_reflection_panel.cpp`:
```cpp
#include "pch.hpp"
#include "ui/window/widgets/thread_reflection_panel.hpp"
#include "core/thread/thread_registry.hpp"

namespace {

const char* state_name(ThreadState s)
{
    switch (s) {
        case ThreadState::Starting:   return "Starting";
        case ThreadState::Running:    return "Running";
        case ThreadState::Stopping:   return "Stopping";
        case ThreadState::Stopped:    return "Stopped";
        case ThreadState::Restarting: return "Restarting";
        case ThreadState::Failed:     return "Failed";
    }
    return "?";
}

const char* policy_name(ThreadOverwatch::RecoveryPolicy p)
{
    return p == ThreadOverwatch::RecoveryPolicy::KillOnly ? "KillOnly" : "Restart";
}

std::string join_status(const std::vector<std::pair<std::string, std::string>>& kv)
{
    std::string out;
    for (const auto& [k, v] : kv) {
        if (!out.empty())
            out += "  ";
        out += std::format("{}={}", k, v);
    }
    return out;
}

} // namespace

void ThreadReflectionPanel::draw(bool* open)
{
    if (open != nullptr && !*open)
        return;
    if (!ImGui::Begin("Threads", open)) {
        ImGui::End();
        return;
    }

    auto snap = ThreadRegistry::instance().snapshot();
    std::ranges::sort(snap, [](const ThreadInfo& a, const ThreadInfo& b) { return a.name < b.name; });

    const auto now = std::chrono::steady_clock::now();
    ImGui::Text("Managed threads: %zu", snap.size());

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("threads_table", 9, flags)) {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("tid");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("policy");
        ImGui::TableSetupColumn("iters");
        ImGui::TableSetupColumn("restarts");
        ImGui::TableSetupColumn("HB age (ms)");
        ImGui::TableSetupColumn("watch");
        ImGui::TableSetupColumn("status");
        ImGui::TableHeadersRow();

        for (const auto& t : snap) {
            ImGui::TableNextRow();
            if (t.state == ThreadState::Failed)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 30, 30, 160));
            else if (t.state == ThreadState::Restarting)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 90, 20, 140));

            const auto hb_age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - t.last_heartbeat).count();

            ImGui::TableNextColumn(); ImGui::TextUnformatted(t.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(t.tid));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(state_name(t.state));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(policy_name(t.policy));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(t.iterations));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(t.restart_count));
            ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(hb_age));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(t.overwatch_watch_id));
            ImGui::TableNextColumn();
            if (t.state == ThreadState::Failed && !t.failed_reason.empty())
                ImGui::TextUnformatted(t.failed_reason.c_str());
            else
                ImGui::TextUnformatted(join_status(t.status).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
```

> **Confirm the include path** matches how sibling widgets include their own headers
> (the project compiles with `-Icode`, so `ui/window/widgets/...` should resolve). If
> `fps_plot.cpp` uses a bare `#include "fps_plot.hpp"` instead, follow that convention.

- [ ] **Step 3: Add to CMakeLists.txt**

In `CMakeLists.txt`, add to the `add_executable(example_sdl3_vulkan ...)` source list, next to the other `code/ui/window/widgets/*.cpp` entries:
```cmake
    code/ui/window/widgets/thread_reflection_panel.cpp
```

- [ ] **Step 4: Build the app to verify it compiles**

Run: `cmake --build --preset build-debug --target example_sdl3_vulkan`
Expected: compiles and links (no run needed yet).

- [ ] **Step 5: Commit**

```bash
git add code/ui/window/widgets/thread_reflection_panel.hpp code/ui/window/widgets/thread_reflection_panel.cpp CMakeLists.txt
git commit -m "feat: ThreadReflectionPanel ImGui debug window"
```

---

### Task 8: Wire the panel into `app.cpp`

**Files:**
- Modify: `code/main/app.cpp`

- [ ] **Step 1: Include the panel and instantiate it**

In `code/main/app.cpp`, add near the other widget includes (`#include "fps_plot.hpp"` ~line 7):
```cpp
#include "thread_reflection_panel.hpp"
```
> If that include doesn't resolve, the widgets dir may not be on the include path the same way; use `#include "ui/window/widgets/thread_reflection_panel.hpp"`. Confirm against how `fps_plot.hpp` is included.

Near `FpsPlot fps_plot;` (~line 58) add:
```cpp
ThreadReflectionPanel thread_panel;
bool show_thread_panel = false;
```

- [ ] **Step 2: Draw it each frame**

Near the per-frame `fps_plot.draw(uptime_seconds);` (~line 203) add:
```cpp
        thread_panel.draw(&show_thread_panel);
```

- [ ] **Step 3: Add the debug-menu toggle**

Near the existing debug checkboxes (`ImGui::Checkbox("Style Editor", &style_editor.IsOpen);` ~line 247) add:
```cpp
            ImGui::Checkbox("Threads", &show_thread_panel);
```

- [ ] **Step 4: Build the app**

Run: `cmake --build --preset build-debug --target example_sdl3_vulkan`
Expected: compiles and links.

- [ ] **Step 5: Commit**

```bash
git add code/main/app.cpp
git commit -m "feat: wire ThreadReflectionPanel into app debug menu"
```

---

### Task 9: `THREADTEST` console command (in-app smoke test)

Spawns a short-lived `ManagedThread` from the console so the panel can be seen populating live, and confirms `ManagedThread` works inside the full app (not just the headless exe).

**Files:**
- Modify: wherever console commands are registered (find with `grep -rn "RegisterCommand(" code/ | grep -v imgui_console.cpp`).
- Likely: `code/ui/console/` command-registration site.

- [ ] **Step 1: Find the registration site**

Run: `grep -rn "RegisterCommand(" code/ | grep -v "void ImGuiConsole::RegisterCommand"`
Identify the file/function where app commands (e.g. `BASH`, `HELP`) are registered. Add the new command there.

- [ ] **Step 2: Register the command**

At the registration site, add (adjust the surrounding `console.` / `RegisterCommand` receiver to match the existing calls):
```cpp
#include "core/thread/managed_thread.hpp"
// ...
RegisterCommand("THREADTEST", "Spawn a 10s ManagedThread visible in the Threads panel",
    [](ImGuiConsole& console, const ConsoleCommandArgs&) {
        // Leak intentionally: a detached, self-stopping demo thread. It stops itself
        // after ~10s of iterations, then unregisters when the unique_ptr is dropped
        // by the body's final iteration via a shared owner.
        auto owner = std::make_shared<std::unique_ptr<ManagedThread>>();
        ManagedThread::Config cfg;
        cfg.name  = "ThreadTestDemo";
        cfg.watch = true;
        cfg.policy = ThreadOverwatch::RecoveryPolicy::KillOnly;
        *owner = std::make_unique<ManagedThread>(cfg,
            [owner, start = std::chrono::steady_clock::now()]
            (const std::stop_token&, ManagedThread& self) {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                self.set_status("elapsed_s",
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()));
                if (elapsed > std::chrono::seconds(10)) {
                    self.request_stop();
                    // Drop our owning reference on a separate thread to avoid joining self.
                    std::thread([owner] { owner->reset(); }).detach();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            });
        console.AddLog("[THREADTEST] spawned 'ThreadTestDemo' — open the Threads panel\n");
    });
```
> The `owner` shared_ptr keeps the `ManagedThread` alive; when the body decides to finish it detaches a tiny thread to `reset()` the owner (destroying the `ManagedThread` off its own worker thread, which is required because a thread cannot join itself).

- [ ] **Step 3: Build the app**

Run: `cmake --build --preset build-debug --target example_sdl3_vulkan`
Expected: compiles and links.

- [ ] **Step 4: Manual verification**

Run the app (`./build/debug/example_sdl3_vulkan` or via the VS Code lldb-dap launch). In the console type `THREADTEST`, open the **Threads** panel from the debug menu, and confirm:
- A `ThreadTestDemo` row appears, `state=Running`, `policy=KillOnly`, `iters` climbing, `status` showing `elapsed_s=…`.
- The name is exactly `ThreadTestDemo` (≤15 chars).
- After ~10 s the row disappears (thread stopped + unregistered).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: THREADTEST console command exercising ManagedThread in-app"
```

---

### Task 10: Final infrastructure verification

- [ ] **Step 1: Full headless test pass**

Run: `cmake --build --preset build-debug --target thread_selftest && ./build/debug/thread_selftest; echo "exit=$?"`
Expected: `== done: 0 failure(s) ==`, `exit=0`.

- [ ] **Step 2: Release build compiles (stacktrace link + APP_DEBUG_LOG off path)**

Run: `cmake --build --preset build-release --target example_sdl3_vulkan`
Expected: links cleanly — confirms `stdc++exp` resolves `std::stacktrace` in Release and the `std::println(stderr,...)` fatal path compiles when `APP_DEBUG_LOG` is a no-op.

- [ ] **Step 3: Manual panel + selftest in-app**

Launch the app, run `THREADTEST`, verify the Threads panel as in Task 9 Step 4.

- [ ] **Step 4: Commit any fixes, then summarize**

If steps surfaced fixes, commit them. The infrastructure is complete: `ManagedThread`, `ThreadRegistry`, `ThreadReflectionPanel`, headless tests, and in-app smoke test all working.

---

## Phase 2 — Consumer migration (separate plan)

**Not part of this plan.** After the infrastructure above merges and `ManagedThread` is a compiled, real API, write a second plan via `superpowers:writing-plans` that migrates the six hand-rolled threads — one self-contained, independently-verifiable task each. Writing this now would mean coding against an API that doesn't yet exist; each consumer should be read in full and converted against the real type.

Each migration converts a `for(;;){ wait_for(...); dequeue; work; post; }` worker into a `ManagedThread` member whose `IterationFn` is one iteration of that loop (the wrapper supplies naming + heartbeat). Per the spec table:

| Consumer (`file`) | Name (≤15) | Policy | Body shape | Migration notes |
|---|---|---|---|---|
| `video_downloader.cpp` | `VideoDownloader` | KillOnly | bounded-wait queue worker | drop manual `watch`/`heartbeat`/`pthread_setname_np`; body = one loop iter; `wait_for(timeout/2)` |
| `history_preview.cpp` | `HistoryPreview` | Restart | bounded-wait queue worker | already restart-on-recovery; same conversion |
| `video_hover_preview.cpp` | `VidHoverPrev` | KillOnly | bounded-wait worker | |
| `video_seek_preview.cpp` | `VidSeekPrev` | KillOnly | bounded-wait worker | |
| `bulk_image_open_queue.cpp` | `BulkImageOpen` | KillOnly | one-shot | body does the whole job, then `self.request_stop()` |
| `file_browser_thread.cpp` | `FileBrowserScan` | KillOnly | bounded-wait + per-entry status | **fixes the `watch_id`-as-`pthread_t` bug at line 116**; per-entry progress via `self.set_status` |

Migration acceptance: each file builds, the thread appears in the Threads panel with the correct ≤15-char name and advancing iterations, and the feature it powers (downloads, previews, file browsing) still works.
