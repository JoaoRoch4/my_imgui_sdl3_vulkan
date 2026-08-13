# Prompt for the next session (paste into Claude Code in VS Code)

---

Fix and harden the CMake setup of the **VulkanMedia** Qt 6 / QML scaffold in
this folder (`QT/VulkanMedia`). The app already builds and runs — do not
redesign the UI, do not change any value in `qml/Theme.qml`, and do not add
third-party dependencies (ask first if you think you need one).

## The actual problem

`cmake -B build` succeeds but emits this author warning:

```
CMake Warning (author) at /usr/lib/cmake/Qt6Core/Qt6CoreMacros.cmake:3565:
  Qt policy QTP0004 is not set: You need qmldir files for each extra
  directory that contains .qml files for your module.
  https://doc.qt.io/qt-6/qt-cmake-policy-qtp0004.html
Call Stack: ... CMakeLists.txt:28 (qt_add_qml_module)
```

It fires because the QML files sit in subdirectories (`qml/` and
`qml/components/`) rather than at the module root. Right now the OLD behaviour
is in effect: every QML file is registered flat under the `VulkanMedia` URI by
basename, so `import VulkanMedia` alone resolves `Theme`, `FileGrid`,
`IconButton`, everything. Every QML file depends on that.

Decide deliberately between these, and say which you picked and why:

1. `qt_policy(SET QTP0004 OLD)` — keeps today's flat imports, silences the
   warning, documents the choice. Lowest risk.
2. `qt_policy(SET QTP0004 NEW)` — Qt generates per-directory qmldir files.
   Verify what this does to type resolution; if `qml/components/*` becomes a
   separate `VulkanMedia.components` module, update every affected `import`
   and re-verify at runtime, not just at compile time.
3. Restructure so the QML sits at the module root (e.g. `QT_QML_SOURCE_DIRECTORY`
   or moving files) and the question disappears.

Whichever you choose, the app must still load with zero warnings on stderr.

## Also worth fixing while you are in CMakeLists.txt

- `install()` currently installs the bare target, so an installed copy will not
  run. Add `qt_generate_deploy_qml_app_script()` + `install(SCRIPT ...)` so
  `cmake --install build --prefix <dir>` produces something launchable.
- Add a `CMakePresets.json` (the repo root already has one — match its style).
  Qt Creator is configuring `build/Desktop_Qt_6_11_1_Debug` while the CLI uses
  `build/`; presets let both share a tree.
- `target_include_directories(appVulkanMedia PRIVATE src)` exists because the
  generated `appvulkanmedia_qmltyperegistrations.cpp` includes the
  `QML_ELEMENT` headers by bare name. Keep it unless you find a cleaner
  mechanism — if you keep it, leave the comment explaining why.
- Consider whether this should be reachable from the repo-root `CMakeLists.txt`
  behind an option (e.g. `VULKANMEDIA_BUILD_QT_PORT`, default OFF). The root
  project is the SDL3/Vulkan/ImGui app with a vcpkg toolchain — do **not** make
  the Qt port build by default, and do not perturb the existing build.

## How to verify (do all of these, do not skip the runtime check)

```sh
rm -rf build
cmake -B build                 # must be warning-free
cmake --build build -j8        # must be error- and warning-free
QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen timeout 6 ./build/appVulkanMedia
# exit 124 = loaded and ran; 255 = QML failed to load; stderr must be EMPTY
```

**Critical environment gotcha:** this Arch Qt build routes QML errors to
journald, so without `QT_FORCE_STDERR_LOGGING=1` a broken app exits silently
with no message at all. A successful *compile* proves nothing about QML — QML
type errors only appear at load time. Always run the offscreen check.

To eyeball the UI without an X server, you can temporarily add a
`QQuickWindow::grabWindow()` call behind an env var in `src/main.cpp`, save a
PNG, look at it, then remove the hook before committing.

## Context you should not have to rediscover

- Branch `worktree-vulkanmedia-qml-scaffold`, commit `016809d`. `git push`
  failed here: the `origin` remote is HTTPS and no credentials are available in
  the agent environment. Commit locally; ask the user to push.
- `README.md` documents what is real vs stubbed and two QML-forced deviations
  from the design spec (integer-only `font.pixelSize`; no `font.families` list,
  so `Theme.uiFamily`/`monoFamily` resolve via `Qt.fontFamilies()`). Keep it
  accurate if your changes touch any of that.
- Qt 6.11.1, CMake 4.4.2, C++20, QtQuick.Controls **Basic** style (not Fusion).
- No colour literal may appear outside `qml/Theme.qml`.

When done: report which QTP0004 option you chose, the clean configure/build/run
output, and commit. This file (`NEXT_PROMPT.md`) is scratch — delete it as part
of your commit.
