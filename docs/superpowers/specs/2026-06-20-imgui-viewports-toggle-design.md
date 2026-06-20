# Toggleable ImGui Multi-Viewports — Design

**Date:** 2026-06-20
**Branch:** refactor/registry-owned-subsystems
**Status:** Approved design, pending implementation plan

## Goal

Let dockable ImGui windows tear out into their own native OS windows
(ImGui "multi-viewport" mode), controlled two ways:

1. **Compile-time gate** — a CMake option decides whether the feature is built
   in at all.
2. **Runtime toggle** — when built in, a "Multi-Viewport" checkbox in the View
   menu flips it live, and the choice persists across restarts.

Today the app enables only docking
([imgui_context.cpp:118-120](../../../code/rendering/imgui_context.cpp#L118));
viewports are not wired at all.

## Why this is feasible as a live toggle

`ImGuiConfigFlags_ViewportsEnable` is one of the few ImGui config flags that is
safe to flip at runtime, but only if the platform/renderer backends have
registered their multi-viewport hooks. In the vendored
`external/imgui-1.92.8-docking` backends, those hooks are installed at **init
time, independent of the config flag**:

- SDL3 platform: gated on `ImGuiBackendFlags_PlatformHasViewports` (a
  backend-set capability flag, not the user config flag) at
  `imgui_impl_sdl3.cpp:653-654`.
- Vulkan renderer: `ImGui_ImplVulkan_InitMultiViewportSupport()` called
  unconditionally at `imgui_impl_vulkan.cpp:1430`.

Because the hooks always exist after init, toggling the config flag mid-run is
fully supported — no context or backend rebuild. The flag alone is inert,
though: the render loop must also pump the platform windows each frame.

## Architecture / wiring points

### 1. Compile-time gate

- Add CMake option `APP_ENABLE_IMGUI_VIEWPORTS`, default `ON`.
- When ON, add compile definition `APP_ENABLE_IMGUI_VIEWPORTS=1` to the app
  target; when OFF, `=0`.
- All new viewport code (state load/save, reconcile method, render-loop pump,
  menu item) is wrapped in `#if APP_ENABLE_IMGUI_VIEWPORTS`. With the option
  OFF the feature compiles out entirely — no menu entry, no runtime cost.

### 2. Persisted + runtime state

- `WindowStateToml` ([window_state_toml.hpp](../../../code/ui/window/persistence/window_state_toml.hpp#L141)):
  add `bool viewports_enabled = false;` next to `vsync`. Serialization is
  automatic via `rfl::toml` reflection — no parse/write code needed. Add a
  matching `FieldCommentFor("viewports_enabled")` entry for the annotated TOML
  comment.
- `AppRuntimeState` ([app_runtime_state.hpp](../../../code/main/app_runtime_state.hpp#L43)):
  add `bool viewportsEnabled = false;` beside `vsync`.
- Load in `App::KickStart` beside line 183:
  `m_Rt->viewportsEnabled = m_State->viewports_enabled;`
- Save in `App::destroy` beside line 454:
  `m_State->viewports_enabled = m_Rt->viewportsEnabled;`

### 3. Runtime control (View menu)

- Add a "Multi-Viewport" checkbox to the View menu
  ([main_menu_bar.cpp:137](../../../code/ui/window/widgets/main_menu_bar.cpp#L137)),
  following the existing `bool*` pattern used by "File Explorer"
  (`MenuContext::show_file_explorer`). It only flips
  `m_Rt->viewportsEnabled`; it does not touch ImGui state directly.
- Wrap the menu item in `#if APP_ENABLE_IMGUI_VIEWPORTS` so it disappears when
  the feature is gated out.

### 4. Reconcile — the core logic unit

A new `imgui_context` method (compiled only under the macro), e.g.:

```cpp
void imgui_context::set_viewports_enabled(bool enabled);
```

Responsibilities:

- Edge-detect against the last applied state (member `bool m_viewports_applied`)
  so the style save/restore only runs on a genuine on→off / off→on transition,
  never every frame.
- On **enable**: set the `ImGuiConfigFlags_ViewportsEnable` bit; save the prior
  `style.WindowRounding` and `style.Colors[ImGuiCol_WindowBg].w`; then apply
  ImGui's recommendation (`WindowRounding = 0`, `WindowBg.w = 1.0f`) so torn-out
  OS windows render correctly.
- On **disable**: clear the flag bit; restore the saved `WindowRounding` /
  `WindowBg.w`.

Called once per frame from the app loop (after `new_frame()`, before the
dockspace) with `m_Rt->viewportsEnabled`; the edge detection makes
per-frame calls cheap and idempotent.

> Style note: applying the recommended fix overrides the live StyleEditor's
> rounding/opacity *while viewports are enabled*. This is the accepted
> trade-off (chosen over leaving style untouched) so native windows look
> correct; the saved values are restored when viewports turn back off.

### 5. Render-loop pump

In `imgui_context::render`
([imgui_context.cpp:333-343](../../../code/rendering/imgui_context.cpp#L333)),
insert between `frame_render` and `frame_present`, matching the official SDL3 +
Vulkan example ordering (called outside the minimized guard):

```cpp
#if APP_ENABLE_IMGUI_VIEWPORTS
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
#endif
```

## Data flow

```
TOML (viewports_enabled)
  └─ load → AppRuntimeState::viewportsEnabled
              ├─ View-menu checkbox writes it
              └─ each frame → imgui_context::set_viewports_enabled()
                                ├─ syncs io.ConfigFlags bit
                                └─ saves/applies/restores style on transition
                          render() → if flag set: UpdatePlatformWindows()
                                                  + RenderPlatformWindowsDefault()
  └─ save ← AppRuntimeState::viewportsEnabled (on destroy)
```

## Components touched

| File | Change |
|------|--------|
| `CMakeLists.txt` | `option(APP_ENABLE_IMGUI_VIEWPORTS ... ON)` + compile definition |
| `code/ui/window/persistence/window_state_toml.hpp` | `bool viewports_enabled = false;` |
| `code/ui/window/persistence/window_state_toml.cpp` | `FieldCommentFor` entry |
| `code/main/app_runtime_state.hpp` | `bool viewportsEnabled = false;` |
| `code/main/app.cpp` | load/save wiring; per-frame reconcile call |
| `code/rendering/imgui_context.hpp/.cpp` | `set_viewports_enabled()`, `m_viewports_applied`, render-loop pump |
| `code/ui/window/widgets/main_menu_bar.hpp/.cpp` | `MenuContext` bool* + View-menu checkbox |

## Error handling / edge cases

- **Reconcile idempotency:** edge detection prevents clobbering StyleEditor
  every frame and prevents repeated flag writes.
- **Minimized window:** the platform-window pump runs outside the
  `is_minimized` guard (matching the official example); secondary windows may
  still need updating while the main window is minimized.
- **Feature gated OFF:** with `APP_ENABLE_IMGUI_VIEWPORTS=0`, a persisted
  `viewports_enabled = true` in TOML is simply ignored (no code reads it), and
  the menu item is absent — no broken state.
- **Style restore on shutdown:** not required — style is rebuilt from TOML each
  launch; the in-memory restore matters only for the live on→off transition.

## Testing

- Manual: launch with option ON, toggle the menu item, drag a docked window
  outside the main OS window; confirm it becomes a native window and docks back
  when toggled off.
- Persistence: enable, quit, relaunch → state restored from TOML.
- Compile gate: build with `-DAPP_ENABLE_IMGUI_VIEWPORTS=OFF` → builds clean,
  menu item absent, no viewport behavior.
- Style: confirm rounding/opacity restore correctly on disable.

## Out of scope (YAGNI)

- Per-window viewport opt-out flags.
- Custom secondary-viewport render passes / DPI-per-monitor tuning beyond
  ImGui defaults.
- Exposing the compile option through any runtime UI.
