#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vk_doctor.py — diagnóstico de Vulkan no host, falando com o loader de verdade.

Ferramenta de depuração para o agente (e para quem passar por aqui): em vez de
deduzir por manifestos, ela carrega a libvulkan por ctypes e chama as mesmas
funções que o app chama, inclusive vkCreateInstance, devolvendo o VkResult cru.

  python3 scripts/vk_doctor.py                 relatório completo + veredito
  python3 scripts/vk_doctor.py decode -6       o que é aquele código
  python3 scripts/vk_doctor.py layers          camadas que o loader enxerga
  python3 scripts/vk_doctor.py manifests       manifestos em disco e o .so de cada
  python3 scripts/vk_doctor.py create          repete o vkCreateInstance do app
  python3 scripts/vk_doctor.py create --layer VK_LAYER_KHRONOS_validation --ext VK_EXT_debug_report
  python3 scripts/vk_doctor.py env             variáveis que mexem no loader
  python3 scripts/vk_doctor.py app             o que o código-fonte pede
  --json                                       saída para consumo por máquina

Por que existe: um `-6` (VK_ERROR_LAYER_NOT_PRESENT) no vkCreateInstance quase
sempre é a camada de validação ausente ou com manifesto órfão (JSON presente, .so
faltando) — e isso não aparece no erro, só no VK_LOADER_DEBUG.
"""

from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

REPO = Path(__file__).resolve().parent.parent
IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"

# ── VkResult (vulkan_core.h). Só os que dá para topar na inicialização. ───────
VK_RESULTS: Dict[int, Tuple[str, str]] = {
    0: ("VK_SUCCESS", "tudo certo"),
    1: ("VK_NOT_READY", "operação ainda não terminou"),
    2: ("VK_TIMEOUT", "espera estourou o tempo"),
    3: ("VK_EVENT_SET", "evento sinalizado"),
    4: ("VK_EVENT_RESET", "evento não sinalizado"),
    5: ("VK_INCOMPLETE", "o array passado era menor que o resultado"),
    -1: ("VK_ERROR_OUT_OF_HOST_MEMORY", "faltou RAM para o driver/loader"),
    -2: ("VK_ERROR_OUT_OF_DEVICE_MEMORY", "faltou VRAM"),
    -3: ("VK_ERROR_INITIALIZATION_FAILED",
         "o driver não conseguiu inicializar (ICD quebrado, GPU sem permissão)"),
    -4: ("VK_ERROR_DEVICE_LOST", "o dispositivo caiu (TDR, reset de GPU, driver morto)"),
    -5: ("VK_ERROR_MEMORY_MAP_FAILED", "vkMapMemory falhou"),
    -6: ("VK_ERROR_LAYER_NOT_PRESENT",
         "uma camada pedida em ppEnabledLayerNames não foi encontrada pelo loader — "
         "camada não instalada, manifesto fora do caminho de busca, ou manifesto "
         "presente apontando para um .so que não existe/não carrega"),
    -7: ("VK_ERROR_EXTENSION_NOT_PRESENT",
         "extensão pedida não existe nesta instância/dispositivo"),
    -8: ("VK_ERROR_FEATURE_NOT_PRESENT", "feature pedida não existe no dispositivo"),
    -9: ("VK_ERROR_INCOMPATIBLE_DRIVER",
         "nenhum ICD compatível com a apiVersion pedida"),
    -10: ("VK_ERROR_TOO_MANY_OBJECTS", "estourou o limite de objetos do tipo"),
    -11: ("VK_ERROR_FORMAT_NOT_SUPPORTED", "formato não suportado"),
    -12: ("VK_ERROR_FRAGMENTED_POOL", "pool fragmentado"),
    -13: ("VK_ERROR_UNKNOWN", "o driver não soube dizer (bug dele, ou estado corrompido)"),
    -1000069000: ("VK_ERROR_OUT_OF_POOL_MEMORY", "pool sem espaço"),
    -1000072003: ("VK_ERROR_INVALID_EXTERNAL_HANDLE", "handle externo inválido"),
    -1000000000: ("VK_ERROR_SURFACE_LOST_KHR", "a surface morreu"),
    -1000000001: ("VK_ERROR_NATIVE_WINDOW_IN_USE_KHR", "a janela já está em uso"),
    -1000001004: ("VK_ERROR_OUT_OF_DATE_KHR", "swapchain desatualizada (resize)"),
    -1000003001: ("VK_ERROR_INCOMPATIBLE_DISPLAY_KHR", "display incompatível"),
    -1000011001: ("VK_ERROR_VALIDATION_FAILED_EXT", "validação reprovou a chamada"),
}

# O que o app pede em code/rendering/vulkan/vulkan_context.cpp:108-112,
# sob APP_USE_VULKAN_DEBUG_REPORT.
APP_LAYERS = ["VK_LAYER_KHRONOS_validation"]
APP_EXTS = ["VK_EXT_debug_report"]

LOADER_ENV = [
    "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_IMPLICIT_LAYER_PATH",
    "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE",
    "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
    "VK_LOADER_DEBUG", "VULKAN_SDK", "VK_LOADER_DRIVERS_SELECT",
    "LD_LIBRARY_PATH", "XDG_DATA_DIRS", "XDG_DATA_HOME",
]

TTY = sys.stdout.isatty()
BLUE = "\x1b[1;34m" if TTY else ""
GREEN = "\x1b[1;32m" if TTY else ""
YELLOW = "\x1b[1;33m" if TTY else ""
RED = "\x1b[1;31m" if TTY else ""
DIM = "\x1b[2m" if TTY else ""
OFF = "\x1b[0m" if TTY else ""


def head(msg: str) -> None:
    print(f"\n{BLUE}── {msg}{OFF}")


def result_name(code: int) -> str:
    name, why = VK_RESULTS.get(code, ("<desconhecido>", "não está na tabela do vulkan_core.h"))
    return f"{code} = {name} — {why}"


# ══════════════════════════════════════════════════════════════════════════════
# ligação com a libvulkan por ctypes (sem binding, sem pip)
# ══════════════════════════════════════════════════════════════════════════════
class VkLayerProperties(ctypes.Structure):
    _fields_ = [("layerName", ctypes.c_char * 256),
                ("specVersion", ctypes.c_uint32),
                ("implementationVersion", ctypes.c_uint32),
                ("description", ctypes.c_char * 256)]


class VkExtensionProperties(ctypes.Structure):
    _fields_ = [("extensionName", ctypes.c_char * 256),
                ("specVersion", ctypes.c_uint32)]


class VkApplicationInfo(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p),
                ("pApplicationName", ctypes.c_char_p),
                ("applicationVersion", ctypes.c_uint32),
                ("pEngineName", ctypes.c_char_p), ("engineVersion", ctypes.c_uint32),
                ("apiVersion", ctypes.c_uint32)]


class VkInstanceCreateInfo(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_uint32), ("pNext", ctypes.c_void_p),
                ("flags", ctypes.c_uint32),
                ("pApplicationInfo", ctypes.POINTER(VkApplicationInfo)),
                ("enabledLayerCount", ctypes.c_uint32),
                ("ppEnabledLayerNames", ctypes.POINTER(ctypes.c_char_p)),
                ("enabledExtensionCount", ctypes.c_uint32),
                ("ppEnabledExtensionNames", ctypes.POINTER(ctypes.c_char_p))]


VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
LIB_NAMES = (["vulkan-1.dll"] if IS_WINDOWS else
             ["libvulkan.1.dylib", "libvulkan.dylib"] if IS_MAC else
             ["libvulkan.so.1", "libvulkan.so"])


def load_loader() -> Tuple[Optional[ctypes.CDLL], str]:
    for name in LIB_NAMES:
        try:
            return ctypes.CDLL(name), name
        except OSError as exc:
            last = str(exc)
    return None, last


def api_version(major: int, minor: int, patch: int = 0) -> int:
    return (major << 22) | (minor << 12) | patch


def enum_layers(lib: ctypes.CDLL) -> Tuple[int, List[dict]]:
    fn = lib.vkEnumerateInstanceLayerProperties
    fn.restype = ctypes.c_int32
    count = ctypes.c_uint32(0)
    rc = fn(ctypes.byref(count), None)
    if rc != 0 or count.value == 0:
        return rc, []
    buf = (VkLayerProperties * count.value)()
    rc = fn(ctypes.byref(count), buf)
    return rc, [{"name": p.layerName.decode(errors="replace"),
                 "spec": vk_version_str(p.specVersion),
                 "impl": p.implementationVersion,
                 "desc": p.description.decode(errors="replace")}
                for p in buf[:count.value]]


def enum_extensions(lib: ctypes.CDLL, layer: Optional[str] = None) -> Tuple[int, List[str]]:
    fn = lib.vkEnumerateInstanceExtensionProperties
    fn.restype = ctypes.c_int32
    name = layer.encode() if layer else None
    count = ctypes.c_uint32(0)
    rc = fn(name, ctypes.byref(count), None)
    if rc != 0 or count.value == 0:
        return rc, []
    buf = (VkExtensionProperties * count.value)()
    rc = fn(name, ctypes.byref(count), buf)
    return rc, [p.extensionName.decode(errors="replace") for p in buf[:count.value]]


def vk_version_str(packed: int) -> str:
    return f"{(packed >> 22) & 0x7f}.{(packed >> 12) & 0x3ff}.{packed & 0xfff}"


def try_create_instance(lib: ctypes.CDLL, layers: List[str], exts: List[str],
                        api: int) -> int:
    """Faz o vkCreateInstance de verdade e devolve o VkResult cru."""
    app = VkApplicationInfo(sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, pNext=None,
                            pApplicationName=b"vk_doctor", applicationVersion=1,
                            pEngineName=b"vk_doctor", engineVersion=1, apiVersion=api)
    layer_arr = (ctypes.c_char_p * max(len(layers), 1))(*[s.encode() for s in layers])
    ext_arr = (ctypes.c_char_p * max(len(exts), 1))(*[s.encode() for s in exts])
    ci = VkInstanceCreateInfo(
        sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, pNext=None, flags=0,
        pApplicationInfo=ctypes.pointer(app),
        enabledLayerCount=len(layers), ppEnabledLayerNames=layer_arr if layers else None,
        enabledExtensionCount=len(exts), ppEnabledExtensionNames=ext_arr if exts else None)
    instance = ctypes.c_void_p()
    create = lib.vkCreateInstance
    create.restype = ctypes.c_int32
    rc = create(ctypes.byref(ci), None, ctypes.byref(instance))
    if rc == 0 and instance:
        destroy = lib.vkDestroyInstance
        destroy.restype = None
        destroy(instance, None)
    return rc


# ══════════════════════════════════════════════════════════════════════════════
# manifestos em disco — onde o loader procura, e se o .so de cada um existe
# ══════════════════════════════════════════════════════════════════════════════
def manifest_dirs() -> List[Path]:
    """Os caminhos de busca do loader, na ordem em que ele os considera."""
    dirs: List[Path] = []
    for var in ("VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_IMPLICIT_LAYER_PATH"):
        for chunk in (os.environ.get(var) or "").split(os.pathsep):
            if chunk:
                dirs.append(Path(chunk))
    if sdk := os.environ.get("VULKAN_SDK"):
        dirs.append(Path(sdk) / "etc" / "vulkan" / "explicit_layer.d")
    if IS_WINDOWS:
        return dirs
    home = Path(os.environ.get("XDG_DATA_HOME") or (Path.home() / ".local/share"))
    roots = [home, Path("/etc"), Path("/usr/local/share"), Path("/usr/share")]
    roots += [Path(p) for p in (os.environ.get("XDG_DATA_DIRS") or "").split(":") if p]
    for root in roots:
        for kind in ("explicit_layer.d", "implicit_layer.d"):
            dirs.append(root / "vulkan" / kind)
    seen, out = set(), []
    for d in dirs:
        if str(d) not in seen:
            seen.add(str(d))
            out.append(d)
    return out


def read_manifests() -> List[dict]:
    found = []
    for d in manifest_dirs():
        if not d.is_dir():
            continue
        for jf in sorted(d.glob("*.json")):
            entry = {"manifest": str(jf), "name": "?", "library": None,
                     "library_found": None, "error": None}
            try:
                data = json.loads(jf.read_text(encoding="utf-8", errors="replace"))
                layer = data.get("layer") or (data.get("layers") or [{}])[0]
                entry["name"] = layer.get("name", "?")
                lib = layer.get("library_path")
                entry["library"] = lib
                if lib:
                    p = Path(lib)
                    cand = p if p.is_absolute() else (jf.parent / lib)
                    # library_path relativo é resolvido ao lado do manifesto; um
                    # nome puro (sem barra) o loader procura pelo caminho do ld.
                    entry["library_found"] = bool(
                        cand.exists() or (("/" not in lib) and find_soname(lib)))
            except Exception as exc:                       # JSON quebrado conta
                entry["error"] = f"{type(exc).__name__}: {exc}"
            found.append(entry)
    return found


def find_soname(soname: str) -> Optional[str]:
    """Procura um .so pelo cache do ld e pelos diretórios usuais."""
    if shutil.which("ldconfig"):
        rc = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True,
                            errors="replace")
        for line in rc.stdout.splitlines():
            if soname in line and "=>" in line:
                return line.split("=>")[-1].strip()
    for d in ("/usr/lib64", "/usr/lib", "/usr/local/lib64", "/usr/local/lib"):
        cand = Path(d) / soname
        if cand.exists():
            return str(cand)
    return None


def icd_files() -> List[dict]:
    out = []
    dirs = []
    for var in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"):
        for chunk in (os.environ.get(var) or "").split(os.pathsep):
            if chunk:
                out.append({"icd": chunk, "from": var,
                            "exists": Path(chunk).exists()})
    if not IS_WINDOWS:
        dirs = [Path("/usr/share/vulkan/icd.d"), Path("/etc/vulkan/icd.d"),
                Path("/usr/local/share/vulkan/icd.d"),
                Path(os.environ.get("XDG_DATA_HOME") or (Path.home() / ".local/share"))
                / "vulkan" / "icd.d"]
    for d in dirs:
        for jf in sorted(d.glob("*.json")) if d.is_dir() else []:
            entry = {"icd": str(jf), "from": "dir", "exists": True, "library": None}
            try:
                data = json.loads(jf.read_text(encoding="utf-8", errors="replace"))
                lib = (data.get("ICD") or {}).get("library_path")
                entry["library"] = lib
                if lib:
                    p = Path(lib)
                    cand = p if p.is_absolute() else (jf.parent / lib)
                    entry["library_found"] = bool(
                        cand.exists() or (("/" not in lib) and find_soname(lib)))
            except Exception as exc:
                entry["error"] = f"{type(exc).__name__}: {exc}"
            out.append(entry)
    return out


# ══════════════════════════════════════════════════════════════════════════════
# o que o código-fonte pede (fonte da verdade: o .cpp, não a minha memória)
# ══════════════════════════════════════════════════════════════════════════════
def scan_source() -> dict:
    src = REPO / "code" / "rendering" / "vulkan" / "vulkan_context.cpp"
    info = {"file": str(src), "exists": src.is_file(), "layers": [], "extensions": [],
            "guards": []}
    if not src.is_file():
        return info
    text = src.read_text(encoding="utf-8", errors="replace")
    info["layers"] = sorted(set(re.findall(r'"(VK_LAYER_[A-Za-z0-9_]+)"', text)))
    info["extensions"] = sorted(set(re.findall(r'"(VK_[A-Z]+_[a-z0-9_]+)"', text)))
    info["guards"] = sorted(set(re.findall(r"#ifdef\s+(APP_[A-Z_]+)", text)))
    for cml in (REPO / "CMakeLists.txt",):
        if cml.is_file():
            body = cml.read_text(encoding="utf-8", errors="replace")
            info["cmake_debug_report"] = [
                ln.strip() for ln in body.splitlines()
                if "APP_USE_VULKAN_DEBUG_REPORT" in ln]
    return info


def packages_hint() -> List[str]:
    hints = []
    if shutil.which("rpm"):
        rc = subprocess.run(["rpm", "-qa", "vulkan*", "mesa-vulkan*"],
                            capture_output=True, text=True, errors="replace")
        hints += [ln for ln in rc.stdout.splitlines() if ln.strip()]
    elif shutil.which("dpkg-query"):
        rc = subprocess.run(["dpkg-query", "-W", "-f=${Package} ${Version}\n",
                             "vulkan*", "mesa-vulkan*"],
                            capture_output=True, text=True, errors="replace")
        hints += [ln for ln in rc.stdout.splitlines() if ln.strip()]
    return sorted(hints)


INSTALL_HINT = {
    "fedora": "sudo dnf install vulkan-validation-layers vulkan-tools",
    "debian": "sudo apt install vulkan-validationlayers vulkan-tools",
    "arch": "sudo pacman -S vulkan-validation-layers vulkan-tools",
}


def distro_hint() -> str:
    try:
        osr = Path("/etc/os-release").read_text(errors="replace")
    except OSError:
        return INSTALL_HINT["fedora"]
    low = osr.lower()
    if "fedora" in low or "rhel" in low or "centos" in low:
        return INSTALL_HINT["fedora"]
    if "debian" in low or "ubuntu" in low:
        return INSTALL_HINT["debian"]
    if "arch" in low:
        return INSTALL_HINT["arch"]
    return "instale o pacote das validation layers da sua distro"


# ══════════════════════════════════════════════════════════════════════════════
# relatório
# ══════════════════════════════════════════════════════════════════════════════
def collect() -> dict:
    lib, libname = load_loader()
    data: dict = {
        "host": {"system": platform.system(), "release": platform.release(),
                 "python": platform.python_version(),
                 "session_type": os.environ.get("XDG_SESSION_TYPE", "")},
        "loader": {"loaded": lib is not None, "library": libname},
        "env": {k: os.environ[k] for k in LOADER_ENV if k in os.environ},
        "source": scan_source(),
        "manifests": read_manifests(),
        "icds": icd_files(),
        "packages": packages_hint(),
    }
    if lib is None:
        return data

    fn = getattr(lib, "vkEnumerateInstanceVersion", None)
    if fn:
        fn.restype = ctypes.c_int32
        ver = ctypes.c_uint32(0)
        if fn(ctypes.byref(ver)) == 0:
            data["loader"]["instance_version"] = vk_version_str(ver.value)

    rc, layers = enum_layers(lib)
    data["layers"] = {"result": rc, "list": layers}
    rc, exts = enum_extensions(lib)
    data["extensions"] = {"result": rc, "list": exts}

    wanted_layers = data["source"]["layers"] or APP_LAYERS
    wanted_exts = [e for e in (data["source"]["extensions"] or APP_EXTS)
                   if e in ("VK_EXT_debug_report", "VK_EXT_debug_utils")]
    have = {l["name"] for l in layers}
    data["wanted"] = {"layers": wanted_layers, "extensions": wanted_exts,
                      "layers_missing": [l for l in wanted_layers if l not in have],
                      "extensions_missing": [e for e in wanted_exts if e not in exts]}

    data["create"] = {
        "plain": try_create_instance(lib, [], [], api_version(1, 2)),
        "as_app": try_create_instance(lib, wanted_layers, wanted_exts, api_version(1, 2)),
    }
    return data


def verdict(d: dict) -> List[str]:
    out = []
    if not d["loader"]["loaded"]:
        return [f"{RED}A libvulkan não carregou{OFF} ({d['loader']['library']}). "
                "Sem loader não há Vulkan nenhum: instale o pacote do loader."]
    c = d.get("create", {})
    if c.get("plain") != 0:
        out.append(f"{RED}Instância mínima (sem camada, sem extensão) falhou:{OFF} "
                   f"{result_name(c['plain'])}. O problema é anterior às camadas.")
    missing = d.get("wanted", {}).get("layers_missing") or []
    orphans = [m for m in d["manifests"] if m["library_found"] is False]
    if missing:
        out.append(f"{RED}Camadas pedidas pelo app e ausentes:{OFF} {', '.join(missing)}")
        for m in orphans:
            if m["name"] in missing:
                out.append(f"  {YELLOW}manifesto órfão{OFF}: {m['manifest']} aponta "
                           f"para '{m['library']}', que não existe — é este o -6")
        out.append(f"  conserto provável: {DIM}{distro_hint()}{OFF}")
    if c.get("as_app") == -6:
        out.append(f"{RED}Reproduzido:{OFF} o vkCreateInstance com o pedido do app "
                   f"devolve {result_name(-6)}")
    elif c.get("as_app") not in (0, None):
        out.append(f"{YELLOW}vkCreateInstance como o app falhou:{OFF} "
                   f"{result_name(c['as_app'])}")
    elif c.get("as_app") == 0 and not missing:
        out.append(f"{GREEN}Sem problema aqui:{OFF} a instância com camada de "
                   "validação + debug_report foi criada e destruída com sucesso.")
    if orphans and not missing:
        out.append(f"{YELLOW}Manifestos apontando para .so inexistente{OFF} "
                   f"(não afetam o app, mas envenenam outras camadas): "
                   + ", ".join(m["name"] for m in orphans))
    out.append(f"{DIM}Para o rastro do loader: VK_LOADER_DEBUG=all ./build/debug/"
               f"example_sdl3_vulkan_debug 2>&1 | grep -i layer{OFF}")
    return out


def print_report(d: dict) -> None:
    h = d["host"]
    print(f"{BLUE}vk_doctor{OFF} — {h['system']} {h['release']}, python {h['python']}"
          + (f", sessão {h['session_type']}" if h["session_type"] else ""))

    head("loader")
    ld = d["loader"]
    print(f"  biblioteca: {ld['library']}  carregada: "
          + (f"{GREEN}sim{OFF}" if ld["loaded"] else f"{RED}não{OFF}"))
    if "instance_version" in ld:
        print(f"  vkEnumerateInstanceVersion: {ld['instance_version']}")

    head("o que o código pede")
    s = d["source"]
    print(f"  {s['file']}")
    print(f"  camadas: {', '.join(s['layers']) or '<nenhuma>'}")
    print(f"  guardas: {', '.join(s['guards']) or '<nenhuma>'}")
    for ln in s.get("cmake_debug_report", [])[:4]:
        print(f"  {DIM}CMakeLists: {ln}{OFF}")

    if "layers" in d:
        head(f"camadas que o loader enxerga ({len(d['layers']['list'])})")
        want = set(d.get("wanted", {}).get("layers") or [])
        for l in d["layers"]["list"]:
            mark = f"{GREEN}*{OFF}" if l["name"] in want else " "
            print(f" {mark} {l['name']:<44} spec {l['spec']:<10} {DIM}{l['desc'][:44]}{OFF}")
        if not d["layers"]["list"]:
            print(f"  {RED}nenhuma{OFF} (rc={result_name(d['layers']['result'])})")

    head("manifestos de camada em disco")
    for m in d["manifests"]:
        state = (f"{GREEN}ok{OFF}" if m["library_found"] else
                 f"{RED}.so AUSENTE{OFF}" if m["library_found"] is False else
                 f"{YELLOW}sem library_path{OFF}")
        print(f"  {m['name']:<44} {state}  {DIM}{m['manifest']}{OFF}")
        if m["error"]:
            print(f"    {RED}{m['error']}{OFF}")
    if not d["manifests"]:
        print(f"  {YELLOW}nenhum manifesto encontrado nos caminhos de busca{OFF}")
        for p in manifest_dirs():
            print(f"    {DIM}procurado: {p}{OFF}")

    head("ICDs (drivers)")
    for i in d["icds"]:
        state = (f"{GREEN}ok{OFF}" if i.get("library_found") else
                 f"{RED}.so AUSENTE{OFF}" if i.get("library_found") is False else "")
        print(f"  {i['icd']}  {state}")

    if d["env"]:
        head("ambiente relevante ao loader")
        for k, v in d["env"].items():
            print(f"  {k}={v}")

    if "create" in d:
        head("vkCreateInstance de verdade")
        print(f"  sem camadas:      {result_name(d['create']['plain'])}")
        print(f"  como o app pede:  {result_name(d['create']['as_app'])}")

    head("veredito")
    for line in verdict(d):
        print(f"  {line}")


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="vk_doctor.py",
                                description="Diagnóstico de Vulkan pelo loader real.")
    p.add_argument("command", nargs="?", default="report",
                   choices=["report", "decode", "layers", "manifests", "create",
                            "env", "app"])
    p.add_argument("code", nargs="?", help="para 'decode': o VkResult (ex.: -6)")
    p.add_argument("--layer", action="append", default=[], help="camada para 'create'")
    p.add_argument("--ext", action="append", default=[], help="extensão para 'create'")
    p.add_argument("--api", default="1.2", help="apiVersion para 'create' (padrão 1.2)")
    p.add_argument("--json", action="store_true", help="saída em JSON")
    args = p.parse_args(argv)

    if args.command == "decode":
        if args.code is None:
            p.error("decode precisa de um código, ex.: decode -6")
        print(result_name(int(args.code)))
        return 0

    if args.command == "create":
        lib, name = load_loader()
        if lib is None:
            print(f"libvulkan não carregou: {name}", file=sys.stderr)
            return 2
        layers = args.layer or (scan_source()["layers"] or APP_LAYERS)
        exts = args.ext or APP_EXTS
        major, _, minor = args.api.partition(".")
        rc = try_create_instance(lib, layers, exts,
                                 api_version(int(major), int(minor or 0)))
        print(f"camadas:    {layers}")
        print(f"extensões:  {exts}")
        print(f"resultado:  {result_name(rc)}")
        return 0 if rc == 0 else 1

    data = collect()
    if args.command == "layers":
        data = {"layers": data.get("layers"), "wanted": data.get("wanted")}
    elif args.command == "manifests":
        data = {"manifests": data["manifests"], "icds": data["icds"],
                "search_paths": [str(p) for p in manifest_dirs()]}
    elif args.command == "env":
        data = {"env": data["env"], "checked": LOADER_ENV}
    elif args.command == "app":
        data = {"source": data["source"], "wanted": data.get("wanted")}

    if args.json:
        print(json.dumps(data, indent=2, ensure_ascii=False))
        return 0
    if args.command == "report":
        print_report(data)
        rc = data.get("create", {}).get("as_app")
        return 0 if rc == 0 else 1
    print(json.dumps(data, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
