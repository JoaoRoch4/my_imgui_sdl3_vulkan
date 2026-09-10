#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
runtime_doctor.py — os binários prontos e o que eles precisam para subir.

  python3 scripts/runtime_doctor.py            relatório
  python3 scripts/runtime_doctor.py --json     estruturado

Olha cada executável construído e responde o que só o ELF sabe: quais .so o
ld.so não resolve, qual RUNPATH ficou gravado, e — o truque que fecha o caso do
VK_ERROR_LAYER_NOT_PRESENT — se o literal "VK_LAYER_KHRONOS_validation" está
dentro do binário, o que revela se aquele config foi compilado com o
APP_USE_VULKAN_DEBUG_REPORT ligado (CMakeLists: Debug e RelWithDebInfo).

Com isso dá para dizer, antes de rodar, que um config específico vai morrer com
-6: o binário pede a camada e o loader não a tem. Essa segunda metade vem do
vk_doctor.py, chamado aqui em modo --json.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from doctorlib import (BAD, BINARIES, CONFIGS, DIM, GREEN, OFF, OK, RED,  # noqa: E402
                       REPO, YELLOW, Findings, capture, elf_strings, head,
                       human, ldd_missing, line, main_wrapper, stamp)

VK_LAYER = "VK_LAYER_KHRONOS_validation"
# Marcador do APP_USE_VULKAN_DEBUG_REPORT: medido, o literal da CAMADA sozinho
# não serve — ele aparece até no Release, vindo do loader vendorizado em
# build/all/thirdparty/vulkan-loader. Já "VK_EXT_debug_report" só entra no ELF
# pelo push do #ifdef (vulkan_context.cpp:112), e bate com a genexp do
# CMakeLists (Debug e RelWithDebInfo, nunca Release).
VK_DEBUG_EXT = "VK_EXT_debug_report"
NEEDLES = [VK_LAYER, VK_DEBUG_EXT, "VK_KHR_swapchain", "libmpv", "placebo"]

# O app procura o cache em SDL_GetBasePath()/../../cache — como os binários
# ficam dois níveis abaixo da raiz, os três configs compartilham <repo>/cache
# (CMakeLists.txt:283-285): window_state.toml, thumbnails e cache de vídeo.
CACHE_DIR = REPO / "cache"

RUN_ENV = ["LD_LIBRARY_PATH", "LD_PRELOAD", "VK_INSTANCE_LAYERS", "VK_LOADER_DEBUG",
           "MANGOHUD", "ENABLE_VKBASALT", "__NV_PRIME_RENDER_OFFLOAD",
           "__GLX_VENDOR_LIBRARY_NAME", "DRI_PRIME", "MESA_LOADER_DRIVER_OVERRIDE",
           "XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY", "SDL_VIDEODRIVER"]


def runpath(binary: Path) -> str:
    if not binary.is_file() or not shutil.which("readelf"):
        return ""
    rc, out = capture(["readelf", "-d", str(binary)], timeout=60)
    for ln in out.splitlines():
        if "RUNPATH" in ln or "RPATH" in ln:
            return ln.split("[", 1)[-1].rstrip("]").strip()
    return ""


def vk_layers_available() -> Tuple[bool, List[str]]:
    """Pergunta ao vk_doctor quais camadas o loader enxerga (ele fala com a
    libvulkan de verdade; aqui não repetimos a ctypes)."""
    doctor = Path(__file__).resolve().parent / "vk_doctor.py"
    if not doctor.is_file():
        return False, []
    rc, out = capture([sys.executable, str(doctor), "layers", "--json"], timeout=90)
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return False, []
    return True, [l["name"] for l in (data.get("layers") or {}).get("list", [])]


def collect() -> Tuple[dict, Findings]:
    f = Findings()
    ok_vk, layers = vk_layers_available()
    data: dict = {
        "binaries": {}, "cache_dir": str(CACHE_DIR),
        "cache_exists": CACHE_DIR.is_dir(),
        "cache_entries": sorted(p.name for p in CACHE_DIR.iterdir())[:12]
        if CACHE_DIR.is_dir() else [],
        "vk_layers_queried": ok_vk, "vk_layers": layers,
        "env": {k: os.environ[k] for k in RUN_ENV if k in os.environ},
    }

    built = 0
    for config in CONFIGS:
        binary = BINARIES[config]
        entry: dict = {"path": str(binary), "exists": binary.is_file()}
        if binary.is_file():
            built += 1
            st = binary.stat()
            entry.update({
                "size": st.st_size, "mtime": st.st_mtime,
                "executable": os.access(binary, os.X_OK),
                "runpath": runpath(binary),
                "missing_libs": ldd_missing(binary),
                "strings": elf_strings(binary, NEEDLES),
            })
            if entry["missing_libs"]:
                f.error(f"{config}: o ld.so não resolve "
                        + ", ".join(l.split()[0] for l in entry["missing_libs"]))
            if not entry["executable"]:
                f.error(f"{config}: {binary.name} não tem bit de execução")
            # O binário pede a camada de validação? Então ela precisa existir.
            wants_layer = (entry["strings"].get(VK_DEBUG_EXT, False)
                           and entry["strings"].get(VK_LAYER, False))
            entry["wants_validation"] = wants_layer
            if wants_layer and ok_vk and VK_LAYER not in layers:
                f.error(f"{config}: o binário pede {VK_LAYER} e o loader não a "
                        "tem — este config vai morrer no vkCreateInstance com "
                        "-6 (VK_ERROR_LAYER_NOT_PRESENT). Detalhe completo em "
                        "scripts/vk_doctor.py")
        data["binaries"][config] = entry

    if built == 0:
        f.error("nenhum config compilado — `python3 build.py` antes de rodar")
    elif built < len(CONFIGS):
        f.note(f"{built} de {len(CONFIGS)} configs compilados")

    if not data["cache_exists"]:
        f.note(f"{CACHE_DIR.relative_to(REPO)}/ ainda não existe — o app o cria "
               "na primeira execução (window_state.toml, thumbnails, vídeo)")

    session = os.environ.get("XDG_SESSION_TYPE", "")
    if session == "wayland" and not os.environ.get("WAYLAND_DISPLAY"):
        f.warn("sessão wayland sem WAYLAND_DISPLAY — o SDL vai cair para X11/erro")
    if not session and not os.environ.get("DISPLAY"):
        f.warn("sem sessão gráfica visível (nem DISPLAY nem WAYLAND_DISPLAY): "
               "o app não abre janela daqui")
    if os.environ.get("MANGOHUD") or os.environ.get("ENABLE_VKBASALT"):
        f.note("overlay ativo no ambiente (MangoHud/vkBasalt) — camadas "
               "implícitas entram na instância e mudam o que a validação vê")
    return data, f


def render(d: dict) -> None:
    print(f"{DIM}runtime_doctor — binários de build/<config>/{OFF}")

    for config in CONFIGS:
        e = d["binaries"][config]
        head(config)
        if not e["exists"]:
            line("binário", f"{BAD}  {DIM}{e['path']}{OFF}")
            continue
        line("binário", f"{human(e['size'])}  {DIM}{e['path']}{OFF}")
        line("executável", OK if e["executable"] else BAD)
        line("RUNPATH", e["runpath"] or f"{DIM}<nenhum>{OFF}")
        line("libs não resolvidas", ", ".join(e["missing_libs"]) or OK)
        marks = "  ".join(
            f"{GREEN if v else DIM}{k}{'' if v else ' (não)'}{OFF}"
            for k, v in e["strings"].items())
        line("literais no ELF", marks)
        line("debug report compilado",
             f"{YELLOW}sim{OFF} (pede a camada de validação)"
             if e.get("wants_validation") else f"{DIM}não{OFF}")

    head("camadas que o loader oferece (via vk_doctor)")
    if not d["vk_layers_queried"]:
        print(f"  {YELLOW}não consegui perguntar ao vk_doctor.py{OFF}")
    else:
        for name in d["vk_layers"]:
            mark = f"{GREEN}*{OFF}" if name == VK_LAYER else " "
            print(f" {mark} {name}")

    head("cache compartilhado")
    line(d["cache_dir"], (", ".join(d["cache_entries"]) or f"{DIM}vazio{OFF}")
         if d["cache_exists"] else f"{DIM}ainda não criado{OFF}")

    if d["env"]:
        head("ambiente que muda a execução")
        for k, v in d["env"].items():
            line(k, v[:80])


if __name__ == "__main__":
    raise SystemExit(main_wrapper(collect, render, sys.argv[1:]))
