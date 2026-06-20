---
name: imanim
description: >
  Add motion to Dear ImGui UI in this project with the ImAnim library
  (external/ImAnim, header+source pair im_anim.{h,cpp}). Covers the
  immediate-mode tween API (iam_tween_float/vec2/vec4/int/color), easing,
  the clip/keyframe timeline system, the mandatory per-frame pump
  (iam_update_begin_frame), and how ImAnim is built into the app as the
  `imanim` static lib. Use this whenever a widget should grow, shrink, fade,
  slide, spring, pulse, or smoothly cross-fade on hover / focus / open / a
  value change — and whenever you see iam_* calls, ImHashStr channel keys, or
  touch code that animates an ImGui control. Reach for this even when the user
  just says "animate it", "make it smooth", "ease it in", or "on hover it
  should…" without naming ImAnim.
---

# ImAnim — animating ImGui widgets in this project

`external/ImAnim/` is a header+source animation library for Dear ImGui
(`im_anim.h` + `im_anim.cpp`; the `_demo/_doc/_usecase` files are examples,
not built). It gives you **tweens** (smooth value
interpolation reacting to UI state) and **clips** (authored keyframe timelines).
For 95% of UI work — hover grows, fade-ins, value cross-fades — you want a
**tween**, not a clip.

The library's own one-screen summary lives at the **bottom of `im_anim.h`**
("Usage notes (summary)"). The header is the API source of truth; `docs/` has
topic guides (`tweens.md`, `clips.md`, `easing.md`, `motion-paths.md`, …).

## The mental model (why tweens fit ImGui)

ImGui redraws everything every frame, so ImAnim tweens are designed to be
**called unconditionally every frame with a *target*** — the library remembers
the current animated value, keyed by `(owner ImGuiID, channel ImGuiID)`, and
eases it toward whatever target you pass. There is no "start animation" call and
no state machine to own:

```cpp
// Grows a value from 0.5 -> 1.0 over 0.3s while hovered, and back when not.
float alpha = iam_tween_float(
    ImGui::GetItemID(),                 // owner: who is animating (stable per widget)
    ImHashStr("alpha"),                 // channel: which property of that owner
    hovered ? 1.0f : 0.5f,              // TARGET this frame (drives the motion)
    0.3f,                               // duration to reach a new target
    iam_ease_preset(iam_ease_out_cubic),
    iam_policy_crossfade,               // smoothly redirect if target changes mid-flight
    ImGui::GetIO().DeltaTime);          // dt
```

Flip the target and the value animates there on its own. That is the whole
pattern — pick a stable id, name a channel, pass the target.

## Mandatory per-frame pump

Tweens only advance when the frame clock ticks. **Once per frame, right after
`ImGui::NewFrame()`**, call `iam_update_begin_frame()`. In this project that
lives in `imgui_context::new_frame()` (`code/rendering/imgui_context.cpp`):

```cpp
void imgui_context::new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    iam_update_begin_frame();           // ← pump tweens
    // iam_clip_update(ImGui::GetIO().DeltaTime); // ← ONLY if clips are used
}
```

Miss it and every animated value freezes at frame 0 (looks like "the animation
does nothing"). `iam_clip_update(dt)` is **only** needed if you author clips;
plain tweens don't need it.

## Build integration (mirrors `implot`)

ImAnim is compiled once into a static `imanim` lib, exactly like ImPlot. The
template is `thirdparty/implot/CMakeLists.txt`:

```cmake
# thirdparty/imanim/CMakeLists.txt
set(IMANIM_DIR ${CMAKE_SOURCE_DIR}/external/ImAnim)
add_library(imanim STATIC ${IMANIM_DIR}/im_anim.cpp)
target_include_directories(imanim SYSTEM PUBLIC ${IMANIM_DIR})  # im_anim.h pulls in imgui.h
target_link_libraries(imanim PUBLIC imgui)
target_compile_options(imanim PRIVATE -w)
app_profilable_treatment(imanim)
```

Then in the top `CMakeLists.txt`: `add_subdirectory(thirdparty/imanim)` next to
the other thirdparty libs, and add `imanim` to the app target's
`target_link_libraries` (alongside `implot`). The include arrives transitively.
For clangd, add `-I.../external/ImAnim` to the `CompileFlags: Add:` block in
`.clangd` so `#include "im_anim.h"` resolves in the editor.

> The first consumer is the thin mpv-style seek bar in
> `code/ui/media/video/widgets/video_ui_window.cpp`; the build wiring above
> lands with it. New animated widgets just `#include "im_anim.h"` and go.

## The keying model — get this right or animations bleed

`(owner_id, channel_id)` is the cache key. Two rules:

- **Owner id must be stable across frames for a given widget**, and **distinct
  between widgets**. Inside a widget, `ImGui::GetItemID()` (after the item) or
  `ImGui::GetID("name")` is ideal — ImGui already scopes IDs by window/stack so
  two list rows won't collide. Reusing one literal id for many widgets makes
  them share (and fight over) one animated value.
- **Channel id names the property**: `ImHashStr("height")`, `ImHashStr("knob")`,
  `ImHashStr("fill")`. Hash once into a `static const ImGuiID` if it's hot.

`init_value` (last arg of `iam_tween_*`) is the value the channel is born with.
Set it to your resting target so the widget doesn't visibly animate from 0 on
the very first frame it appears.

## Common recipes

**Hover-grow (track + knob), the seek-bar pattern**
```cpp
const bool active = hovered || held;
float h    = iam_tween_float(id, ImHashStr("h"),    active ? 6.0f : 3.0f, 0.18f,
                             iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade, dt, 3.0f);
float knob = iam_tween_float(id, ImHashStr("knob"), active ? 1.0f : 0.0f, 0.18f,
                             iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade, dt, 0.0f);
// draw track of height h; draw knob with radius/alpha scaled by knob
```

**Color cross-fade (blend in a perceptual space, not raw sRGB)**
```cpp
ImVec4 col = iam_tween_color(id, ImHashStr("col"), target_srgb, 0.2f,
                             iam_ease_preset(iam_ease_out_quad), iam_policy_crossfade,
                             iam_col_oklab, dt, resting_srgb);
```

**Appear / slide-in (offset that eases to 0 when shown)**
```cpp
float dy = iam_tween_float(id, ImHashStr("slide"), shown ? 0.0f : 24.0f, 0.25f,
                           iam_ease_back(1.4f), iam_policy_crossfade, dt, 24.0f);
ImGui::SetCursorPosY(base_y + dy);
```

**Springy / overshoot feel** — swap the easing: `iam_ease_elastic(amp, period)`,
`iam_ease_back(overshoot)`, or `iam_ease_spring_desc(mass, stiff, damp, v0)`.

## Beyond tweens (pointers, don't inline)

The header has large subsystems you only reach for occasionally — read the
relevant section of `im_anim.h` / `docs/` when needed:
- **Clips** (`iam_clip::begin(...).key_float(...).end()`, `iam_play`,
  `iam_instance::get_float`) — authored keyframe timelines with loops, markers,
  callbacks, layering. Needs `iam_clip_update(dt)` in the pump.
- **Oscillators / shake / wiggle / noise** — continuous procedural motion
  (`iam_oscillate`, `iam_shake`, `iam_wiggle`, `iam_noise_channel_*`).
- **Motion paths**, **text-on-path / text stagger**, **gradient / style /
  transform interpolation** — specialized; each has a header section + doc.

## Gotchas checklist

- **No pump = frozen.** `iam_update_begin_frame()` must run every frame after
  `NewFrame()`. Only add `iam_clip_update(dt)` when clips are actually used.
- **dt is `ImGui::GetIO().DeltaTime`.** Don't invent your own clock; ImAnim
  assumes the ImGui frame delta.
- **Stable, unique owner ids.** Prefer `GetItemID()`/`GetID(...)`. Shared ids
  = widgets animating in lockstep or stealing each other's value.
- **Set `init_value`** to the resting target to avoid a first-frame jump.
- **Blend colors in `iam_col_oklab`** (or `srgb_linear`), not raw `srgb`, or
  mid-blends go muddy.
- **Bound memory on long-lived UIs**: call `iam_gc(600)` (and `iam_clip_gc`)
  occasionally so stale per-widget channels are reclaimed.
- Tweens drive *values*; you still draw. Apply the result via `PushStyleVar`,
  draw-list calls, or cursor math — ImAnim never draws your widget for you.
