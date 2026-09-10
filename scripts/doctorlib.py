#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
doctorlib.py — peças comuns dos doctors (scripts/*_doctor.py).

Cada doctor é um programa independente com o mesmo contrato:
  * roda sem argumento e imprime um relatório com um "veredito" no fim;
  * aceita --json e cospe a mesma informação estruturada;
  * sai com 0 quando não achou nada grave e 1 quando achou.

O scripts/doctor.py roda todos e junta os vereditos.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO = Path(__file__).resolve().parent.parent
CONFIGS = ("Debug", "Release", "RelWithDebInfo")
BUILD_DIR = REPO / "build" / "all"

# Onde cada config deposita o executável (CMakeLists.txt:293-299).
BINARIES = {
    "Debug": REPO / "build/debug/example_sdl3_vulkan_debug",
    "Release": REPO / "build/release/example_sdl3_vulkan_release",
    "RelWithDebInfo": REPO / "build/RELWITHDEBINFO/example_sdl3_vulkan_RELWITHDEBINFO",
}

TTY = sys.stdout.isatty()
BLUE = "\x1b[1;34m" if TTY else ""
GREEN = "\x1b[1;32m" if TTY else ""
YELLOW = "\x1b[1;33m" if TTY else ""
RED = "\x1b[1;31m" if TTY else ""
DIM = "\x1b[2m" if TTY else ""
OFF = "\x1b[0m" if TTY else ""

OK = f"{GREEN}ok{OFF}"
BAD = f"{RED}falta{OFF}"


def head(msg: str) -> None:
    print(f"\n{BLUE}── {msg}{OFF}")


def line(label: str, value: str, width: int = 34) -> None:
    print(f"  {label:<{width}} {value}")


class Findings:
    """Acumula o que está errado. Severidade 'erro' derruba o exit code."""

    def __init__(self) -> None:
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.notes: List[str] = []

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    def report(self) -> int:
        head("veredito")
        if not self.errors and not self.warnings:
            print(f"  {GREEN}nada a consertar{OFF}")
        for m in self.errors:
            print(f"  {RED}erro{OFF}  {m}")
        for m in self.warnings:
            print(f"  {YELLOW}aviso{OFF} {m}")
        for m in self.notes:
            print(f"  {DIM}nota  {m}{OFF}")
        return 1 if self.errors else 0

    def as_dict(self) -> dict:
        return {"errors": self.errors, "warnings": self.warnings, "notes": self.notes}


def capture(cmd: Sequence[str], cwd: Optional[Path] = None,
            timeout: int = 60) -> Tuple[int, str]:
    """Roda e captura stdout+stderr; nunca levanta."""
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=str(cwd or REPO),
                           capture_output=True, text=True, errors="replace",
                           timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        return 127, f"{type(exc).__name__}: {exc}"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def tool_version(exe: str, *args: str) -> str:
    path = shutil.which(exe)
    if not path:
        return f"{RED}ausente{OFF}"
    rc, out = capture([path, *(args or ("--version",))], timeout=20)
    first = next((l for l in out.splitlines() if l.strip()), "")
    return f"{first.strip()[:60]}  {DIM}{path}{OFF}"


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:,.0f} {unit}" if unit == "B" else f"{n:,.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GB"


def stamp(path: Path) -> str:
    try:
        st = path.stat()
    except OSError:
        return f"{RED}não existe{OFF}"
    return (f"{human(st.st_size)}  "
            f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(st.st_mtime))}")


def mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def exists_mark(path: Path) -> str:
    return OK if path.exists() else BAD


def read_cache(build_dir: Path = BUILD_DIR) -> Dict[str, str]:
    """CMakeCache.txt como dicionário chave->valor (ignora o tipo)."""
    cache = build_dir / "CMakeCache.txt"
    out: Dict[str, str] = {}
    if not cache.is_file():
        return out
    for ln in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith(("#", "//")) or "=" not in ln:
            continue
        key, _, value = ln.partition("=")
        out[key.split(":")[0]] = value
    return out


def pkgconfig_version(pc_file: Path) -> Optional[str]:
    """Versão declarada em um .pc, sem depender do pkg-config estar no PATH."""
    if not pc_file.is_file():
        return None
    for ln in pc_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.lower().startswith("version:"):
            return ln.split(":", 1)[1].strip()
    return None


def ldd_missing(binary: Path) -> List[str]:
    """Bibliotecas que o ld.so não resolve para este ELF."""
    if not binary.is_file() or not shutil.which("ldd"):
        return []
    rc, out = capture(["ldd", str(binary)], timeout=90)
    return [ln.strip() for ln in out.splitlines() if "not found" in ln]


def elf_strings(binary: Path, needles: Iterable[str],
                chunk: int = 1 << 22) -> Dict[str, bool]:
    """Procura literais dentro do binário — jeito barato de saber qual flag de
    compilação entrou (ex.: o nome da camada de validação só existe no ELF se o
    APP_USE_VULKAN_DEBUG_REPORT estava ligado)."""
    found = {n: False for n in needles}
    if not binary.is_file():
        return found
    encoded = {n: n.encode() for n in found}
    tail = b""
    with binary.open("rb") as fh:
        while block := fh.read(chunk):
            buf = tail + block
            for name, raw in encoded.items():
                if not found[name] and raw in buf:
                    found[name] = True
            if all(found.values()):
                break
            tail = buf[-256:]
    return found


def conda_shadow() -> List[str]:
    """Um env conda ativo pode pôr cmake/ninja/qmake na frente dos do sistema —
    a árvore build/all foi configurada com um cmake específico e trocar de
    binário no meio do caminho regenera tudo."""
    prefix = os.environ.get("CONDA_PREFIX")
    if not prefix:
        return []
    hits = []
    for tool in ("cmake", "ninja", "qmake", "qmake6", "clang", "pkg-config"):
        found = shutil.which(tool)
        if found and found.startswith(prefix):
            hits.append(f"{tool} -> {found}")
    return hits


def main_wrapper(collect, render, argv: List[str]) -> int:
    """CLI comum: --json imprime o dicionário, senão renderiza o relatório."""
    import argparse
    import json

    p = argparse.ArgumentParser()
    p.add_argument("--json", action="store_true", help="saída estruturada")
    args = p.parse_args(argv)
    data, findings = collect()
    if args.json:
        print(json.dumps({**data, "verdict": findings.as_dict()}, indent=2,
                         ensure_ascii=False, default=str))
        return 1 if findings.errors else 0
    render(data)
    return findings.report()
