#!/usr/bin/env bash
#
# setup.sh — one-shot bootstrap for this ImGui + SDL3 + Vulkan app on Fedora/Nobara.
#
# The project builds ALMOST EVERYTHING from source: the media/UI stack (FFmpeg,
# mpv, libplacebo, SDL3, Vulkan-Loader, Dear ImGui, ImPlot, TagLib, stb,
# ffmpegthumbnailer, doctest) is git-cloned into the git-ignored external/ tree
# and compiled by the authored CMake glue under thirdparty/<lib>/. A few deps are
# resolved from the system (freetype2, fontconfig, egl, gl) or from vcpkg
# (curl, libwebp, reflectcpp[toml], taglib). plutosvg/plutovg are not packaged by
# Fedora and are vendored into external/ and built from source there — NOT installed
# to the system (static libs into an in-repo prefix; no sudo, no /usr/local writes).
#
# This script is IDEMPOTENT — re-running it only does the missing work:
#   • installs only the dnf packages you don't already have (rpm -q check)
#   • skips any external/<lib> that is already cloned
#   • skips the plutosvg build if pkg-config already finds it
#   • vcpkg install is a no-op for ports already built
#
# Phases (run in order, each individually skippable):
#   1. system packages   (dnf5; needs sudo)
#   2. vcpkg deps        (curl, libwebp, reflectcpp[toml], taglib)
#   3. plutovg + plutosvg from source -> external/ (FreeType-enabled, static; no system install)
#   4. clone all external/ sources at pinned refs
#   5. (optional) build the app via ./build.sh
#
# Usage:
#   ./setup.sh                      Run phases 1-4 (does NOT build the app).
#   ./setup.sh --build              Also build the Debug config afterwards.
#   ./setup.sh --skip-packages      Skip a phase (also: --skip-vcpkg / --skip-plutosvg / --skip-clone).
#   ./setup.sh --jobs 8             Parallelism for the from-source library builds.
#   ./setup.sh --vcpkg-root DIR     Point at a vcpkg checkout (default: $VCPKG_ROOT or ~/vcpkg).
#   ./setup.sh --no-sudo            Never call sudo (assume you're root / packages present).
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
DO_PLUTOSVG=1
DO_CLONE=1
DO_BUILD=0
ASSUME_YES=0
USE_SUDO=1
JOBS="$(nproc 2>/dev/null || echo 4)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
VCPKG_TRIPLET="x64-linux"
# plutovg/plutosvg are vendored into external/ and installed to an IN-REPO prefix
# (git-ignored via external/*). Static libs -> nothing is written to the system.
PLUTO_PREFIX="$SCRIPT_DIR/external/_local"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-packages) DO_PACKAGES=0 ;;
        --skip-vcpkg)    DO_VCPKG=0 ;;
        --skip-plutosvg) DO_PLUTOSVG=0 ;;
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

# sudo wrapper: honour --no-sudo and the case where we're already root.
if [[ $USE_SUDO -eq 1 && $EUID -ne 0 ]]; then SUDO="sudo"; else SUDO=""; fi
run_root() { if [[ -n "$SUDO" ]]; then $SUDO "$@"; else "$@"; fi; }

# ── Pinned upstreams (all refs verified via `git ls-remote` on 2026-07-23) ─────
# Format: "external/dir|git_url|ref|kind|flags"   flags: recurse = --recurse-submodules
readonly SOURCES=(
  "external/FFmpeg|https://github.com/FFmpeg/FFmpeg.git|n8.0|tag|"
  "external/SDL|https://github.com/libsdl-org/SDL.git|main|branch|"
  "external/Vulkan-Headers|https://github.com/KhronosGroup/Vulkan-Headers.git|vulkan-sdk-1.4.350.0|tag|"
  "external/Vulkan-Loader|https://github.com/KhronosGroup/Vulkan-Loader.git|vulkan-sdk-1.4.350.0|tag|"
  "external/mpv|https://github.com/mpv-player/mpv.git|v0.41.0|tag|"
  "external/imgui-1.92.8-docking|https://github.com/ocornut/imgui.git|v1.92.8-docking|tag|"
  "external/implot|https://github.com/epezent/implot.git|master|branch|"   # master: has the imgui-1.92 AddRect fix (>= d65a2be); v1.0 does NOT build
  "external/taglib|https://github.com/taglib/taglib.git|v2.3.1|tag|recurse"  # utfcpp submodule
  "external/ffmpegthumbnailer|https://github.com/dirkvdb/ffmpegthumbnailer.git|v2.3.0|tag|"
  "external/stb|https://github.com/nothings/stb.git|master|branch|"          # header-only, no tags upstream
  "external/doctest|https://github.com/doctest/doctest.git|v2.5.3|tag|"
)
# libplacebo is handled specially (needs a subset of submodules), see clone phase.
readonly LIBPLACEBO_URL="https://code.videolan.org/videolan/libplacebo.git"
readonly LIBPLACEBO_MIRROR="https://github.com/haasn/libplacebo.git"   # fallback if VideoLAN host is unreachable
readonly LIBPLACEBO_REF="v7.360.1"                                     # >=7.360.1 required by mpv v0.41.0
readonly LIBPLACEBO_SUBMODULES=(3rdparty/glad 3rdparty/jinja 3rdparty/markupsafe 3rdparty/Vulkan-Headers 3rdparty/fast_float)

# ── Fedora 44 packages (every name verified present via `dnf5 repoquery`) ──────
readonly PKGS=(
  # ── build toolchain ──
  clang lld cmake ninja-build meson make git pkgconf-pkg-config python3
  nasm                       # x86 SIMD assembler for FFmpeg 8.0 (Release/RelWithDebInfo)
  # ── host toolchain vcpkg needs to compile its ports (curl -> OpenSSL) ──
  gcc gcc-c++ perl perl-FindBin perl-IPC-Cmd zip unzip tar
  # ── system-resolved app deps (pkg_check_modules) + plutosvg build-dep ──
  freetype-devel fontconfig-devel
  libglvnd-devel mesa-libEGL-devel mesa-libGL-devel   # egl.pc/gl.pc are owned by libglvnd-devel on F44
  # ── media stack (mpv / libplacebo / FFmpeg) ──
  luajit-devel libass-devel
  zlib-ng-compat-devel zlib-ng-compat-static          # zlib/zlib-devel were removed on F40+; -static for the all-static link
  glslang-devel libshaderc-devel lcms2-devel          # libplacebo GLSL->SPIR-V + color management
  # ── SDL3 backends + Vulkan-Loader WSI (Wayland/X11/DRM) + audio ──
  wayland-devel wayland-protocols-devel libxkbcommon-devel libdecor-devel
  libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel
  libXfixes-devel libXScrnSaver-devel libxcb-devel
  libdrm-devel mesa-libgbm-devel
  pipewire-devel pulseaudio-libs-devel alsa-lib-devel
  dbus-devel liburing-devel
)

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 1 — system packages
# ═══════════════════════════════════════════════════════════════════════════════
install_packages() {
    step "Phase 1/4 — system packages (dnf5)"
    command -v rpm  >/dev/null 2>&1 || die "rpm not found — this script targets Fedora/Nobara."
    command -v dnf5 >/dev/null 2>&1 || command -v dnf >/dev/null 2>&1 || die "dnf/dnf5 not found."
    local DNF; DNF="$(command -v dnf5 || command -v dnf)"

    # Only install what's actually missing.
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
    # --skip-unavailable: don't abort the whole batch if a name drifts in a future release.
    run_root "$DNF" install "${yes[@]}" --skip-unavailable "${missing[@]}"
    ok "packages installed"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 2 — vcpkg dependencies
# ═══════════════════════════════════════════════════════════════════════════════
install_vcpkg_deps() {
    step "Phase 2/4 — vcpkg deps (curl, libwebp, reflectcpp[toml], taglib)"
    local vcpkg_bin="$VCPKG_ROOT/vcpkg"
    if [[ ! -x "$vcpkg_bin" ]]; then
        if [[ -d "$VCPKG_ROOT/.git" || -f "$VCPKG_ROOT/bootstrap-vcpkg.sh" ]]; then
            log "bootstrapping vcpkg in $VCPKG_ROOT"
            "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
        else
            warn "vcpkg not found at $VCPKG_ROOT — cloning it there"
            git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
            "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
        fi
    fi
    [[ -x "$vcpkg_bin" ]] || die "vcpkg binary still missing at $vcpkg_bin"

    if [[ "$VCPKG_ROOT" != "/home/joao/vcpkg" ]]; then
        warn "the app's root CMakeLists hardcodes /home/joao/vcpkg as the toolchain."
        warn "since you're using $VCPKG_ROOT, configure with:"
        warn "  cmake --preset all -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    fi

    # Classic (non-manifest) mode: MUST run from a dir WITHOUT a vcpkg.json, else vcpkg
    # switches to manifest mode and rejects package args. Run from $VCPKG_ROOT.
    # 'reflectcpp[toml]' is single-quoted so bash doesn't glob the brackets.
    log "vcpkg install (idempotent — already-built ports are skipped)"
    ( cd "$VCPKG_ROOT" && ./vcpkg install curl libwebp 'reflectcpp[toml]' taglib --triplet "$VCPKG_TRIPLET" )
    ok "vcpkg deps ready in $VCPKG_ROOT/installed/$VCPKG_TRIPLET"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 3 — plutovg + plutosvg vendored into external/ (not packaged by Fedora)
# ═══════════════════════════════════════════════════════════════════════════════
# Built as STATIC libs into an in-repo prefix (external/_local). Static -> nothing
# lands in the system and there's no runtime .so to find (no ld.so.conf). The only
# wiring needed is PKG_CONFIG_PATH so the app's pkg_check_modules(PLUTOSVG REQUIRED)
# finds the .pc files (which live outside pkg-config's default dirs).
PLUTO_PKGCONFIG="$PLUTO_PREFIX/lib64/pkgconfig:$PLUTO_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PLUTO_PKGCONFIG${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

pluto_present() {
    pkg-config --exists plutovg plutosvg 2>/dev/null \
        && pkg-config --cflags plutosvg 2>/dev/null | grep -q 'PLUTOSVG_HAS_FREETYPE'
}

build_plutosvg() {
    step "Phase 3/4 — plutovg + plutosvg (FreeType-enabled, static) -> external/"
    if pluto_present; then
        skip "FreeType-enabled plutosvg $(pkg-config --modversion plutosvg) already built in external/"
        return 0
    fi
    command -v cmake >/dev/null 2>&1 || die "cmake missing (run phase 1 first)"
    pkg-config --exists freetype2 || die "freetype2 missing — install freetype-devel (phase 1)."

    # 3a. plutovg (static) -> external/_local. Provides plutovg.pc + the CMake config
    #     plutosvg's find_package(plutovg) resolves against, and the archive the app
    #     links after plutosvg. PIC so it links into the app's PIE executable.
    [[ -d external/plutovg/.git ]] || \
        git clone --depth 1 --branch v1.3.3 https://github.com/sammycage/plutovg.git external/plutovg
    cmake -S external/plutovg -B external/plutovg/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_INSTALL_PREFIX="$PLUTO_PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF -DPLUTOVG_BUILD_EXAMPLES=OFF
    cmake --build external/plutovg/build -j "$JOBS"
    cmake --install external/plutovg/build
    ok "plutovg built + installed to external/_local"

    # 3b. plutosvg (static, FreeType-ON) -> external/_local. --recursive gives a
    #     plutovg submodule fallback (find_package resolves the 3a install first).
    #     FreeType-ON makes plutosvg.pc emit -DPLUTOSVG_HAS_FREETYPE, which exposes
    #     plutosvg_ft_svg_hooks() so color-emoji / COLR glyphs rasterise.
    [[ -d external/plutosvg/.git ]] || \
        git clone --depth 1 --branch v0.0.8 --recursive https://github.com/sammycage/plutosvg.git external/plutosvg
    cmake -S external/plutosvg -B external/plutosvg/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_INSTALL_PREFIX="$PLUTO_PREFIX" -DCMAKE_PREFIX_PATH="$PLUTO_PREFIX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
    cmake --build external/plutosvg/build -j "$JOBS"
    cmake --install external/plutosvg/build
    ok "plutosvg built + installed to external/_local (FreeType-enabled, static)"

    # The app's pkg_check_modules(PLUTOSVG REQUIRED) needs plutosvg.pc on PKG_CONFIG_PATH.
    # It lives in the in-repo prefix (outside pkg-config's default dirs), so register it
    # for future shells — a plain `./build.sh` then finds it. No system files touched;
    # static libs mean there's no runtime .so to locate either.
    local marker="# >>> my_imgui_sdl3_vulkan: external plutosvg pkg-config <<<"
    if [[ -w "$HOME/.bashrc" || ! -e "$HOME/.bashrc" ]] && ! grep -qF "$marker" "$HOME/.bashrc" 2>/dev/null; then
        {
            printf '%s\n' "$marker"
            printf 'export PKG_CONFIG_PATH="%s${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"\n' "$PLUTO_PKGCONFIG"
        } >> "$HOME/.bashrc"
        log "added PKG_CONFIG_PATH -> external/_local in ~/.bashrc (new shell or 'source ~/.bashrc' before building)"
    fi
    ok "plutosvg discovery configured (vendored in external/, no system install)"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 4 — clone external/ sources
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
    step "Phase 4/4 — clone external/ sources (pinned refs)"
    mkdir -p external

    for entry in "${SOURCES[@]}"; do
        IFS='|' read -r dir url ref kind flags <<< "$entry"
        clone_one "$dir" "$url" "$ref" "$kind" "$flags"
    done

    # libplacebo: clone then init ONLY the 5 build-required submodules (skip the
    # demo-only nuklear). Fall back to the GitHub mirror if VideoLAN is unreachable.
    if [[ -d external/libplacebo/.git ]]; then
        skip "external/libplacebo already cloned"
    else
        log "cloning ${C_DIM}$LIBPLACEBO_REF${C_OFF} -> external/libplacebo"
        if ! git clone --depth 1 --branch "$LIBPLACEBO_REF" "$LIBPLACEBO_URL" external/libplacebo; then
            warn "VideoLAN host failed — retrying via mirror $LIBPLACEBO_MIRROR"
            git clone --depth 1 --branch "$LIBPLACEBO_REF" "$LIBPLACEBO_MIRROR" external/libplacebo
        fi
    fi
    log "initialising libplacebo submodules"
    git -C external/libplacebo submodule update --init --depth 1 -- "${LIBPLACEBO_SUBMODULES[@]}"
    ok "all sources present under external/"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase 5 — optional build
# ═══════════════════════════════════════════════════════════════════════════════
build_app() {
    step "Phase 5 — build (Debug) via ./build.sh"
    warn "the first build compiles FFmpeg + mpv + libplacebo + SDL3 from source — expect this to take a while."
    ./build.sh debug
}

# ── run ───────────────────────────────────────────────────────────────────────
log "Setup for ${C_DIM}$SCRIPT_DIR${C_OFF}  (jobs=$JOBS, vcpkg=$VCPKG_ROOT)"
(( DO_PACKAGES )) && install_packages    || skip "phase 1 (packages) skipped"
(( DO_VCPKG ))    && install_vcpkg_deps   || skip "phase 2 (vcpkg) skipped"
(( DO_PLUTOSVG )) && build_plutosvg       || skip "phase 3 (plutosvg) skipped"
(( DO_CLONE ))    && clone_sources        || skip "phase 4 (clone) skipped"

printf '\n'
ok "Dependencies ready."
if (( DO_BUILD )); then
    build_app
    ok "Build complete — run ./build.sh --run to launch."
else
    cat <<EOF

${C_GREEN}Next steps${C_OFF}
  1. plutosvg is vendored + built in ${C_DIM}external/_local${C_OFF} (not the system). ${C_DIM}./build.sh${C_OFF} needs
     PKG_CONFIG_PATH to point there — open a NEW terminal (or ${C_DIM}source ~/.bashrc${C_OFF}), or run:
        ${C_DIM}PKG_CONFIG_PATH="$PLUTO_PKGCONFIG" ./build.sh${C_OFF}
  2. Build everything:   ${C_DIM}./build.sh${C_OFF}          (Debug + Release + RelWithDebInfo)
     …or just Debug:     ${C_DIM}./build.sh debug${C_OFF}
     …and run it:        ${C_DIM}./build.sh debug --run${C_OFF}

  Note: CMake prints a harmless ${C_DIM}"PLUTOSVG_FT_LIBRARY missing; color emoji disabled"${C_OFF}
  warning — the vendored plutosvg IS FreeType-enabled, so emoji work regardless. To
  silence it, configure with ${C_DIM}-DPLUTOSVG_FT_LIBRARY=$PLUTO_PREFIX/lib64/libplutosvg.a${C_OFF}
EOF
fi
