# Native mpv OSD — Design

**Date:** 2026-06-20
**Branch:** feat/mpv-native-osd
**Status:** Approved design, pending implementation plan

## Goal

Replace the app's custom ImGui status overlay (`VideoOsdOverlay`) with mpv's
**native libass OSD**, rendered inline into the video texture by the render API.
mpv formats the messages itself (property-expanded templates, `show-progress`
seek bar). Download progress — which mpv has no concept of — is shown in the
ImGui window **title bar** when that title is visible, and via mpv `show-text`
when it is not (fullscreen).

## Background — current state (as discovered)

- **Custom overlay:** `VideoOsdOverlay`
  ([video_osd_overlay.{hpp,cpp}](../../../code/ui/media/video/widgets/video_osd_overlay.cpp))
  is a transient ImGui toast (`show(msg, duration)` → `draw()` with fade). It is
  embedded per-entry as `VideoEntry.osd` / `PlaceboEntry.osd` and surfaced
  through `VideoUiWindow::State.osd`.
- **Call sites** (all in
  [video_ui_window.cpp](../../../code/ui/media/video/widgets/video_ui_window.cpp)):
  pause/play (494), seek (500-502), reload (546, 781), fullscreen (555, 672),
  speed hold (686, 693), loop (765), volume (832), and the fullscreen download
  block (702-709).
- **Two asymmetric paths:**
  - **Path A** — `VideoPlayer::draw_window` → `VideoUiWindow::draw`. Owns every
    `osd.show()` call and already appends ` (X MB↓)` to the visible title in
    windowed mode ([video_player.cpp:1904-1913](../../../code/ui/media/video/player/video_player.cpp#L1904)).
  - **Path B** — `VideoPlayerPlacebo::draw` is a bare `Begin`+`Image` loop
    ([video_player_placebo.cpp:483-524](../../../code/ui/media/video/player/video_player_placebo.cpp#L483));
    its `PlaceboEntry.osd` is **vestigial** (set on open, never drawn) and its
    title has no download info.
- **Render API renders OSD inline:** the mpv render call bakes OSD into the same
  framebuffer as the video (ImPlay passes no special OSD param —
  [mpv.cpp:80-84](../../../external/ImPlay/source/mpv.cpp#L80); the SW path here
  is the same). So **no render-loop changes are needed** — issuing an OSD
  command and letting the existing render pass run is sufficient. A side benefit:
  Path B gains OSD it never had, because the text lands in the sampled texture.

## Design

### 1. Enable mpv OSD at init (playback handles only)

In the option block before `mpv_initialize` for the **real playback** handles
(Path A `VideoPlayer` open paths ~393-398 and the URL variant ~462-480; Path B
`VideoPlayerPlacebo` open):

- `osd-level = 1` (OSD messages enabled).
- `osd-duration = 1200` (ms; matches the old default toast duration).
- `osd-bar = yes` (so `show-progress` draws the seek bar).
- `osd-playing-msg = ${media-title}` — mpv auto-shows this on file load,
  replacing the open-time `initial_osd_message`.
- Optional `osd-font-size` tuning for parity with the old overlay.

**Keep OSD off** (`osd-level = 0`) on the hover-preview, seek-preview, and
`m_hover_player` handles so thumbnails render clean (no baked-in text).

### 2. Convert call sites to native mpv OSD

A small helper (free function near the top of `video_ui_window.cpp`):

```cpp
// Issue an mpv OSD message. mpv renders it into the video texture.
void mpv_osd(mpv_handle *h, const char *text);   // wraps ["show-text", text]
```

Replacements (mpv formats the value via property expansion where possible):

| Action | Old | New |
|--------|-----|-----|
| Seek | `osd.show("Seek +Ns")` | `mpv_command_string(h, "show-progress")` |
| Pause/Play | `osd.show("Paused"/"Playing")` | `mpv_osd(h, "${pause}")` → "yes"/"no", or literal "Paused"/"Playing" |
| Volume | `osd.show("▁▂… 50%")` | `mpv_osd(h, "Volume: ${volume}%")` |
| Speed | `osd.show("1x")` | `mpv_osd(h, "Speed: ${speed}x")` |
| Loop | `osd.show("Loop On/Off")` | `mpv_osd(h, "${loop-file}")` or literal |
| Fullscreen | `osd.show("Fullscreen")` | `mpv_osd(h, "Fullscreen"/"Windowed")` |
| Reload | `osd.show("Reloading...")` | `mpv_osd(h, "Reloading…")` |

> Property-expanded templates pass through `mpv_command`'s `${…}` engine. Escape
> literal `%`/`$` as needed and confirm `${volume}`/`${speed}` formatting on a
> real file during implementation.

### 3. Download info → window title (with fullscreen fallback)

mpv has no download-bytes property, so download progress is **app-owned**:

- **Windowed (title visible):** keep the `(X MB↓)` title append in Path A;
  **mirror it into Path B's `win_title`** so both paths show it.
- **Fullscreen (`NoDecoration`, no title bar):** push the same
  `"Downloading X MB"` string through `mpv_osd(h, …)` so it stays visible.
  This is the only download case that uses the OSD.
- **Remove** the custom-overlay fullscreen download block (702-709); its
  fullscreen role is taken over by the `mpv_osd` call above.

Title identity is preserved: the download text is added only to the **visible**
portion before the `###video_<id>` / `###vpp_<id>` ID suffix, so window
position/state persistence is unaffected.

### 4. Remove `VideoOsdOverlay`

- Delete `video_osd_overlay.{hpp,cpp}`.
- Remove the `osd` member from `VideoEntry` and `PlaceboEntry`, and the
  `#include "video_osd_overlay.hpp"` from both plus `video_ui_window.hpp` and
  `video_player.hpp`.
- Remove the `osd` field from `VideoUiWindow::State` and the constructor arg.
- Replace the `initial_osd_message` → `osd.show()` plumbing (constructors in
  both players) with the `osd-playing-msg` option; drop the parameter if no
  longer used, or keep it only where a non-title open message is still wanted.
- Update `thirdparty/`/`CMakeLists` source lists to drop the two files.

## Data flow

```
user action (pause/seek/volume/…) in VideoUiWindow
  └─ mpv_command(show-text / show-progress)
       └─ mpv flags render update → update_frames() re-renders
            └─ OSD baked into video texture (both SW and placebo paths)
                 └─ ImGui::Image samples the texture (OSD already in it)

download bytes (app downloader)
  ├─ windowed → appended to display_title → ImGui::Begin title bar
  └─ fullscreen → mpv_osd("Downloading X MB")
```

## Error handling / edge cases

- **Throttle vs OSD:** the upload throttle gates on `container-fps`. An OSD-only
  change on a paused or static frame must still re-render. mpv flags a render
  update when the OSD changes; `update_frames()` must process it even when no new
  video frame is available — verify the throttle does not drop these.
- **Paused video:** OSD appears because the OSD change triggers a render update.
- **Preview/hover handles:** must stay `osd-level = 0`; a stray OSD here would
  bake text into thumbnails.
- **Property expansion:** guard against empty/unknown properties (e.g. before a
  file is loaded) so the template renders sanely.
- **Path B controls:** Path B's simple draw has no keybindings to *trigger* OSD;
  that pre-existing limitation is out of scope — but `osd-playing-msg` and the
  download string still appear because mpv/app drive them without UI controls.

## Testing

- Manual (Path A): pause, seek (seek bar via `show-progress`), volume, speed,
  loop, fullscreen toggles all show mpv-rendered OSD baked into the frame.
- Paused-frame OSD: pause, then change volume → OSD still appears.
- Download: windowed shows `(X MB↓)` in the title; fullscreen shows
  "Downloading X MB" via OSD; both clear when the download completes.
- Thumbnails: hover/seek previews show **no** OSD text.
- Path B: open under `IMGUI_USE_VPP=1`; confirm `osd-playing-msg` + download
  string appear and nothing references the removed overlay.
- Build: confirm `video_osd_overlay.*` removal compiles clean across both paths.

## Out of scope (YAGNI)

- mpv OSC (the Lua on-screen controller) — needs input forwarding mpv doesn't
  get as a headless decoder.
- Adding full playback controls to Path B's draw loop.
- Subtitle UI/track selection (mpv OSD would render subs if enabled, but no new
  UI is added here).
- Per-message font/emoji parity with the old ImGui overlay (libass uses its own
  font; the fancy volume-bar glyphs are intentionally dropped in favor of
  `Volume: N%`).
