# Video player — design concept

A standalone program that prototypes the sketched player layout, so the interaction can
be judged before any of it is grafted onto the real player in
`code/ui/media/video/widgets/video_ui_window.cpp`.

```
build/debug/concept_player          # or: ./build.sh concept_player
```

## What it prototypes

| Sketch element | Here |
|---|---|
| menu bar | `File` / `View` / `Motion`, with a right-aligned now-playing readout. Docked chrome when windowed; **fullscreen it becomes a fourth auto-hiding panel** — translucent, sliding down off the top edge. |
| scene view (left, auto-hide) | `SceneStrip` — a thumbnail every N seconds, click to jump, follows the playhead |
| folder view (right, auto-hide) | `FolderShelf` — sibling clips, play badge, duration chip, hover to preview |
| hover preview | **inline**: the hovered tile itself starts playing, badge fades out, progress line along its bottom edge. No popup, nothing floats over the video. |
| controls (bottom, auto-hide, smooth anim) | `TransportBar` — **full-width seek strip above** the control row, transport left, volume right |

Plus the piece that isn't in the sketch: **every panel can be popped out of the player**
(`PanelPlacement`) — into a real OS window under X11, into a floating in-window panel
under Wayland (see below).

## Keys and flags

| | |
|---|---|
| `Space` | play / pause |
| `←` `→` | seek ∓5 s |
| `F` / `Esc` | fullscreen / leave fullscreen |
| `Tab` | pin every panel open (compare the sketched layout against the auto-hiding one) |
| `1` `2` `3` | pop scene view / folder view / controls out and back |
| `Ctrl+Q` | quit |

```
concept_player --pin                       start with every panel held open
concept_player --popout=scene,folder       start with those panels torn off
```

On the video itself: tap to pause, hold to run at 2× while held, double-click for
fullscreen — carried over verbatim from the current player, so the new chrome has to
prove it can coexist with them.

## Why it looks the way it does

**There is no decoder.** Frames come from `FramePattern`, which draws a seeded sky, a sun
tracking the timestamp, and three parallax ridges straight into an `ImDrawList`. The
questions this program answers — reveal timing, motion, layout, whether the strip reads
at a glance — need pictures that change over time and differ between clips, not mpv. Every
widget is written against plain data (a timestamp, a `MediaTile`), so porting to real
frames touches the draw call and nothing else.

**Panels are not chrome, they have a placement.** `ConceptPanel` is either an auto-hiding
overlay riding over the video, or a torn-off window. That is what removes the choice the
current player is stuck with — cover the video, or don't have the panel at all.

**Preview happens where you are looking.** The first version floated a preview card over
the video. Inline is better on both counts that matter: the preview appears exactly where
the eye already is, and it cannot cover the thing you are comparing it against.

**Fullscreen has no chrome, so the menu bar becomes a panel.** Windowed, it is an
ordinary menu bar on the player window. Fullscreen, there is nothing to attach it to, so
it joins the other three: hidden until the cursor reaches the top edge, then sliding down
over the picture. It runs much more transparent than the side panels (`background_alpha`
0.34 against 0.90) — those are content you read, this is a strip of labels you glance at,
sitting directly over the video, so it only has to stay legible. `draw_menu_items()` is
shared by both paths, so the docked and fullscreen bars cannot drift apart.

**Auto-hide is per-panel and has a way back.** The current player has one hardcoded 1.5 s
idle timer that hard-cuts the whole control bar (`if (!show_controls) { ImGui::End();
return; }`), with no gesture to bring it back except moving the mouse, which reveals
everything. `PanelReveal` gives each panel its own edge-proximity hot zone (`View > Show
reveal hot zones` draws them), and its `shown()` is a continuous 0..1 so the panel slides
and fades rather than blinking.

**The transport owns the bottom edge.** The side panels are laid out against a stage
shortened by however much the transport currently occupies, so the controls and volume are
never buried under a side panel. Drawing order in `ConceptPlayerApp::draw()` depends on
this — transport first, then the side panels.

## Motion / ImAnim

The project's `imanim` skill documents `external/ImAnim` as how UI motion is done here.
**That library is not vendored in this checkout** — only `.claude/skills/imanim/SKILL.md`
is, on every branch. So `Motion` (`motion.hpp`) reproduces ImAnim's contract exactly:

- called unconditionally every frame with a *target*, never "start animation"
- state keyed by `(owner ImGuiID, channel ImGuiID)`
- a target that flips mid-flight is redirected smoothly (ImAnim's crossfade policy)
- `init` is the value a channel is born with, so nothing animates in from 0 on first frame
- `Motion::begin_frame()` is the mandatory per-frame pump, called once in `main.cpp` right
  after `ImGui::NewFrame()` — exactly where `iam_update_begin_frame()` goes

Drop `external/ImAnim` in place and re-run CMake: `concept_player/CMakeLists.txt` detects
it, defines `CONCEPT_HAS_IMANIM`, and every `Motion::` call forwards to `iam_tween_*`. No
call site changes. `View > Motion` shows which backend is live and the channel count.

> The forwarding branch in `motion.cpp` is written from the skill doc, since the real
> header isn't available to check against. If ImAnim's signatures have drifted, that file
> is the only place that needs adjusting — nothing else calls `iam_*` directly.

## Build

Same shape as `launcher/`: its own minimal Dear ImGui, linking only from-source
`SDL3::SDL3`, so it builds in seconds and drags in no FFmpeg / mpv / libplacebo / Vulkan.

One deliberate difference: the renderer backend is **`imgui_impl_sdlgpu3`, not
`imgui_impl_sdlrenderer3`**. SDL_Renderer3 lists multi-viewport as unsupported
(`[ ] Renderer: Multi-viewport support`) while SDL_GPU3 supports it, and popping a panel
into a real OS window needs it. SDL_GPU is core SDL3, so this costs no extra dependency.

### Viewports: X11 yes, Wayland no

Measured on this machine, with a `--popout=scene,folder` launch:

| backend | platform windows |
|---|---|
| x11 | `3 (1 main + 2 popped out)`, stable — two real OS windows |
| wayland | `4 → 3 → 2 → 1` within a second — every torn-off panel silently snapped back |

The Wayland collapse is not a bug in the panel code. The compositor owns window placement
and offers no protocol for a client to learn where its window went, so ImGui reads a
torn-off panel back as overlapping the host and auto-merge reclaims it. Rather than ship a
feature that quietly undoes itself, `main.cpp` does not set
`ImGuiConfigFlags_ViewportsEnable` under Wayland at all; pop-out there becomes a floating
window inside the player. The startup log states which mode is live.

Two ordering rules fell out of this and are load-bearing (`concept_panel.cpp`):

- popped-out panels set `ImGuiViewportFlags_NoAutoMerge` through an `ImGuiWindowClass`
  **before** `Begin()` — ImGui decides whether to create a viewport up front, so this is
  the only way to force one. Positioning a window outside the host is not enough.
- overlay panels call `SetNextWindowViewport(GetMainViewport()->ID)`. Without it, a panel
  mid-slide sits partly outside the host rect, and three OS windows flickered into
  existence for a few frames on every launch (the count spiked to 4, then fell to 1).

## Files

One type per file, named after it.

```
motion.*, motion_channel.hpp, ease_preset.hpp   the ImAnim seam
panel_edge/placement/bounds.hpp                 layout vocabulary
panel_reveal.*                                  auto-hide + edge proximity + slide
concept_panel.*                                 overlay <-> popped-out OS window
playback_clock.*, media_tile.hpp, scene_frame.hpp   stand-in state
frame_pattern.*                                 procedural frames
video_stage.*, scene_strip.*, folder_shelf.*,
transport_bar.*                                 the sketch widgets
concept_player_app.*                            composition + menus + shortcuts
main.cpp                                        SDL3 + SDL_GPU + ImGui bootstrap
```
