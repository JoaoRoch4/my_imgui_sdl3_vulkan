# Documentation index

## Building on Windows → branch [`qt-windows-msvc`](https://github.com/JoaoRoch4/my_imgui_sdl3_vulkan/tree/qt-windows-msvc)

**Windows / MSVC work happens on `qt-windows-msvc`.** That is the branch to check
out, build and open in Visual Studio 2026:

```sh
git switch qt-windows-msvc
```

Everything you need to build there is measured and written down in
**[`windows-msvc-context.md`](windows-msvc-context.md)** *(in Portuguese)* — which
toolset the repo actually uses, which Visual Studio installation CMake picks, and
what still does not build on Windows. The short version:

| | |
|---|---|
| Toolset | `v145` (the **v180** generation), MSVC 14.51.36231 — `v180` is the MSBuild targets folder, not a `PlatformToolset` alias |
| Generator | `Visual Studio 18 2026`, x64 — CMake resolves it to the **Community** install, while `vswhere -latest` resolves to **Enterprise** |
| What builds | `QT/VulkanMedia` → `appVulkanMedia.exe`, via the `vs2026` preset |
| What does not | `example_sdl3_vulkan` (the main SDL3/Vulkan app) — it is Linux-only, see §6 of the doc |

```powershell
$env:QTDIR = "C:\Qt\6.11.2\msvc2022_64"
cmake --preset vs2026
cmake --build build/vs2026 --config Debug --target appVulkanMedia
```

`main` carries the same document for reference, but Windows changes belong on
`qt-windows-msvc`.

## Branches

| Branch | What it is |
|---|---|
| `main` | Integration branch. Linux (clang + Ninja) is the primary build target here. |
| [`qt-windows-msvc`](https://github.com/JoaoRoch4/my_imgui_sdl3_vulkan/tree/qt-windows-msvc) | **The Windows branch** — MSVC / Visual Studio 2026 port of the Qt app. Identical to `main` as of `18e05ff`; it diverges as Windows work lands. |
| [`qtwinmsv-broken-backup`](https://github.com/JoaoRoch4/my_imgui_sdl3_vulkan/tree/qtwinmsv-broken-backup) | Frozen tip of `qt-windows-msvc` from before it was reset onto `main`. Kept only for the superseded `.slnx`/MSBuild hand-port guide: `git show qtwinmsv-broken-backup:docs/vs2026-slnx-port.md` |

## The documents

| Document | About |
|---|---|
| [`windows-msvc-context.md`](windows-msvc-context.md) | 🪟 Windows/MSVC toolchain, VS 2026, the `vs2026` CMake preset, what blocks the main app. **Start here for anything Windows.** *(pt-BR)* |
| [`memory_management.md`](memory_management.md) | Why `AppContext` owns subsystems through `unique_ptr` instead of `shared_ptr`. |
| [`superpowers/specs/`](superpowers/specs) | Design documents, one per feature, dated. |
| [`superpowers/plans/`](superpowers/plans) | Implementation plans that follow from those specs. |

Component-level notes live next to their code rather than here — most usefully
[`QT/VulkanMedia/README.md`](../QT/VulkanMedia/README.md), which documents the Qt/QML
scaffold, its theme singleton, and what in it is real versus stubbed.
