#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
doctor.py — roda todos os doctors e resume o que cada um achou.

  python3 scripts/doctor.py                 tudo, relatório completo
  python3 scripts/doctor.py --quiet          só o resumo por área
  python3 scripts/doctor.py --only vk,build  escolhe áreas
  python3 scripts/doctor.py --json           junta o JSON de todos

Áreas (cada uma é um programa independente, roda sozinha também):
  vk       scripts/vk_doctor.py       loader, camadas, ICDs, vkCreateInstance real
  build    scripts/build_doctor.py    preset, ninja deps, PCH, vcpkg (portas+features)
  media    scripts/media_doctor.py    ffmpeg/mpv/libplacebo instalados, hwaccels, NVIDIA
  runtime  scripts/runtime_doctor.py  binários, ldd, RUNPATH, qual config pede validação

Convenção: exit 0 = nada grave; 1 = pelo menos um achado grave. O doctor.py sai
com 1 se qualquer área saiu com 1.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List

HERE = Path(__file__).resolve().parent
AREAS = {
    "vk": ("vk_doctor.py", "Vulkan: loader, camadas e vkCreateInstance"),
    "build": ("build_doctor.py", "Build: preset, ninja, PCH, vcpkg"),
    "media": ("media_doctor.py", "Mídia: ffmpeg/mpv/libplacebo e GPU"),
    "runtime": ("runtime_doctor.py", "Runtime: binários e o que falta para subir"),
}

TTY = sys.stdout.isatty()
BLUE = "\x1b[1;34m" if TTY else ""
GREEN = "\x1b[1;32m" if TTY else ""
RED = "\x1b[1;31m" if TTY else ""
DIM = "\x1b[2m" if TTY else ""
OFF = "\x1b[0m" if TTY else ""


def run_area(area: str, as_json: bool, quiet: bool) -> Dict[str, object]:
    script = HERE / AREAS[area][0]
    if not script.is_file():
        return {"area": area, "rc": 127, "output": f"{script} não existe"}
    cmd = [sys.executable, str(script)] + (["--json"] if as_json else [])
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = (proc.stdout or "") + (proc.stderr or "")
    if not as_json and not quiet:
        print(f"\n{BLUE}{'═' * 78}{OFF}")
        print(f"{BLUE}║ {area}  —  {AREAS[area][1]}{OFF}")
        print(f"{BLUE}{'═' * 78}{OFF}")
        print(out.rstrip())
    return {"area": area, "rc": proc.returncode, "output": out}


def verdict_lines(output: str) -> List[str]:
    """As linhas do bloco 'veredito' de um doctor, para o resumo."""
    keep, out = False, []
    for ln in output.splitlines():
        if "veredito" in ln:
            keep = True
            continue
        if keep and ln.strip():
            out.append(ln.strip())
    return out


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="doctor.py")
    p.add_argument("--only", default="", help="áreas separadas por vírgula: "
                                              + ", ".join(AREAS))
    p.add_argument("--quiet", action="store_true", help="só o resumo")
    p.add_argument("--json", action="store_true", help="junta o JSON de todos")
    args = p.parse_args(argv)

    chosen = [a.strip() for a in args.only.split(",") if a.strip()] or list(AREAS)
    for a in chosen:
        if a not in AREAS:
            p.error(f"área desconhecida '{a}' (use: {', '.join(AREAS)})")

    results = [run_area(a, args.json, args.quiet) for a in chosen]

    if args.json:
        merged = {}
        for r in results:
            try:
                merged[r["area"]] = json.loads(str(r["output"]))
            except json.JSONDecodeError:
                merged[r["area"]] = {"raw": r["output"], "rc": r["rc"]}
            merged[r["area"]]["rc"] = r["rc"]
        print(json.dumps(merged, indent=2, ensure_ascii=False))
        return 1 if any(r["rc"] for r in results) else 0

    print(f"\n{BLUE}{'═' * 78}{OFF}\n{BLUE}║ resumo{OFF}\n{BLUE}{'═' * 78}{OFF}")
    for r in results:
        rc = int(r["rc"])                                  # type: ignore[arg-type]
        mark = f"{GREEN}ok{OFF}" if rc == 0 else f"{RED}achou algo{OFF}"
        print(f"  {r['area']:<9} {mark}")
        for ln in verdict_lines(str(r["output"]))[:6]:
            print(f"      {DIM}{ln[:120]}{OFF}")
    return 1 if any(r["rc"] for r in results) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
