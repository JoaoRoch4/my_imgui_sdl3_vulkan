#!/usr/bin/env bash
#
# setup.sh — one-shot bootstrap for this ImGui + SDL3 + Vulkan app on Fedora/Nobara.
#
# The project builds ALMOST EVERYTHING from source: the media/UI stack is git-cloned
# into the git-ignored external/ tree and compiled by the authored CMake glue under
# thirdparty/<lib>/. From-source: FFmpeg (full-fat: x264/x265/dav1d/aom/vpx/opus/…
# + NVENC/NVDEC/VAAPI/VDPAU/Vulkan), mpv, libplacebo, SDL3, Vulkan-Loader, Dear ImGui,
# ImPlot, TagLib, stb, ffmpegthumbnailer, doctest, libwebp, plutovg, plutosvg.
# Resolved from the system: freetype2 (Freetype::Freetype), fontconfig, egl, gl.
# From vcpkg: curl, reflectcpp[toml].
#
# This script is IDEMPOTENT — re-running only does the missing work:
#   • installs only the dnf packages you don't already have (rpm -q check)
#   • skips any external/<lib> already cloned
#   • vcpkg install is a no-op for ports already built
#
# Phases (each individually skippable):
#   1. system packages   (dnf5; needs sudo)
#   2. vcpkg deps        (curl, reflectcpp[toml])
#   3. clone all external/ sources at pinned refs
#   4. (optional) build the app via ./build.sh
#
# Usage:
#   ./setup.sh                      Run phases 1-3 (does NOT build the app).
#   ./setup.sh --build              Also build the Debug config afterwards.
#   ./setup.sh --skip-packages      Skip a phase (also: --skip-vcpkg / --skip-clone).
#   ./setup.sh --jobs 8             Parallelism for the build.
#   ./setup.sh --vcpkg-root DIR     vcpkg checkout (default: $VCPKG_ROOT or ~/vcpkg).
#   ./setup.sh --no-sudo            Never call sudo.
#   ./setup.sh -y                   Assume "yes" — don't prompt before installing packages.
#   -h | --help                     Show this header.
#
# After setup, build with:  ./build.sh            (all three configs)
#                           ./build.sh debug      (Debug only)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── pretty output ─────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BLUE=$'\e[1;34m'; C_GREEN=$'\e[1;32m'; C_YELLOW=$'\e[1;33m'
    C_RED=$'\e[1;31m'; C_CYAN=$'\e[1;36m'; C_DIM=$'\e[2m'; C_OFF=$'\e[0m'
else
    C_BLUE=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_CYAN=""; C_DIM=""; C_OFF=""
fi
log()  { printf '%s==>%s %s\n'  "$C_BLUE"   "$C_OFF" "$*"; }
step() { printf '\n%s┌── %s%s\n'  "$C_CYAN" "$*" "$C_OFF"; }
ok()   { printf '%s ok%s %s\n'  "$C_GREEN"  "$C_OFF" "$*"; }
skip() { printf '%s   skip%s %s\n' "$C_DIM" "$C_OFF" "$*"; }
warn() { printf '%swarn%s %s\n' "$C_YELLOW" "$C_OFF" "$*"; }
die()  { printf '%serr%s %s\n'  "$C_RED"    "$C_OFF" "$*" >&2; exit 1; }
usage() { awk 'NR>=3 { if ($0 !~ /^#/) exit; sub(/^#( |$)/, ""); print }' "$0"; }

# ── config ────────────────────────────────────────────────────────────────────
DO_PACKAGES=1
DO_VCPKG=1
DO_CLONE=1
DO_BUILD=0
ASSUME_YES=0
USE_SUDO=1
JOBS="$(nproc 2>/dev/null || echo 4)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
VCPKG_TRIPLET="x64-linux"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-packages) DO_PACKAGES=0 ;;
        --skip-vcpkg)    DO_VCPKG=0 ;;
        --skip-clone)    DO_CLONE=0 ;;
        --build)         DO_BUILD=1 ;;
        -y|--yes)        ASSUME_YES=1 ;;
        --no-sudo)       USE_SUDO=0 ;;
        --jobs)          JOBS="${2:?--jobs needs a number}"; shift ;;
        --jobs=*)        JOBS="${1#*=}" ;;
        --vcpkg-root)    VCPKG_ROOT="${2:?--vcpkg-root needs a path}"; shift ;;
        --vcpkg-root=*)  VCPKG_ROOT="${1#*=}" ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "unknown argument '$1' (try --help)" ;;
    esac
    shift
done

if [[ $USE_SUDO -eq 1 && $EUID -ne 0 ]]; then SUDO="sudo"; else SUDO=""; fi
run_root() { if [[ -n "$SUDO" ]]; then $SUDO "$@"; else "$@"; fi; }

# ── Pinned upstream versions (single source of truth — no scattered literals) ──
# Refs verified via `git ls-remote` (2026-07-23). Bump here and re-run to move a dep.
readonly V_FFMPEG="n8.0"
readonly V_SDL="main"                       # SDL3 3.5.0 is dev-only; lives on main (no tag)
readonly V_VULKAN="vulkan-sdk-1.4.350.0"    # shared by Vulkan-Headers + Vulkan-Loader
readonly V_LIBPLACEBO="v7.360.1"            # >=7.360.1 required by mpv v0.41.0
readonly V_MPV="v0.41.0"
readonly IMGUI_VER="1.92.8-docking"         # dir is external/imgui-<ver>; git tag is v<ver>
readonly V_IMPLOT="master"                  # master has the imgui-1.92 AddRect fix (v1.0 does not)
readonly V_TAGLIB="v2.3.1"
readonly V_FFTHUMB="v2.3.0"
readonly V_STB="master"                     # header-only, no upstream tags
readonly V_DOCTEST="v2.5.3"
readonly V_LIBWEBP="v1.6.0"
readonly V_PLUTOVG="v1.3.3"
readonly V_PLUTOSVG="v0.0.8"

# ── Source table: "external/dir|git_url|ref|kind|flags"  (flags: recurse) ──────
readonly SOURCES=(
  "external/FFmpeg|https://github.com/FFmpeg/FFmpeg.git|${V_FFMPEG}|tag|"
  "external/SDL|https://github.com/libsdl-org/SDL.git|${V_SDL}|branch|"
  "external/Vulkan-Headers|https://github.com/KhronosGroup/Vulkan-Headers.git|${V_VULKAN}|tag|"
  "external/Vulkan-Loader|https://github.com/KhronosGroup/Vulkan-Loader.git|${V_VULKAN}|tag|"
  "external/mpv|https://github.com/mpv-player/mpv.git|${V_MPV}|tag|"
  "external/imgui-${IMGUI_VER}|https://github.com/ocornut/imgui.git|v${IMGUI_VER}|tag|"
  "external/implot|https://github.com/epezent/implot.git|${V_IMPLOT}|branch|"
  "external/taglib|https://github.com/taglib/taglib.git|${V_TAGLIB}|tag|recurse"
  "external/ffmpegthumbnailer|https://github.com/dirkvdb/ffmpegthumbnailer.git|${V_FFTHUMB}|tag|"
  "external/stb|https://github.com/nothings/stb.git|${V_STB}|branch|"
  "external/doctest|https://github.com/doctest/doctest.git|${V_DOCTEST}|tag|"
  "external/libwebp|https://github.com/webmproject/libwebp.git|${V_LIBWEBP}|tag|"
  "external/plutovg|https://github.com/sammycage/plutovg.git|${V_PLUTOVG}|tag|"
  "external/plutosvg|https://github.com/sammycage/plutosvg.git|${V_PLUTOSVG}|tag|recurse"
)
# libplacebo needs a subset of submodules — handled specially (clone phase).
readonly LIBPLACEBO_URL="https://code.videolan.org/videolan/libplacebo.git"
readonly LIBPLACEBO_MIRROR="https://github.com/haasn/libplacebo.git"
readonly LIBPLACEBO_SUBMODULES=(3rdparty/glad 3rdparty/jinja 3rdparty/markupsafe 3rdparty/Vulkan-Headers 3rdparty/fast_float)

# ── Fedora 44 packages (every name verified via `dnf5 repoquery`) ─────────────
readonly PKGS=(
  # ── build toolchain ──
  clang lld cmake ninja-build meson make git pkgconf-pkg-config python3
  nasm                       # x86 SIMD assembler for FFmpeg
  # ── host toolchain vcpkg needs to compile its ports (curl -> OpenSSL) ──
  gcc gcc-c++ perl perl-FindBin perl-IPC-Cmd zip unzip tar
  # ── system-resolved app deps ──
  freetype-devel fontconfig-devel                     # Freetype::Freetype + FcFontMatch
  libglvnd-devel mesa-libEGL-devel mesa-libGL-devel   # egl.pc/gl.pc (owned by libglvnd-devel)
  boost-devel                                         # pch.hpp: boost::multiprecision + boost::math (header-only)
  # ── mpv / libplacebo ──
  luajit-devel libass-devel
  zlib-ng-compat-devel zlib-ng-compat-static          # zlib.pc + static archive
  glslang-devel libshaderc-devel lcms2-devel          # libplacebo GLSL->SPIR-V + color mgmt
  # ── FFmpeg external codec libs (full-fat build; Nobara/RPM-Fusion provides these) ──
  libdav1d-devel libaom-devel x264-devel x265-devel libvpx-devel
  opus-devel libvorbis-devel libtheora-devel lame-devel fdk-aac-free-devel
  libwebp-devel libass-devel gnutls-devel
  # .so symlinks FFmpeg's STATIC link (--pkg-config-flags=--static) pulls transitively:
  #   gnutls -> -lunistring,  x265 -> -lnuma
  libunistring-devel numactl-devel
  # ── FFmpeg hardware accel (NVENC/NVDEC via headers only — no CUDA toolkit) ──
  nv-codec-headers libva-devel libvdpau-devel vulkan-headers vulkan-loader-devel
  # ── SDL3 backends + Vulkan-Loader WSI (Wayland/X11/DRM) + audio ──
  wayland-devel wayland-protocols-devel libxkbcommon-devel libdecor-devel
  libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel
  libXfixes-devel libXScrnSaver-devel libxcb-devel
  # Medido em 08/09/2026: sem libXtst-devel o configure do SDL3 morre em
  # external/SDL/cmake/macros.cmake:449 ("Couldn't find dependency package for
  # XTEST"). A alternativa seria -DSDL_X11_XTEST=OFF; instalar o -devel é mais
  # barato do que abrir mão do XTest.
  libXtst-devel
  libdrm-devel mesa-libgbm-devel
  pipewire-devel pulseaudio-libs-devel alsa-lib-devel
  dbus-devel liburing-devel
)

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 1 — system packages
# ═══════════════════════════════════════════════════════════════════════════════
install_packages() {
    step "Phase 1/3 — system packages (dnf5)"
    command -v rpm  >/dev/null 2>&1 || die "rpm not found — this script targets Fedora/Nobara."
    command -v dnf5 >/dev/null 2>&1 || command -v dnf >/dev/null 2>&1 || die "dnf/dnf5 not found."
    local DNF; DNF="$(command -v dnf5 || command -v dnf)"

    local missing=()
    for p in "${PKGS[@]}"; do
        rpm -q "$p" >/dev/null 2>&1 || missing+=("$p")
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        ok "all ${#PKGS[@]} packages already installed"
        return 0
    fi

    log "installing ${#missing[@]} missing package(s):"
    printf '     %s\n' "${missing[@]}" | sed "s/^/${C_DIM}/;s/$/${C_OFF}/"
    local yes=(); [[ $ASSUME_YES -eq 1 ]] && yes=(-y)
    # --skip-unavailable: don't abort the batch if a name drifts / a repo is absent.
    # x264/x265/dav1d/fdk-aac live in RPM Fusion (shipped by Nobara); on stock Fedora
    # enable it first: sudo dnf install rpmfusion-free-release rpmfusion-nonfree-release
    run_root "$DNF" install "${yes[@]}" --skip-unavailable "${missing[@]}"
    ok "packages installed"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 2 — vcpkg dependencies (curl, reflectcpp[toml])
# ═══════════════════════════════════════════════════════════════════════════════
# taglib + libwebp are built from source (thirdparty/taglib, thirdparty/WebP), so
# vcpkg only supplies curl (CURL::libcurl) and reflectcpp (reflectcpp::reflectcpp).
install_vcpkg_deps() {
    step "Phase 2/3 — vcpkg deps (curl, reflectcpp[toml])"
    local vcpkg_bin="$VCPKG_ROOT/vcpkg"
    if [[ ! -x "$vcpkg_bin" ]]; then
        if [[ -f "$VCPKG_ROOT/bootstrap-vcpkg.sh" ]]; then
            log "bootstrapping vcpkg in $VCPKG_ROOT"
            "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
        else
            warn "vcpkg not found at $VCPKG_ROOT — cloning it there"
            git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
            "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
        fi
    fi
    [[ -x "$vcpkg_bin" ]] || die "vcpkg binary still missing at $vcpkg_bin"

    if [[ "$VCPKG_ROOT" != "$HOME/vcpkg" ]]; then
        warn "CMake resolves vcpkg from \$VCPKG_ROOT / ~/vcpkg (see CMakeLists 'vcpkg' block)."
        warn "using $VCPKG_ROOT — export VCPKG_ROOT=$VCPKG_ROOT so ./build.sh's configure agrees."
    fi

    # Classic (non-manifest) mode: run from a dir WITHOUT a vcpkg.json (i.e. $VCPKG_ROOT),
    # else vcpkg switches to manifest mode and rejects package args. Single-quote the
    # bracketed feature so bash doesn't glob it.
    log "vcpkg install (idempotent — already-built ports are skipped)"
    # --recurse: acrescentar a feature [toml] a um reflectcpp[core] já instalado
    # é um REBUILD, e sem essa flag o vcpkg recusa e sai != 0. Pior: se você
    # ignorar, os headers instalados declaram rfl::toml::Writer e a
    # libreflectcpp.a não define — o erro reaparece no LINK do app.
    ( cd "$VCPKG_ROOT" && ./vcpkg install curl 'reflectcpp[toml]' --triplet "$VCPKG_TRIPLET" --recurse )
    ok "vcpkg deps ready in $VCPKG_ROOT/installed/$VCPKG_TRIPLET"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 3 — clone external/ sources (plutovg/plutosvg/libwebp are built by CMake)
# ═══════════════════════════════════════════════════════════════════════════════
clone_one() {
    local dir="$1" url="$2" ref="$3" kind="$4" flags="$5"
    if [[ -d "$dir/.git" ]]; then
        skip "$dir already cloned ($(git -C "$dir" describe --tags --always 2>/dev/null || echo present))"
        return 0
    fi
    log "cloning ${C_DIM}$ref${C_OFF} -> $dir"
    local args=(clone --depth 1)
    [[ "$kind" == "tag" || "$kind" == "branch" ]] && args+=(--branch "$ref")
    [[ "$flags" == *recurse* ]] && args+=(--recurse-submodules --shallow-submodules)
    git "${args[@]}" "$url" "$dir"
}

clone_sources() {
    step "Phase 3/3 — clone external/ sources (pinned refs)"
    mkdir -p external

    for entry in "${SOURCES[@]}"; do
        IFS='|' read -r dir url ref kind flags <<< "$entry"
        clone_one "$dir" "$url" "$ref" "$kind" "$flags"
    done

    # libplacebo: clone then init ONLY the 5 build-required submodules (skip demo-only
    # nuklear). Fall back to the GitHub mirror if VideoLAN is unreachable.
    if [[ -d external/libplacebo/.git ]]; then
        skip "external/libplacebo already cloned"
    else
        log "cloning ${C_DIM}$V_LIBPLACEBO${C_OFF} -> external/libplacebo"
        if ! git clone --depth 1 --branch "$V_LIBPLACEBO" "$LIBPLACEBO_URL" external/libplacebo; then
            warn "VideoLAN host failed — retrying via mirror $LIBPLACEBO_MIRROR"
            git clone --depth 1 --branch "$V_LIBPLACEBO" "$LIBPLACEBO_MIRROR" external/libplacebo
        fi
    fi
    log "initialising libplacebo submodules"
    git -C external/libplacebo submodule update --init --depth 1 -- "${LIBPLACEBO_SUBMODULES[@]}"
    ok "all sources present under external/"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 4 — optional build
# ═══════════════════════════════════════════════════════════════════════════════
build_app() {
    step "Phase 4 — build (Debug) via ./build.sh"
    warn "the first build compiles FFmpeg (full codec set) + mpv + libplacebo from source — this takes a while."
    ./build.sh debug
}

# ── run ───────────────────────────────────────────────────────────────────────
log "Setup for ${C_DIM}$SCRIPT_DIR${C_OFF}  (jobs=$JOBS, vcpkg=$VCPKG_ROOT)"
(( DO_PACKAGES )) && install_packages    || skip "phase 1 (packages) skipped"
(( DO_VCPKG ))    && install_vcpkg_deps   || skip "phase 2 (vcpkg) skipped"
(( DO_CLONE ))    && clone_sources        || skip "phase 3 (clone) skipped"

printf '\n'
ok "Dependencies ready."
if (( DO_BUILD )); then
    build_app
    ok "Build complete — run ./build.sh debug --run to launch."
else
    cat <<EOF

${C_GREEN}Next steps${C_OFF}
  Build everything:   ${C_DIM}./build.sh${C_OFF}          (Debug + Release + RelWithDebInfo)
   …or just Debug:    ${C_DIM}./build.sh debug${C_OFF}
   …and run it:       ${C_DIM}./build.sh debug --run${C_OFF}

  plutovg/plutosvg/libwebp are compiled from source by CMake (targets plutovg::plutovg,
  plutosvg::plutosvg, WebP::webp) — nothing is installed to the system, and no
  PKG_CONFIG_PATH setup is needed. FreeType comes from the system (freetype-devel).
EOF
fi
