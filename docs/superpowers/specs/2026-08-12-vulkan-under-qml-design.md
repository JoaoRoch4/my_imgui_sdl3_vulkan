# Vulkan under QML in VulkanMedia — design

Date: 2026-08-12
Status: awaiting review

## Goal

Give `QT/VulkanMedia` a reusable QML item that renders raw Vulkan *underneath*
the QML scene, clipped to the item's own rectangle. This is the bring-up step
for the eventual mpv/libplacebo zero-copy video surface: once a Vulkan region
can be positioned inside the UI, swapping its contents from a demo shape to a
decoded frame is a contained change.

Ported from Qt's official `scenegraph/vulkanunderqml` example (a copy lives at
`QT/Concepts/vulkanunderqml`), with three deliberate departures: the render is
clipped to the item, memory goes through VMA, and the API is driven by
volk + Vulkan-Hpp instead of raw C with `QVulkanFunctions`.

## Decisions taken

| Question | Choice |
| --- | --- |
| Scope of the Vulkan render | Clipped to the item's mapped scene rect, not the whole window |
| VMA source | CMake `FetchContent`, pinned by tag |
| Dispatch ownership | volk loads the pointers; Vulkan-Hpp's default dispatcher is initialised from the same ones |
| Qt's Vulkan API | Allowed and used — `QVulkanInstance` is the seed for volk |
| Re-verification | Headless smoke test **and** a fresh playback screenshot |

## Architecture

Three units, each independently understandable.

### 1. `VulkanSurface` (QQuickItem, GUI thread)

`src/vulkansurface.{h,cpp}`. A `QQuickItem` with `QML_ELEMENT`, exposing:

- `real t` — animation parameter, drives the demo shape.
- `bool available` (read-only) — false when the scene graph is not running on
  Vulkan, so QML can show a fallback instead of a blank hole.

It owns no Vulkan state. It connects `windowChanged` → `beforeSynchronizing`
(sync) and `sceneGraphInvalidated` (cleanup), exactly as the Qt example does,
and it pushes three things to the renderer during sync: `t`, the device-pixel
item rect, and the window pointer.

Departure from the example: the example calls `win->setColor(Qt::black)`
because its blend is additive (`SrcAlpha, One`). We do **not** touch the window
colour — `Theme.bg` stays authoritative. The pipeline uses standard
`SrcAlpha, OneMinusSrcAlpha` so the surface composites over the themed
background like any other element.

### 2. `VulkanSurfaceRenderer` (render thread)

Same translation unit, not exported. Connected with `Qt::DirectConnection` to:

- `beforeRendering` → one-time init (dispatch, VMA, buffers, pipeline).
- `beforeRenderPassRecording` → record the draw as an underlay.

Everything it needs comes from `QSGRendererInterface::getResource()`:
`VulkanInstanceResource`, `PhysicalDeviceResource`, `DeviceResource`,
`RenderPassResource`, `CommandListResource`. **Qt created the instance and the
device; the renderer must never destroy them** — it destroys only what it
allocated.

Clipping is the substantive change from the example. The example sets the
viewport to the full window and draws a `-1..1` quad, so the quad covers
everything. Here the viewport *is* the item rect, which maps the same `-1..1`
quad into that rect with no shader change, and the scissor guards the edges:

```
VkViewport vp = { rx, ry, rw, rh, 0.0f, 1.0f };
VkRect2D   sc = { { int32_t(rx), int32_t(ry) }, { uint32_t(rw), uint32_t(rh) } };
```

Both Qt window coordinates and Vulkan's framebuffer coordinates put the origin
top-left, so the rect needs scaling by `devicePixelRatio` but no Y flip. The
rect is computed on the GUI thread during sync (`mapToScene` on the item's
`boundingRect`), never read from the item on the render thread.

### 3. Dispatch and allocation setup

Once, during renderer init, in this order:

```
volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(
    inst->getInstanceProcAddr("vkGetInstanceProcAddr")));
volkLoadInstance(vkInstance);
volkLoadDevice(device);
VULKAN_HPP_DEFAULT_DISPATCHER.init(vkInstance, vkGetInstanceProcAddr,
                                   device, vkGetDeviceProcAddr);
```

`VK_NO_PROTOTYPES` must be defined before any Vulkan or Qt-Vulkan header in
every TU that includes this code; it is set as a `target_compile_definitions`
on the target rather than a per-file `#define`, so a future TU cannot silently
get it wrong. Qt dlopens the loader instead of linking it, so no symbol clash.

VMA is created with explicit function pointers (required when prototypes are
off), then owns the vertex and uniform buffers that the example allocated by
hand:

```
VmaVulkanFunctions fns{};
fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
fns.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;
// VmaAllocatorCreateInfo: physicalDevice, device, instance, pVulkanFunctions
```

The example's manual memory-type search (~20 lines, twice) disappears —
`VMA_MEMORY_USAGE_AUTO` with `HOST_ACCESS_SEQUENTIAL_WRITE` replaces it.
The uniform buffer keeps its per-frame-slot layout: `framesInFlight` slots,
one dynamic offset, sized to `minUniformBufferOffsetAlignment`. That detail is
not incidental — writing a single UBO every frame would race in-flight frames.

## Build changes

`QT/VulkanMedia/CMakeLists.txt` only. The parent build is not touched.

- `find_package(Qt6 ... )` unchanged; Vulkan comes through `Qt6::Gui`.
- `find_package(volk)` — the system package ships
  `/usr/lib/cmake/volk/volkConfig.cmake` and a compiled `/usr/lib/libvolk.a`,
  so the target is `volk::volk`.
- `find_package(Vulkan REQUIRED)` for `vulkan.hpp`. Arch's `vulkan-headers`
  ships **no** `VulkanHeaders` CMake config, so CMake's built-in `FindVulkan`
  is the way in; link `Vulkan::Headers` only — never `Vulkan::Vulkan`, since
  linking the loader would fight Qt, which dlopens it.
- `FetchContent_Declare(VulkanMemoryAllocator ...)` pinned to a release tag,
  `FetchContent_MakeAvailable`. Header-only; one TU defines
  `VMA_IMPLEMENTATION`.
- `target_compile_definitions(appVulkanMedia PRIVATE VK_NO_PROTOTYPES
  VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)`.
- Shaders: `surface.vert` / `surface.frag` compiled to SPIR-V by a custom
  command using `glslangValidator` (already `find_program`'d by the parent
  repo's root CMakeLists for `bc1_encode.comp` — same pattern, same tool), with
  the `.spv` output added to `qt_add_qml_module`'s `RESOURCES`. Source GLSL is
  checked in; SPIR-V is generated, not committed.

This breaks the README's "no third-party dependencies" claim for the Qt port.
The README gets updated in the same change rather than left stale.

## Enabling Vulkan

`src/main.cpp` gains one line before the engine is constructed:

```
QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
```

This is app-global and irreversible at runtime. Consequences accepted:

- Qt Multimedia's `VideoOutput` presents through the Vulkan RHI instead of
  OpenGL. Expected to work and to be a step toward zero-copy, but it is the
  main regression risk and is explicitly re-verified below.
- If Vulkan initialisation fails, Qt Quick does not silently fall back — the
  app fails to render. `VulkanSurface.available` reports the actual
  `graphicsApi()` so QML degrades gracefully, but the window itself does not.

## QML integration

A ninth panel, `vulkan`, rather than embedding the surface in an existing one —
this keeps the Vulkan bring-up isolated from the video playback work.

- `qml/VulkanPanel.qml` — a `VulkanSurface` filling the panel's inset rect with
  a `SequentialAnimation on t`, plus a themed caption and a fallback state when
  `!available`.
- `qml/LauncherPanel.qml` — one entry appended to the `launchers` array.
- `qml/Main.qml` — one entry each in `panelTitles` and `panelGlyphs`, one
  string appended to the `order` array in `StackLayout.currentIndex`, and one
  `Page { VulkanPanel {} }`.

The panel proves the clipping visually: if the rect maths is wrong, the shape
bleeds outside the panel and over the docks, which is unmissable.

## Error handling

The example calls `qFatal()` on every failure. That is wrong for an
application panel — a shader that fails to load should not take down a file
browser. Every failure path here sets an error string, marks the renderer
inert, and returns; `VulkanSurface` surfaces it to QML the same way
`VideoPanel` surfaces a media error. Vulkan-Hpp's exception mode is used for
the calls we make, wrapped at the init boundary so nothing escapes into Qt's
render loop.

Resource release keeps the example's belt-and-braces shape — both
`sceneGraphInvalidated` and `releaseResources()` with a `CleanupJob`
`QRunnable`, since a threaded render loop can invalidate either way — with the
VMA allocator destroyed last, after the buffers it owns.

## Verification

1. `cmake -B build` — no new warnings beyond the deferred QTP0004 one.
2. `cmake --build build -j8` — clean.
3. `QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen timeout 6 ./build/appVulkanMedia`
   — exit 124, **stderr empty**. Note: offscreen plus Vulkan may not be viable
   on this box; if the RHI cannot initialise headlessly, the smoke test moves
   to `QSG_RHI_BACKEND=vulkan` on the real display and the README's headless
   contract is amended honestly rather than quietly dropped.
4. `QSG_INFO=1` run confirms `Creating QRhi with backend Vulkan`.
5. Screenshot of the Vulkan panel — shape visible, clipped to the panel, docks
   and chrome unpainted.
6. **Screenshot of video playback under the Vulkan RHI** — the regression
   check for the work completed earlier today. Same temporary `grabWindow()`
   hook as before, removed before commit.

## Out of scope

- mpv/libplacebo frame delivery into this surface.
- The `applySeek()` TODO(human) still outstanding in `VideoPanel.qml`.
- The deferred `NEXT_PROMPT.md` CMake debt (QTP0004, deploy script, presets).
