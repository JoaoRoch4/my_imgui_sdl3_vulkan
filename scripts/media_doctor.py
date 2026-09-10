#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
media_doctor.py — a stack de mídia dos ExternalProjects, por configuração.

  python3 scripts/media_doctor.py            relatório
  python3 scripts/media_doctor.py --json     estruturado

O app compila contra headers INSTALADOS pelos EPs (ffmpeg, mpv, libplacebo,
ffmpegthumbnailer), e o LINK_DEPENDS ordena o link e não a compilação — é daí
que vem o "'mpv/client.h' file not found" num build paralelo fresco. Este doctor
olha cada árvore de install: header existe, .a existe, qual versão o .pc declara,
e quais hwaccels o FFmpeg realmente ficou com (config.h do build, não o que o
CMakeLists pediu).

Também confere o que a rota zero-copy precisa do sistema (Vulkan Video, CUDA/
NVDEC, EGL/GL para o caminho libplacebo) e se o driver NVIDIA está de pé.
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from doctorlib import (BAD, BUILD_DIR, CONFIGS, DIM, GREEN, OFF, OK, RED,  # noqa: E402
                       REPO, YELLOW, Findings, capture, head, human, line,
                       main_wrapper, pkgconfig_version, stamp)

THIRDPARTY = BUILD_DIR / "thirdparty"

# EP -> onde mora o header que o app inclui, a lib que o link espera e o .pc.
# "origem" do header: "install" = a árvore instalada pelo EP; "source" = o
# checkout em external/. O ffmpegthumbnailer é do segundo tipo — ele instala só
# a .a, e o app inclui <libffmpegthumbnailer/videothumbnailerc.h> direto do fonte
# (medido em external/, e nenhum -I de install aparece no compile_commands).
PROJECTS = {
    "ffmpeg": {"header": ("install", "include/libavcodec/avcodec.h"),
               "lib": "lib/libavcodec.a", "pc": "lib/pkgconfig/libavcodec.pc"},
    "mpv": {"header": ("install", "include/mpv/client.h"),
            "lib": "lib/libmpv.a", "pc": "lib/pkgconfig/mpv.pc"},
    "libplacebo": {"header": ("install", "include/libplacebo/renderer.h"),
                   "lib": "lib/libplacebo.a", "pc": "lib/pkgconfig/libplacebo.pc"},
    "ffmpegthumbnailer": {
        "header": ("source", "external/ffmpegthumbnailer/libffmpegthumbnailer/videothumbnailerc.h"),
        "lib": "lib/libffmpegthumbnailer.a", "pc": None},
}

# O que decide se a rota Vulkan-Video/NVDEC existe de verdade no FFmpeg build.
FFMPEG_FLAGS = ["CONFIG_VULKAN", "CONFIG_LIBPLACEBO", "CONFIG_CUDA", "CONFIG_CUVID",
                "CONFIG_NVDEC", "CONFIG_FFNVCODEC", "CONFIG_LIBDRM", "CONFIG_VAAPI",
                "CONFIG_H264_VULKAN_HWACCEL", "CONFIG_HEVC_VULKAN_HWACCEL",
                "CONFIG_AV1_VULKAN_HWACCEL", "CONFIG_H264_NVDEC_HWACCEL"]

SYSTEM_LIBS = ["libEGL.so.1", "libGL.so.1", "libdrm.so.2", "libvulkan.so.1",
               "libwayland-client.so.0", "libX11.so.6", "libnvidia-encode.so.1"]


def install_root(project: str, config: str) -> Path:
    return THIRDPARTY / project / config / "install"


def probe_project(project: str, config: str) -> dict:
    root = install_root(project, config)
    spec = PROJECTS[project]
    origin, header = spec["header"]
    lib, pc = spec["lib"], spec["pc"]
    hp = (REPO / header) if origin == "source" else (root / header)
    lp = root / lib
    entry = {
        "root": str(root), "installed": root.is_dir(),
        "header": str(hp), "header_ok": hp.is_file(), "header_origin": origin,
        "lib": str(lp), "lib_ok": lp.is_file(),
        "lib_size": lp.stat().st_size if lp.is_file() else 0,
        "version": pkgconfig_version(root / pc) if pc else None,
    }
    if project == "mpv" and hp.is_file():
        text = hp.read_text(encoding="utf-8", errors="replace")
        if m := re.search(r"MPV_CLIENT_API_VERSION\s+MPV_MAKE_VERSION\((\d+),\s*(\d+)\)", text):
            entry["client_api"] = f"{m.group(1)}.{m.group(2)}"
    return entry


def ffmpeg_config(config: str) -> Dict[str, bool]:
    """Lê o config.h que o próprio configure do FFmpeg gerou."""
    cfg = THIRDPARTY / "ffmpeg" / config / "build" / "config.h"
    out: Dict[str, bool] = {}
    if not cfg.is_file():
        return out
    text = cfg.read_text(encoding="utf-8", errors="replace")
    for flag in FFMPEG_FLAGS:
        m = re.search(rf"^#define {flag} (\d)$", text, re.M)
        out[flag] = bool(m and m.group(1) == "1")
    return out


def system_library(soname: str) -> str:
    if shutil.which("ldconfig"):
        rc, out = capture(["ldconfig", "-p"], timeout=30)
        for ln in out.splitlines():
            if soname in ln and "=>" in ln:
                return ln.split("=>")[-1].strip()
    for d in ("/usr/lib64", "/usr/lib"):
        p = Path(d) / soname
        if p.exists():
            return str(p)
    return ""


def nvidia() -> dict:
    info = {"present": bool(shutil.which("nvidia-smi"))}
    if not info["present"]:
        return info
    rc, out = capture(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                       "--format=csv,noheader"], timeout=30)
    gpus = [l.strip() for l in out.splitlines()
            if l.strip() and "Unable to" not in l and "No devices" not in l]
    info["gpus"] = gpus if rc == 0 else []
    info["error"] = "" if (rc == 0 and gpus) else out.strip()[:200]
    if not gpus:
        # nvidia-smi sem handle quase nunca diz a causa; quem diz é o NVRM no
        # log do kernel (fullchip reset, Xid, falha de alocação do NVKMS).
        readable, lines = kernel_nvrm()
        info["kernel"] = lines
        info["kernel_readable"] = readable
    return info


def kernel_nvrm() -> Tuple[bool, List[str]]:
    """(log legível?, últimas queixas do driver NVIDIA). O Fedora restringe o
    dmesg a root, então sem privilégio devolvemos só a dica do comando."""
    rc, out = capture(["dmesg", "--level=err,warn", "--notime"], timeout=30)
    if rc != 0:
        return False, ["log do kernel exige root: "
                       "sudo dmesg | grep -iE 'NVRM|" + "Xid" + "'"]
    return True, [l.strip() for l in out.splitlines()
                  if re.search(r"NVRM|" + "Xid" + r"|nvidia-drm", l)][-6:]


def collect() -> Tuple[dict, Findings]:
    f = Findings()
    data: dict = {"thirdparty": str(THIRDPARTY), "projects": {}, "ffmpeg": {},
                  "system_libs": {}, "nvidia": nvidia()}

    if not THIRDPARTY.is_dir():
        f.error(f"{THIRDPARTY.relative_to(REPO)} não existe — a stack de mídia "
                "nunca foi compilada (`python3 build.py debug`)")
        return data, f

    for config in CONFIGS:
        data["projects"][config] = {p: probe_project(p, config) for p in PROJECTS}
        data["ffmpeg"][config] = ffmpeg_config(config)

    # Um config parcial é o caso que morde: header instalado sem lib, ou nada.
    for config in CONFIGS:
        for project, info in data["projects"][config].items():
            if not info["installed"]:
                f.note(f"{config}/{project}: sem árvore de install — esse config "
                       "ainda não foi compilado")
                continue
            if not info["header_ok"]:
                where = ("o checkout em external/" if info["header_origin"] == "source"
                         else "a árvore de install do EP")
                f.error(f"{config}/{project}: falta o header "
                        f"{Path(info['header']).name} em {where} — é exatamente o "
                        "erro de compilação que a ordem do LINK_DEPENDS não "
                        "previne (build.py faz o pré-build para evitá-lo)")
            if not info["lib_ok"]:
                f.error(f"{config}/{project}: falta {Path(info['lib']).name} — "
                        "o link do app vai morrer")

    ready = [c for c in CONFIGS
             if all(i["lib_ok"] for i in data["projects"][c].values())]
    data["configs_ready"] = ready

    for config in ready:
        flags = data["ffmpeg"][config]
        if flags and not flags.get("CONFIG_VULKAN"):
            f.warn(f"{config}: FFmpeg sem CONFIG_VULKAN — a rota zero-copy de "
                   "Vulkan Video não existe neste build, só a de software/NVDEC")
        if flags and not flags.get("CONFIG_LIBPLACEBO"):
            f.note(f"{config}: FFmpeg sem CONFIG_LIBPLACEBO (o app usa libplacebo "
                   "direto, então isso só afeta o filtro do ffmpeg)")

    for so in SYSTEM_LIBS:
        data["system_libs"][so] = system_library(so)
    for so in ("libvulkan.so.1", "libEGL.so.1", "libGL.so.1"):
        if not data["system_libs"][so]:
            f.error(f"{so} não está no cache do ld — o caminho "
                    "NVDEC→OpenGL→libplacebo precisa dela")

    nv = data["nvidia"]
    if nv["present"] and not nv.get("gpus"):
        f.error("nvidia-smi não consegue um handle da GPU: "
                + (nv.get("error") or "sem detalhe").replace("\n", " / ")
                + " — sem isso não há NVDEC nem Vulkan na NVIDIA")
        for ln in nv.get("kernel", []):
            f.note(f"kernel: {ln[:160]}")
        if nv.get("kernel_readable") and any(
                "FULLCHIP_RESET" in l or "Xid" in l for l in nv.get("kernel", [])):
            f.note("o driver passou por um reset de chip inteiro; com a sessão "
                   "gráfica usando os módulos, só reboot devolve a GPU")
    return data, f


def render(d: dict) -> None:
    print(f"{DIM}media_doctor — {d['thirdparty']}{OFF}")

    for config in CONFIGS:
        projects = d["projects"].get(config, {})
        if not any(p["installed"] for p in projects.values()):
            head(f"{config}  {DIM}(não compilado){OFF}")
            continue
        head(config)
        for name, info in projects.items():
            ver = info.get("version") or info.get("client_api") or ""
            print(f"  {name:<20} header {OK if info['header_ok'] else BAD}   "
                  f"lib {OK if info['lib_ok'] else BAD} "
                  f"{human(info['lib_size']):>10}   {DIM}{ver}{OFF}")
        flags = d["ffmpeg"].get(config) or {}
        if flags:
            on = [k.replace("CONFIG_", "") for k, v in flags.items() if v]
            off = [k.replace("CONFIG_", "") for k, v in flags.items() if not v]
            print(f"  {DIM}ffmpeg on :{OFF} {', '.join(on) or '-'}")
            print(f"  {DIM}ffmpeg off:{OFF} {', '.join(off) or '-'}")

    head("bibliotecas do sistema (rota libplacebo/GL/Vulkan)")
    for so, path in d["system_libs"].items():
        line(so, path or BAD)

    head("NVIDIA")
    nv = d["nvidia"]
    if not nv["present"]:
        line("nvidia-smi", f"{DIM}ausente (sem GPU NVIDIA ou sem driver){OFF}")
    else:
        for gpu in nv.get("gpus", []):
            line("gpu", gpu)
        if nv.get("error"):
            line("erro", f"{RED}{nv['error'].splitlines()[0]}{OFF}")
        for ln in nv.get("kernel", []):
            print(f"    {DIM}{ln[:150]}{OFF}")


if __name__ == "__main__":
    raise SystemExit(main_wrapper(collect, render, sys.argv[1:]))
