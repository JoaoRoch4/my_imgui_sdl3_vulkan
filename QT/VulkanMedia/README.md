# VulkanMedia — Qt 6 / QML scaffold

The QML port of the SDL3 + Vulkan + ImGui media browser: chrome, docks, theme
and interaction model, with stub data behind them. Everything renders populated
on first run; no media backend is wired up yet.

## Build

```sh
cmake -B build
cmake --build build
./build/appVulkanMedia
```

Requires Qt 6.5+ (developed against 6.11), CMake 3.21+, a C++20 compiler.
Modules used: `Core Gui Quick Qml QuickControls2 Concurrent` — all part of Qt,
**no third-party dependencies**.

Headless smoke test (the way this scaffold is verified in CI-like settings):

```sh
QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen timeout 5 ./build/appVulkanMedia
# exit 124 = the window loaded and ran until the timeout; 255 = QML failed to load
```

`QT_FORCE_STDERR_LOGGING=1` matters on distros whose Qt is built with journald
support — without it, QML errors go to the journal and the app appears to fail
silently.

## Where the theme lives

`qml/Theme.qml` — a QML singleton (`pragma Singleton`; registered through
`set_source_files_properties(... QT_QML_SINGLETON_TYPE TRUE)` in
`CMakeLists.txt`, since `qt_add_qml_module` generates the `qmldir` itself).

It holds the exact palette from the design system plus derived helpers:

| Group | Members |
| --- | --- |
| Surfaces | `bg` `surface` `panel` `chrome` |
| Text | `text` `bodyText` (neutral-300) `mutedText` (neutral-500) `dimText` |
| Ramps | `neutral` / `accentRamp` arrays, plus `neutral100…900`, `accent100…900` |
| Tint pair | `tintFill` (accent-900) + `tintRing` (accent-800) |
| Borders | `border` (neutral-800), `borderSoft` (neutral-900) |
| Type | `uiFamily` `monoFamily` `fsMicro` `fsSmall` `fsBody` |
| Motion | `durHover` 90 · `durTab` 120 · `durPopup` 140 · `durPanel` 160 · `durThumb` 180 · `durDisplace` 200 |

**No colour literals exist outside `Theme.qml`.** Where a translucent variant is
needed, call sites derive it (`Qt.alpha(Theme.accent, 0.12)`) rather than
hard-coding a hex value.

Design rules held throughout: the accent is a line, ring or glow — never a
large fill; tinted surfaces are accent-900 with an accent-800 inset ring;
primary buttons are outlined; focus is a 2px accent ring at 2px offset, never
the platform default. The style is QtQuick.Controls **Basic**
(`QQuickStyle::setStyle("Basic")` in `main.cpp`) precisely so nothing is
themed behind the app's back.

### Two deviations from the spec, both forced by QML

- **Font sizes.** `font.pixelSize` is an integer property, so the design
  system's half-pixel steps cannot be assigned literally (QML rejects
  `font.pixelSize: 9.5`). They are rounded once in `Theme`: `fsMicro` 10
  (9.5), `fsSmall` 11 (10.5), `fsBody` 12 (11.5). Integer sizes are used
  literally.
- **Font families.** This Qt's QML font value type has no `families` list, so
  `Theme.uiFamily` / `Theme.monoFamily` resolve the first *installed* family
  out of `fontUiFamilies` / `fontMonoFamilies` via `Qt.fontFamilies()`.
  `fontUi` / `fontMono` still carry the spec'd `"Inter"` / `"JetBrains Mono"`.
  If Inter is not installed, the UI falls back to Noto Sans / DejaVu Sans.

## Layout

```
MenuBarRow    29px   app name + File…Help, active menu tinted, themed dropdowns
ToolBarRow    36px   back/forward/up · breadcrumb pill · sort pill · view mode · search
Body                 LauncherPanel (176) │ tab strip + panel stack │ DetailsPanel (252)
CommandBar    34px   accent prompt, mono input, blinking caret, completion popup
StatusBar     26px   selection summary · thumbs n/total · downloads · renderer · fps
```

`PanelMenu.qml` (Panels menu, or Ctrl+Shift+P) is the 306px tool-window menu:
LEFT/RIGHT/BOTTOM DOCK groups, accent checkmarks for open panels, mono
shortcuts, and a 186px "Open in →" submenu.

## Real vs stubbed

**Real**

- `FileSystemModel` (`src/filesystemmodel.{h,cpp}`) — `QAbstractListModel` with
  roles `name path kind size modified thumbnail selected focused duration
  dimensions format`. Reads a real directory, sorts folders-first with a
  natural collator, watches it with `QFileSystemWatcher` (coalesced through a
  150 ms timer), and implements selection (`selectOnly` `toggleSelected`
  `selectRange` `selectIndices` `selectAll` `clearSelection`), a focused row
  that drives the details panel, `goUp`/`enter`, and trash/delete
  (`QFile::moveToTrash` / `QFile::remove`).
- `ThumbnailProvider` (`src/thumbnailprovider.{h,cpp}`) — a
  `QQuickAsyncImageProvider`; each request is a `QRunnable` on a `QThreadPool`,
  so decoding never touches the GUI thread. Results go through a mutex-guarded
  LRU `QCache` (192 entries). `ThumbnailCache` is a C++ QML singleton exposing
  `ready`/`total`, which the status bar binds to as "thumbs n/total". Real
  image files are decoded with `QImageReader` (scaled at read time).
- Selection, multi-select (ctrl / shift), rubber-band banding, hover, the
  context menu, tab open/close/reopen, panel switching, command-bar history and
  completion, search dimming, and every animation listed in the spec.
- `QSettings` persistence (via `QtCore`'s `Settings`): dock visibility, active
  panel, view mode, open panels, last folder. (Dock *widths* are the fixed 176 /
  252 constants from the spec, so there is nothing to persist there yet.)

**Stubbed**

- **Folder contents.** When the configured folder is missing or empty, the model
  serves ~20 fake entries (`stubEntries()`), so the window is never blank and
  never crashes on a missing path. `stubbed` is exposed as a property; trash
  and delete on stub rows only drop rows, they never touch the disk.
- **Thumbnails.** No real decoder is wired in. `drawPlaceholder()` paints a
  deterministic gradient + glyph per path, and a small artificial delay
  (40–260 ms, hashed from the path) makes the spinner and the grid's add
  transition observable. Replace the body of `ThumbnailResponse::run()` to hook
  up a real decoder.
- **VideoPanel.qml** — OSD chrome over an empty surface. mpv/libplacebo
  integration is explicitly out of scope for the scaffold.
- **PlaceholderPanel.qml** — images, downloads, console, config, metadata,
  threads all render a themed empty state.
- **Places / volumes** are a static `ListModel` (mounted, unmounted and
  connected-network rows are represented, but nothing is really mounted).
- **View mode.** The list/grid/masonry SegmentedControl switches and persists,
  but `FileGrid` does not read `viewMode` yet — all three render the same grid.
- **Sort pill, panel placement ("Open in → …"), tag chips, copy-paths, the
  downloads count and the renderer string** log or no-op.
- **fps** is real (`FrameAnimation.smoothFrameTime`), which means the render
  loop runs continuously. Set `StatusBar.showFps: false` to stop that.
- **Icons/fonts** — Unicode glyphs, no bundled assets. See `assets/*/README.md`.

## Keyboard

| Keys | Action |
| --- | --- |
| Alt+1 / 2 / 3 / 4 | focus left dock / right dock / command bar / center |
| Ctrl+1 / 2 / 3 | open Files / Images / Video |
| F1 · F2 | Console · Runtime config |
| Ctrl+T · Ctrl+Shift+T | new tab · reopen closed tab |
| Ctrl+` | focus the command bar |
| Ctrl+D · Ctrl+A · Esc | toggle details · select all · clear selection |
| Ctrl+Shift+P | panel menu |
| Ctrl+O · Ctrl+Q · F11 | open folder · quit · full screen |
| Delete · Shift+Delete | move to trash · permanent-delete confirm dialog |

## Layout of the source

```
CMakeLists.txt
src/main.cpp                  QQmlApplicationEngine, Basic style, image provider
src/filesystemmodel.{h,cpp}   directory model + selection
src/thumbnailprovider.{h,cpp} async provider, LRU cache, progress counter
qml/Theme.qml                 the singleton every other file reads
qml/Main.qml                  window, docks, tabs, shortcuts, persistence
qml/{MenuBarRow,ToolBarRow,LauncherPanel,PlacesList,FileGrid,
     DetailsPanel,CommandBar,StatusBar,PanelMenu}.qml
qml/{VideoPanel,PlaceholderPanel}.qml
qml/components/{IconButton,SegmentedControl,Chip,PanelHeader,KeyValueRow}.qml
qml/Format.js                 byte/date formatting helpers
assets/fonts  assets/icons    empty, see the READMEs inside
```

QML files in subdirectories are still registered under the flat `VulkanMedia`
URI by basename — `import VulkanMedia` is all any file needs, including for the
`components/` types.
