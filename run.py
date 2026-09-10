#!/usr/bin/env python3
"""
run.py — pick a build config with the arrow keys and launch the app.

Usage:
    ./run.py                  Interactive menu (up/down + Enter).
    ./run.py debug            Skip the menu, run that config straight away.
    ./run.py release -- --foo Everything after `--` is forwarded to the app.

Keys in the menu:
    up / down (or k / j)  move          Enter  run the highlighted config
    b                     build it first (build.py <cfg>), then run
    r                     rebuild it    (build.py <cfg> --rebuild), then run
    q / Esc               quit

Config -> binary mapping mirrors CMakeLists.txt (RUNTIME_OUTPUT_DIRECTORY_* /
OUTPUT_NAME_*); build.sh keeps the same three configs.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EXE_SUFFIX = ".exe" if os.name == "nt" else ""


@dataclass(frozen=True)
class Config:
    key: str        # ./build.sh keyword
    cmake: str      # CMake configuration name
    out_dir: str    # RUNTIME_OUTPUT_DIRECTORY_<CFG>, relative to the repo root
    exe: str        # OUTPUT_NAME_<CFG>
    blurb: str

    @property
    def binary(self) -> Path:
        return ROOT / self.out_dir / (self.exe + EXE_SUFFIX)


CONFIGS = (
    Config("debug", "Debug", "build/debug", "example_sdl3_vulkan_debug",
           "assertions + full symbols, slowest"),
    Config("release", "Release", "build/release", "example_sdl3_vulkan_release",
           "optimized, no symbols"),
    Config("release-log", "RelWithDebInfo", "build/RELWITHDEBINFO",
           "example_sdl3_vulkan_RELWITHDEBINFO",
           "optimized + symbols, best for perf"),
)

# ── pretty output ─────────────────────────────────────────────────────────────
TTY = sys.stdout.isatty()
BLUE = "\033[1;34m" if TTY else ""
GREEN = "\033[1;32m" if TTY else ""
YELLOW = "\033[1;33m" if TTY else ""
RED = "\033[1;31m" if TTY else ""
DIM = "\033[2m" if TTY else ""
REV = "\033[7m" if TTY else ""
OFF = "\033[0m" if TTY else ""
HIDE_CURSOR = "\033[?25l" if TTY else ""
SHOW_CURSOR = "\033[?25h" if TTY else ""


def log(msg: str) -> None:
    print(f"{BLUE}==>{OFF} {msg}")


def die(msg: str):
    print(f"{RED}err{OFF} {msg}", file=sys.stderr)
    raise SystemExit(1)


def describe(cfg: Config) -> str:
    """Right-hand column: freshness of the binary, or that there is none."""
    binary = cfg.binary
    if not binary.exists():
        return f"{YELLOW}not built{OFF}"
    st = binary.stat()
    when = time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))
    return f"{DIM}{when}  {st.st_size / 1e6:,.0f} MB{OFF}"


# ── key input ─────────────────────────────────────────────────────────────────
# Returns one of: 'up', 'down', 'enter', 'quit', or the literal character typed.
if os.name == "nt":
    import msvcrt

    def read_key() -> str:
        ch = msvcrt.getwch()
        if ch in ("\x00", "\xe0"):          # arrow keys arrive as a 2-char code
            return {"H": "up", "P": "down"}.get(msvcrt.getwch(), "")
        if ch in ("\r", "\n"):
            return "enter"
        if ch in ("\x1b", "\x03"):          # Esc, Ctrl-C
            return "quit"
        return ch.lower()

    def raw_mode():
        from contextlib import nullcontext
        return nullcontext()

else:
    import termios
    import tty
    from contextlib import contextmanager
    from select import select

    @contextmanager
    def raw_mode():
        fd = sys.stdin.fileno()
        saved = termios.tcgetattr(fd)
        try:
            # cbreak, not raw: it turns off canonical mode + echo (so keys arrive
            # one at a time, unechoed) while KEEPING output post-processing, so a
            # printed "\n" still carries a carriage return and the menu does not
            # walk off to the right. ISIG stays on, so Ctrl-C still interrupts.
            tty.setcbreak(fd)
            yield
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, saved)

    def read_key() -> str:
        # Read the raw fd, never sys.stdin: the text wrapper buffers ahead, so
        # the rest of an escape sequence would sit in Python's buffer where the
        # select() below cannot see it, and every arrow key would look like Esc.
        fd = sys.stdin.fileno()
        ch = os.read(fd, 1).decode(errors="replace")
        if ch == "\x1b":
            # Esc alone vs. an escape sequence: a real arrow key sends the rest
            # immediately, so nothing pending within ~50 ms means a bare Esc.
            if not select([fd], [], [], 0.05)[0]:
                return "quit"
            seq = os.read(fd, 2).decode(errors="replace")
            return {"[A": "up", "[B": "down"}.get(seq, "")
        if ch in ("\r", "\n"):
            return "enter"
        if ch == "\x03":                     # Ctrl-C
            return "quit"
        return ch.lower()


# ── menu ──────────────────────────────────────────────────────────────────────
HEADER = "Which build do you want to run?"
FOOTER = ("↑/↓ move · Enter run · b build+run · r rebuild+run · q quit"
          if TTY else "")


def draw(selected: int, first: bool) -> None:
    lines = len(CONFIGS) + 3
    if not first:
        sys.stdout.write(f"\033[{lines}A")   # rewind over the previous frame
    print(f"\033[2K{HEADER}")
    for i, cfg in enumerate(CONFIGS):
        marker = f"{GREEN}>{OFF}" if i == selected else " "
        label = f"{cfg.cmake:<16}"
        label = f"{REV}{label}{OFF}" if i == selected else label
        print(f"\033[2K {marker} {label} {describe(cfg)}  {DIM}{cfg.blurb}{OFF}")
    print("\033[2K")
    print(f"\033[2K{DIM}{FOOTER}{OFF}")


def choose() -> "tuple[Config, str] | None":
    """Menu loop. Returns (config, action) where action is run|build|rebuild."""
    if not sys.stdin.isatty():
        die("stdin is not a terminal — pass a config instead: ./run.py debug")

    selected = next((i for i, c in enumerate(CONFIGS) if c.binary.exists()), 0)
    sys.stdout.write(HIDE_CURSOR)
    try:
        with raw_mode():
            first = True
            while True:
                draw(selected, first)
                first = False
                key = read_key()
                if key in ("quit", "q"):
                    return None
                if key in ("up", "k"):
                    selected = (selected - 1) % len(CONFIGS)
                elif key in ("down", "j"):
                    selected = (selected + 1) % len(CONFIGS)
                elif key == "enter":
                    return CONFIGS[selected], "run"
                elif key == "b":
                    return CONFIGS[selected], "build"
                elif key == "r":
                    return CONFIGS[selected], "rebuild"
    finally:
        sys.stdout.write(SHOW_CURSOR)
        sys.stdout.flush()


# ── actions ───────────────────────────────────────────────────────────────────
def build(cfg: Config, rebuild: bool) -> None:
    # build.py first: it is the cross-platform port and needs no bash. build.sh
    # stays as the fallback for a checkout that only has the shell script.
    script = ROOT / "build.py"
    if script.exists():
        cmd = [sys.executable, str(script), cfg.key]
    elif (ROOT / "build.sh").exists() and os.name != "nt":
        cmd = [str(ROOT / "build.sh"), cfg.key]
    else:
        die(f"neither build.py nor build.sh found — build manually with: "
            f"cmake --build build/all --config {cfg.cmake}")
    cmd += ["--rebuild"] if rebuild else []
    log(f"Building {DIM}{cfg.cmake}{OFF} ({' '.join(cmd[1:])})")
    rc = subprocess.call(cmd, cwd=ROOT)
    if rc != 0:
        die(f"build failed (exit {rc})")


def run(cfg: Config, app_args: "list[str]") -> int:
    binary = cfg.binary
    if not binary.exists():
        die(f"binary not found: {binary.relative_to(ROOT)} — build it first "
            f"(./run.py and press 'b', or ./build.py {cfg.key})")
    if not os.access(binary, os.X_OK):
        die(f"not executable: {binary.relative_to(ROOT)}")
    log(f"Running {binary.relative_to(ROOT)}"
        + (f" {' '.join(app_args)}" if app_args else ""))
    # cwd = repo root: the app resolves assets and build/cache relative to it.
    return subprocess.call([str(binary), *app_args], cwd=ROOT)


def main(argv: "list[str]") -> int:
    app_args: "list[str]" = []
    if "--" in argv:
        cut = argv.index("--")
        argv, app_args = argv[:cut], argv[cut + 1:]

    if argv and argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0

    action = "run"
    if argv:
        name = argv[0]
        cfg = next((c for c in CONFIGS
                    if name.lower() in (c.key, c.cmake.lower())), None)
        if cfg is None:
            die(f"unknown config '{name}' (expected: "
                + " | ".join(c.key for c in CONFIGS) + ")")
        for flag in argv[1:]:
            if flag == "--build":
                action = "build"
            elif flag == "--rebuild":
                action = "rebuild"
            else:
                die(f"unknown argument '{flag}' (try --help)")
    else:
        picked = choose()
        if picked is None:
            return 0
        cfg, action = picked

    if action in ("build", "rebuild"):
        build(cfg, rebuild=action == "rebuild")

    rc = run(cfg, app_args)
    if rc != 0:
        print(f"{YELLOW}warn{OFF} app exited with code {rc}")
    return rc


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.stdout.write(SHOW_CURSOR)
        print()
        raise SystemExit(130)
