#!/usr/bin/env bash
#
# build.sh — convenience wrapper around the CMake "all" preset
#            (Ninja Multi-Config: Debug, Release, RelWithDebInfo).
#
# Usage:
#   ./build.sh                 Build ALL three configs (default).
#   ./build.sh debug           Build Debug only        -> build/debug
#   ./build.sh release         Build Release only      -> build/release
#   ./build.sh release-log     Build RelWithDebInfo    -> build/release-log
#   ./build.sh all             Build all three explicitly.
#
# Flags (combine freely):
#   --run        Launch the app for the built config after a successful build
#                (defaults to the Debug binary when building 'all').
#   --perf       Profile the app with Linux `perf record` (call-graph dwarf).
#                Prefers the RelWithDebInfo (release-log) binary — optimized
#                code with -g symbols. Writes perf.data.
#   --test       Build and run the test suite (image_tests, image_job_tests).
#   --fresh      Reconfigure from scratch (wipes build/all/.ninja_deps — see the
#                "stale deps" note below) before building.
#   -h|--help    Show this help.
#
set -euo pipefail

# Run from the repo root regardless of where we were invoked from, so the
# relative preset paths below always resolve.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

readonly PRESET="all"
readonly BUILD_DIR="build/all"

# ── pretty output ─────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_BLUE=$'\e[1;34m'; C_GREEN=$'\e[1;32m'; C_YELLOW=$'\e[1;33m'
    C_RED=$'\e[1;31m'; C_DIM=$'\e[2m'; C_OFF=$'\e[0m'
else
    C_BLUE=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_DIM=""; C_OFF=""
fi
log()  { printf '%s==>%s %s\n'  "$C_BLUE"   "$C_OFF" "$*"; }
ok()   { printf '%s ok%s %s\n'  "$C_GREEN"  "$C_OFF" "$*"; }
warn() { printf '%swarn%s %s\n' "$C_YELLOW" "$C_OFF" "$*"; }
die()  { printf '%serr%s %s\n'  "$C_RED"    "$C_OFF" "$*" >&2; exit 1; }

# Print the leading comment header (from line 3 to the first non-comment line),
# stripping the leading "# ". Robust to edits — no hardcoded line numbers.
usage() { awk 'NR>=3 { if ($0 !~ /^#/) exit; sub(/^#( |$)/, ""); print }' "$0"; }

# ── config keyword -> (CMake config, output binary) ───────────────────────────
config_name() {
    case "$1" in
        debug)       echo "Debug" ;;
        release)     echo "Release" ;;
        release-log) echo "RelWithDebInfo" ;;
        *) die "unknown config '$1' (expected: debug | release | release-log)" ;;
    esac
}
binary_path() {
    case "$1" in
        debug)       echo "build/debug/example_sdl3_vulkan_debug" ;;
        release)     echo "build/release/example_sdl3_vulkan_release" ;;
        release-log) echo "build/release-log/example_sdl3_vulkan_release-log" ;;
    esac
}

# ── steps ─────────────────────────────────────────────────────────────────────
configure() {
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        log "Configuring preset '$PRESET' (first run)..."
        cmake --preset "$PRESET"
    fi
}

# Known gotcha (see project memory "Ninja deps rebuild gotcha"): a corrupt
# build/all/.ninja_deps makes every build recompile the whole app + SDL3 +
# thirdparty. The cure is to delete that one file and run a clean build — NOT to
# nuke CMake or re-glob.
#
# ┌─ YOUR CONTRIBUTION ─────────────────────────────────────────────────────────┐
# │ How aggressively should the script self-heal from this?                      │
# │                                                                              │
# │ Implement `ninja_deps_looks_stale` to return 0 (stale -> auto-reset) or      │
# │ 1 (fine). You have firsthand knowledge of when this file goes bad, so the    │
# │ policy is yours. Trade-off:                                                   │
# │   • An automatic heuristic = no surprise full rebuilds, but a wrong guess     │
# │     throws away good incremental state and costs a slow rebuild.              │
# │   • Returning 1 always = predictable; you reset manually with --fresh when    │
# │     you notice the symptom.                                                   │
# │                                                                              │
# │ Ideas for a heuristic (pick one or invent your own — ~5-10 lines):           │
# │   • size/age: a many-MB .ninja_deps that's older than build.ninja is suspect │
# │   • probe: a no-op `ninja -n` (dry run) that lists ~every object = corrupt    │
# │   • sentinel: compare against a hash you stamp after each clean build         │
# └──────────────────────────────────────────────────────────────────────────────┘
ninja_deps_looks_stale() {
    # TODO(you): replace this conservative default with your chosen heuristic.
    return 1   # 1 = "looks fine, leave it alone"
}

reset_ninja_deps() {
    warn "Resetting $BUILD_DIR/.ninja_deps (forces one clean build)"
    rm -f "$BUILD_DIR/.ninja_deps"
}

build_config() {
    local cfg="$1" name
    name="$(config_name "$cfg")"
    log "Building ${C_DIM}$name${C_OFF}  ->  $(binary_path "$cfg")"
    cmake --build "$BUILD_DIR" --config "$name"
    ok "Built $name"
}

run_app() {
    local cfg="$1" bin
    bin="$(binary_path "$cfg")"
    [[ -x "$bin" ]] || die "binary not found: $bin (did the build succeed?)"
    log "Running $bin"
    "./$bin"
}

run_perf() {
    local cfg="$1" bin
    bin="$(binary_path "$cfg")"
    [[ -x "$bin" ]] || die "binary not found: $bin (did the build succeed?)"
    command -v perf >/dev/null 2>&1 || die "perf not found — install the linux 'perf' tools"
    [[ "$cfg" == "release-log" ]] || \
        warn "profiling '$cfg' — RelWithDebInfo (release-log) gives the truest hotspots"
    # --call-graph dwarf: optimized builds omit frame pointers, so unwind via the
    # DWARF CFI that -g emits instead of walking fp chains (which would be broken).
    log "Profiling $bin under perf record (call-graph dwarf)"
    perf record -g --call-graph dwarf -o perf.data -- "./$bin"
    ok "Wrote perf.data — view with:  perf report -i perf.data"
}

run_tests() {
    log "Building + running tests (image_tests, image_job_tests)"
    cmake --build "$BUILD_DIR" --config Debug --target image_tests image_job_tests
    ctest --test-dir "$BUILD_DIR" -C Debug --output-on-failure
    ok "Tests passed"
}

# ── arg parsing ───────────────────────────────────────────────────────────────
DO_RUN=0
DO_PERF=0
DO_TEST=0
DO_FRESH=0
CONFIGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        debug|release|release-log) CONFIGS+=("$1") ;;
        all)        CONFIGS=(debug release release-log) ;;
        --run)      DO_RUN=1 ;;
        --perf)     DO_PERF=1 ;;
        --test)     DO_TEST=1 ;;
        --fresh)    DO_FRESH=1 ;;
        -h|--help)  usage; exit 0 ;;
        *)          die "unknown argument '$1' (try --help)" ;;
    esac
    shift
done

# Default: build everything.
[[ ${#CONFIGS[@]} -eq 0 ]] && CONFIGS=(debug release release-log)

# ── run ───────────────────────────────────────────────────────────────────────
(( DO_FRESH )) && reset_ninja_deps
configure
if ninja_deps_looks_stale; then
    reset_ninja_deps
fi

for cfg in "${CONFIGS[@]}"; do
    build_config "$cfg"
done

(( DO_TEST )) && run_tests

if (( DO_PERF )); then
    # Profile the optimized RelWithDebInfo build when it's among those built;
    # otherwise fall back to whatever was built first (with a warning).
    perf_cfg="${CONFIGS[0]}"
    [[ " ${CONFIGS[*]} " == *" release-log "* ]] && perf_cfg="release-log"
    run_perf "$perf_cfg"
fi

if (( DO_RUN )); then
    # Run the requested config; when several were built, default to debug.
    run_cfg="${CONFIGS[0]}"
    [[ " ${CONFIGS[*]} " == *" debug "* ]] && run_cfg="debug"
    run_app "$run_cfg"
fi

ok "Done."
