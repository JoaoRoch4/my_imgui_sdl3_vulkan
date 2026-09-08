#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bootstrap.py — setup de uma tacada só, em Windows e Linux, com os arquivos de
                IDE para VS Code, CLion e Visual Studio.

Faz em Python o que setup.sh + build.sh fazem só em Fedora/bash, e ainda gera a
configuração dos três IDEs. Tudo é idempotente: rodar de novo só faz o que falta.

  python3 bootstrap.py                  detecta + deps + clone + configure + IDEs
  python3 bootstrap.py doctor           só o relatório da máquina (não muda nada)
  python3 bootstrap.py ide --ide vscode só (re)escreve a config de um IDE
  python3 bootstrap.py build debug      compila (equivalente cross-platform do build.sh)
  python3 bootstrap.py run debug        compila e roda

Fases do 'setup' (cada uma pulável com --skip-<fase>):
  1. packages   pacotes de sistema  (dnf/apt/pacman no Linux; winget no Windows)
  2. vcpkg      bootstrap do vcpkg + ports do triplet certo
  3. clone      external/ nos refs fixados (lidos do setup.sh — fonte única)
  4. configure  cmake --preset  (Linux: 'all' / Windows: 'vs2026')
  5. ide        .vscode/, .idea/ e a solução .slnx do Visual Studio

Onde os binários saem (medido no CMakeLists.txt:292-299, não no build.sh):
  Debug           build/debug/example_sdl3_vulkan_debug
  Release         build/release/example_sdl3_vulkan_release
  RelWithDebInfo  build/RELWITHDEBINFO/example_sdl3_vulkan_RELWITHDEBINFO
  Windows (Qt)    build/vs2026/QT/VulkanMedia/<Config>/appVulkanMedia.exe
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

REPO = Path(__file__).resolve().parent
IS_WINDOWS = os.name == "nt"
IS_LINUX = sys.platform.startswith("linux")
IS_MAC = sys.platform == "darwin"

CONFIGS = ("Debug", "Release", "RelWithDebInfo")
CONFIG_ALIASES = {
    "debug": "Debug",
    "release": "Release",
    "release-log": "RelWithDebInfo",
    "relwithdebinfo": "RelWithDebInfo",
}

DRY_RUN = False
ASSUME_YES = False

# Com --windows-root, um C: do Windows montado no Linux: dá para preparar o
# checkout do Windows daqui, lendo Qt/VS/vcpkg de verdade e gravando os caminhos
# como o Windows os vê (C:/Qt/...), em vez de /run/media/....
WIN_ROOT: Optional["Path"] = None
WIN_DRIVE = "C:"


def to_target_path(path) -> str:
    """O caminho como o SO-alvo o enxerga (C:/Qt/... e não /run/media/…/Qt/...)."""
    if path is None:
        return ""
    path = Path(path)
    if WIN_ROOT is not None:
        try:
            return f"{WIN_DRIVE}/" + str(path.relative_to(WIN_ROOT)).replace("\\", "/")
        except ValueError:
            pass
    return str(path).replace("\\", "/")


def from_target_path(win_path: str) -> Path:
    """O inverso: 'C:\\Program Files\\...' -> o caminho montado que dá para ler."""
    normalized = win_path.replace("\\", "/")
    if WIN_ROOT is not None and re.match(r"^[A-Za-z]:/", normalized):
        return WIN_ROOT / normalized[3:]
    return Path(normalized)

# ══════════════════════════════════════════════════════════════════════════════
# saída bonitinha
# ══════════════════════════════════════════════════════════════════════════════
def _enable_ansi() -> bool:
    if not sys.stdout.isatty():
        return False
    if IS_WINDOWS:
        try:
            import ctypes

            k = ctypes.windll.kernel32
            h = k.GetStdHandle(-11)
            mode = ctypes.c_uint32()
            if not k.GetConsoleMode(h, ctypes.byref(mode)):
                return False
            k.SetConsoleMode(h, mode.value | 0x0004)  # VIRTUAL_TERMINAL_PROCESSING
        except Exception:
            return False
    return True


_C = _enable_ansi()
BLUE = "\x1b[1;34m" if _C else ""
GREEN = "\x1b[1;32m" if _C else ""
YELLOW = "\x1b[1;33m" if _C else ""
RED = "\x1b[1;31m" if _C else ""
CYAN = "\x1b[1;36m" if _C else ""
DIM = "\x1b[2m" if _C else ""
OFF = "\x1b[0m" if _C else ""


def log(msg: str) -> None:
    print(f"{BLUE}==>{OFF} {msg}")


def step(msg: str) -> None:
    print(f"\n{CYAN}┌── {msg}{OFF}")


def ok(msg: str) -> None:
    print(f"{GREEN} ok{OFF} {msg}")


def skip(msg: str) -> None:
    print(f"{DIM}   skip{OFF} {msg}")


def warn(msg: str) -> None:
    print(f"{YELLOW}warn{OFF} {msg}")


def die(msg: str, code: int = 1) -> "NoReturn":  # type: ignore[valid-type]
    print(f"{RED}err{OFF} {msg}", file=sys.stderr)
    sys.exit(code)


def confirm(question: str) -> bool:
    if ASSUME_YES:
        return True
    if not sys.stdin.isatty():
        warn(f"{question} — sem tty e sem -y, assumindo NÃO")
        return False
    return input(f"{YELLOW}?{OFF} {question} [y/N] ").strip().lower() in ("y", "yes", "s", "sim")


# ══════════════════════════════════════════════════════════════════════════════
# processos
# ══════════════════════════════════════════════════════════════════════════════
def run(
    cmd: Sequence[str],
    cwd: Optional[Path] = None,
    check: bool = True,
    env: Optional[Dict[str, str]] = None,
    quiet: bool = False,
) -> int:
    """Roda um comando mostrando a linha. Respeita --dry-run."""
    printable = " ".join(str(c) for c in cmd)
    if DRY_RUN:
        print(f"{DIM}   [dry-run] {printable}{OFF}")
        return 0
    if not quiet:
        print(f"{DIM}   $ {printable}{OFF}")
    full_env = None
    if env:
        full_env = dict(os.environ)
        full_env.update(env)
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd or REPO), env=full_env)
    if check and proc.returncode != 0:
        die(f"comando falhou ({proc.returncode}): {printable}")
    return proc.returncode


def capture(cmd: Sequence[str], cwd: Optional[Path] = None) -> Tuple[int, str]:
    """Roda e captura stdout+stderr. Nunca levanta; devolve (rc, texto)."""
    try:
        proc = subprocess.run(
            [str(c) for c in cmd],
            cwd=str(cwd or REPO),
            capture_output=True,
            text=True,
            errors="replace",
        )
    except (OSError, FileNotFoundError) as exc:
        return 127, str(exc)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def which(name: str) -> Optional[str]:
    return shutil.which(name)


def write_file(path: Path, content: str, label: Optional[str] = None,
               backup: bool = True) -> None:
    label = label or str(path.relative_to(REPO) if path.is_relative_to(REPO) else path)
    if DRY_RUN:
        print(f"{DIM}   [dry-run] escreveria {label} ({len(content)} bytes){OFF}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    existed = path.exists()
    old = path.read_text(encoding="utf-8") if existed else None
    if old == content:
        skip(f"{label} já está atualizado")
        return
    if existed and backup:
        # só para arquivos que este script não é dono (ex.: .clangd versionado);
        # a config de IDE é regerável, encher a pasta de .bak só polui.
        keep = path.with_suffix(path.suffix + ".bak")
        keep.write_text(old or "", encoding="utf-8")
        warn(f"{label} existia — backup em {keep.name}")
    path.write_text(content, encoding="utf-8")
    ok(f"{'reescrito' if existed else 'escrito'} {label}")


def write_json(path: Path, data: dict, backup: bool = False) -> None:
    write_file(path, json.dumps(data, indent=2, ensure_ascii=False) + "\n", backup=backup)


def merge_json(path: Path, data: dict) -> None:
    """Funde chaves em um JSON existente (não destrói o que o usuário pôs lá)."""
    if path.exists():
        try:
            current = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            warn(f"{path.name} não é JSON estrito (comentários?) — gravando {path.name}.new")
            write_json(path.with_suffix(path.suffix + ".new"), data)
            return
        if isinstance(current, dict):
            current.update(data)
            data = current
    write_json(path, data)


# ══════════════════════════════════════════════════════════════════════════════
# detecção da máquina
# ══════════════════════════════════════════════════════════════════════════════
@dataclass
class VsInstall:
    path: Path
    name: str
    version: str
    product: str

    @property
    def toolsets(self) -> Dict[str, List[str]]:
        """{'v170': ['v143'], 'v180': ['v145','ClangCL'], ...} — o teste barato da §8."""
        out: Dict[str, List[str]] = {}
        vc = self.path / "MSBuild" / "Microsoft" / "VC"
        if not vc.is_dir():
            return out
        for gen in sorted(vc.iterdir()):
            ts = gen / "Platforms" / "x64" / "PlatformToolsets"
            out[gen.name] = sorted(p.name for p in ts.iterdir()) if ts.is_dir() else []
        return out


@dataclass
class Host:
    system: str                      # "windows" | "linux" | "darwin"
    distro: str = ""                 # "fedora", "ubuntu", "arch", ...
    distro_like: str = ""
    pkg: str = ""                    # "dnf" | "apt" | "pacman" | "winget" | ""
    vcpkg_root: Path = field(default_factory=Path)
    triplet: str = ""
    qt_dir: Optional[Path] = None
    vulkan_sdk: Optional[Path] = None
    vs_installs: List[VsInstall] = field(default_factory=list)
    cross: bool = False               # alvo != máquina onde este script roda

    @property
    def is_windows(self) -> bool:
        return self.system == "windows"

    @property
    def preset(self) -> str:
        """O preset de configure que serve para este host."""
        return "vs2026" if self.is_windows else "all"

    @property
    def build_dir(self) -> Path:
        return REPO / "build" / ("vs2026" if self.is_windows else "all")


def _read_os_release() -> Dict[str, str]:
    data: Dict[str, str] = {}
    for candidate in ("/etc/os-release", "/usr/lib/os-release"):
        p = Path(candidate)
        if p.is_file():
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                if "=" in line and not line.startswith("#"):
                    k, _, v = line.partition("=")
                    data[k.strip()] = v.strip().strip('"')
            break
    return data


def _find_vs_installs() -> List[VsInstall]:
    if WIN_ROOT is not None:
        return _scan_vs_installs(WIN_ROOT)
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return []
    rc, out = capture([str(vswhere), "-all", "-prerelease", "-products", "*", "-format", "json"])
    if rc != 0:
        return []
    try:
        entries = json.loads(out)
    except json.JSONDecodeError:
        return []
    installs = []
    for e in entries:
        installs.append(
            VsInstall(
                path=Path(e.get("installationPath", "")),
                name=e.get("displayName", "?"),
                version=e.get("installationVersion", "?"),
                product=(e.get("catalog") or {}).get("productDisplayVersion", "?"),
            )
        )
    return installs


def _scan_vs_installs(root: Path) -> List[VsInstall]:
    """Mesmo inventário do vswhere, lido do disco: cada instalação registra um
    state.json em ProgramData\\Microsoft\\VisualStudio\\Packages\\_Instances\\<id>."""
    instances = root / "ProgramData" / "Microsoft" / "VisualStudio" / "Packages" / "_Instances"
    out: List[VsInstall] = []
    if not instances.is_dir():
        return out
    for entry in sorted(instances.iterdir()):
        state = entry / "state.json"
        if not state.is_file():
            continue
        try:
            # o state.json vem com BOM
            data = json.loads(state.read_text(encoding="utf-8-sig"))
        except (json.JSONDecodeError, OSError):
            continue
        install_path = data.get("installationPath", "")
        fs_path = from_target_path(install_path)
        out.append(VsInstall(
            path=fs_path,
            name=data.get("displayName") or fs_path.name or install_path,
            version=data.get("installationVersion", "?"),
            product=(data.get("catalogInfo") or {}).get("productDisplayVersion", "?"),
        ))
    return out


def _find_qt(system: str) -> Optional[Path]:
    """QTDIR do ambiente, senão o kit mais novo em C:\\Qt (Windows) / o Qt6 do sistema."""
    env = os.environ.get("QTDIR") or os.environ.get("Qt6_DIR")
    if env and Path(env).is_dir() and WIN_ROOT is None:
        return Path(env)
    if system == "windows":
        root = (WIN_ROOT / "Qt") if WIN_ROOT is not None \
            else Path(os.environ.get("SystemDrive", "C:") + "\\Qt")
        if not root.is_dir():
            return None
        best: Optional[Tuple[Tuple[int, ...], Path]] = None
        for ver in root.iterdir():
            if not re.fullmatch(r"\d+(\.\d+)*", ver.name):
                continue
            for kit in ver.iterdir():
                # só kits MSVC x64: mingw/llvm-mingw têm outra ABI (windows-msvc-context.md §5)
                if re.fullmatch(r"msvc\d{4}_64", kit.name) and (kit / "bin").is_dir():
                    key = tuple(int(x) for x in ver.name.split("."))
                    if best is None or key > best[0]:
                        best = (key, kit)
        return best[1] if best else None
    qmake = which("qmake6") or which("qmake")
    if qmake:
        rc, out = capture([qmake, "-query", "QT_INSTALL_PREFIX"])
        if rc == 0 and out.strip():
            return Path(out.strip())
    return None


def _find_vulkan_sdk(system: str) -> Optional[Path]:
    env = os.environ.get("VULKAN_SDK")
    if env and Path(env).is_dir() and WIN_ROOT is None:
        return Path(env)
    if system == "windows":
        root = (WIN_ROOT / "VulkanSDK") if WIN_ROOT is not None \
            else Path(os.environ.get("SystemDrive", "C:") + "\\VulkanSDK")
        if root.is_dir():
            versions = sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name)
            if versions:
                return versions[-1]
    return None


def detect_host(target_os: Optional[str] = None) -> Host:
    real = "windows" if IS_WINDOWS else ("darwin" if IS_MAC else "linux")
    system = target_os or real
    h = Host(system=system, cross=system != real)

    if system == "linux" and not h.cross:
        osr = _read_os_release()
        h.distro = osr.get("ID", "")
        h.distro_like = osr.get("ID_LIKE", "")
        for tool, name in (("dnf5", "dnf"), ("dnf", "dnf"), ("apt-get", "apt"), ("pacman", "pacman")):
            if which(tool):
                h.pkg = name
                break
        h.triplet = "x64-linux" if platform.machine() in ("x86_64", "AMD64") else "arm64-linux"
        h.vcpkg_root = Path(os.environ.get("VCPKG_ROOT") or (Path.home() / "vcpkg"))
    elif system == "windows":
        h.pkg = "winget" if (not h.cross and which("winget")) else ""
        h.triplet = "x64-windows"
        default_vcpkg = ((WIN_ROOT / "vcpkg") if WIN_ROOT is not None
                         else Path(os.environ.get("SystemDrive", "C:") + "\\vcpkg"))
        h.vcpkg_root = Path(os.environ["VCPKG_ROOT"]) if (
            os.environ.get("VCPKG_ROOT") and not h.cross) else default_vcpkg
        h.vs_installs = _find_vs_installs()
    elif system == "linux":
        h.triplet = "x64-linux"
        h.vcpkg_root = Path(os.environ.get("VCPKG_ROOT") or (Path.home() / "vcpkg"))
    else:
        h.triplet = "arm64-osx" if platform.machine() == "arm64" else "x64-osx"
        h.vcpkg_root = Path(os.environ.get("VCPKG_ROOT") or (Path.home() / "vcpkg"))

    h.qt_dir = _find_qt(system)
    h.vulkan_sdk = _find_vulkan_sdk(system)
    return h


# ══════════════════════════════════════════════════════════════════════════════
# setup.sh é a fonte única dos refs fixados e da lista de pacotes Fedora
# ══════════════════════════════════════════════════════════════════════════════
@dataclass
class Sources:
    versions: Dict[str, str]
    repos: List[Tuple[str, str, str, str, str]]  # dir, url, ref, kind, flags
    fedora_packages: List[str]
    libplacebo_url: str
    libplacebo_mirror: str
    libplacebo_ref: str
    libplacebo_submodules: List[str]


def _array_body(text: str, name: str) -> str:
    """Corpo de um `readonly NAME=( ... )` do setup.sh, sem os comentários.

    Cortar no primeiro ")" cru não serve: os comentários do PKGS têm parênteses
    ("(owned by libglvnd-devel)"), e um "^)" multilinha atravessaria um array de
    uma linha só até o fechamento do array seguinte. Então: tira o comentário de
    cada linha primeiro, e só então procura o ")" que fecha."""
    m = re.search(r"^readonly\s+%s=\(" % re.escape(name), text, re.M)
    if not m:
        die(f"setup.sh mudou de forma: não achei o array {name}. "
            f"Conserte parse_setup_sh() em bootstrap.py antes de confiar no clone.")
    lines: List[str] = []
    for line in text[m.end():].splitlines():
        code = re.sub(r"#.*$", "", line)
        close = code.find(")")
        if close != -1:
            lines.append(code[:close])
            return "\n".join(lines)
        lines.append(code)
    die(f"array {name} do setup.sh não fecha — parse abortado")


def parse_setup_sh() -> Sources:
    """Lê os refs/pacotes do setup.sh em vez de duplicá-los aqui.

    Duplicar a tabela significaria ela divergir em silêncio no dia em que alguém
    bumpar uma versão só num dos dois arquivos. Se o parse falhar, o script morre
    dizendo isso — nunca clona um ref adivinhado."""
    sh = REPO / "setup.sh"
    if not sh.is_file():
        die("setup.sh não encontrado — ele é a fonte dos refs fixados de external/.")
    # um checkout do Windows (core.autocrlf) traz CRLF; normaliza antes de casar
    text = sh.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")

    versions = dict(re.findall(r'^readonly\s+([A-Z_0-9]+)="([^"]*)"', text, re.M))

    def expand(s: str) -> str:
        return re.sub(r"\$\{([A-Z_0-9]+)\}", lambda m: versions.get(m.group(1), m.group(0)), s)

    repos: List[Tuple[str, str, str, str, str]] = []
    for entry in re.findall(r'"([^"]+)"', _array_body(text, "SOURCES")):
        parts = expand(entry).split("|")
        if len(parts) != 5 or "${" in expand(entry):
            die(f"entrada de SOURCES ininteligível no setup.sh: {entry!r}")
        repos.append(tuple(parts))  # type: ignore[arg-type]

    pkgs_body = re.sub(r"#[^\n]*", "", _array_body(text, "PKGS"))
    fedora_packages = pkgs_body.split()

    submods = _array_body(text, "LIBPLACEBO_SUBMODULES").split()

    if len(repos) < 10 or len(fedora_packages) < 20:
        die(f"parse do setup.sh saiu curto demais ({len(repos)} repos, "
            f"{len(fedora_packages)} pacotes) — revise parse_setup_sh().")

    return Sources(
        versions=versions,
        repos=repos,
        fedora_packages=fedora_packages,
        libplacebo_url=versions.get("LIBPLACEBO_URL", "")
        or re.search(r'LIBPLACEBO_URL="([^"]+)"', text).group(1),
        libplacebo_mirror=re.search(r'LIBPLACEBO_MIRROR="([^"]+)"', text).group(1),
        libplacebo_ref=versions.get("V_LIBPLACEBO", ""),
        libplacebo_submodules=submods,
    )


# ══════════════════════════════════════════════════════════════════════════════
# Fase 1 — pacotes de sistema
# ══════════════════════════════════════════════════════════════════════════════
# Fedora vem do setup.sh (verificado com dnf5 repoquery). As listas de Debian e
# Arch são a tradução dessa, e cada nome foi conferido em 08/09/2026 contra
# packages.debian.org/trixie, packages.ubuntu.com/noble e a API do archlinux.org:
# 69/69 existem no Debian trixie, 64/64 no Arch. Ainda assim o instalador
# resolve os nomes no gerenciador local antes de instalar (_apt_unknown /
# _pacman_unknown) — release diferente renomeia pacote, e é melhor pular um nome
# e dizer qual do que derrubar o lote inteiro.
APT_PACKAGES = [
    # toolchain
    "clang", "lld", "cmake", "ninja-build", "meson", "make", "git", "pkg-config",
    "python3", "nasm", "gcc", "g++", "perl", "zip", "unzip", "tar", "curl",
    # deps de sistema do app
    "libfreetype-dev", "libfontconfig-dev", "libglvnd-dev", "libegl1-mesa-dev",
    "libgl1-mesa-dev", "libboost-all-dev",
    # mpv / libplacebo
    "libluajit-5.1-dev", "libass-dev", "zlib1g-dev", "glslang-dev", "glslang-tools",
    "libshaderc-dev", "liblcms2-dev",
    # codecs do FFmpeg
    "libdav1d-dev", "libaom-dev", "libx264-dev", "libx265-dev", "libvpx-dev",
    "libopus-dev", "libvorbis-dev", "libtheora-dev", "libmp3lame-dev",
    "libfdk-aac-dev", "libwebp-dev", "libgnutls28-dev", "libunistring-dev", "libnuma-dev",
    # hwaccel
    # nv-codec-headers é o nome do FONTE; o binário no Debian/Ubuntu é este:
    "libffmpeg-nvenc-dev", "libva-dev", "libvdpau-dev", "libvulkan-dev",
    "vulkan-validationlayers",
    # SDL3 / WSI / áudio
    "libwayland-dev", "wayland-protocols", "libxkbcommon-dev", "libdecor-0-dev",
    "libx11-dev", "libxext-dev", "libxcursor-dev", "libxi-dev", "libxrandr-dev",
    "libxfixes-dev", "libxss-dev", "libxtst-dev", "libxcb1-dev", "libdrm-dev", "libgbm-dev",
    "libpipewire-0.3-dev", "libpulse-dev", "libasound2-dev", "libdbus-1-dev", "liburing-dev",
]

PACMAN_PACKAGES = [
    "clang", "lld", "cmake", "ninja", "meson", "make", "git", "pkgconf", "python",
    "nasm", "gcc", "perl", "zip", "unzip", "tar", "curl",
    "freetype2", "fontconfig", "libglvnd", "boost",
    "luajit", "libass", "zlib", "glslang", "shaderc", "lcms2",
    "dav1d", "aom", "x264", "x265", "libvpx", "opus", "libvorbis", "libtheora",
    "lame", "libwebp", "gnutls", "libunistring", "numactl",
    "ffnvcodec-headers", "libva", "libvdpau", "vulkan-headers", "vulkan-icd-loader",
    "wayland", "wayland-protocols", "libxkbcommon", "libdecor", "libx11", "libxext",
    "libxcursor", "libxi", "libxrandr", "libxfixes", "libxss", "libxtst", "libxcb", "libdrm",
    "mesa", "pipewire", "libpulse", "alsa-lib", "dbus", "liburing",
]

# Windows: só o que dá para instalar sem interação. VS e Qt ficam de fora de
# propósito — são instalações de vários GB com componentes a escolher, e a
# escolha errada é pior do que não instalar (docs/windows-msvc-context.md §1).
WINGET_PACKAGES = [
    ("cmake", "Kitware.CMake"),
    ("ninja", "Ninja-build.Ninja"),
    ("git", "Git.Git"),
    ("python", "Python.Python.3.13"),
]


def _missing_rpm(pkgs: List[str]) -> List[str]:
    missing = []
    for p in pkgs:
        rc, _ = capture(["rpm", "-q", p])
        if rc != 0:
            missing.append(p)
    return missing


def _missing_dpkg(pkgs: List[str]) -> List[str]:
    missing = []
    for p in pkgs:
        rc, out = capture(["dpkg-query", "-W", "-f=${Status}", p])
        if rc != 0 or "install ok installed" not in out:
            missing.append(p)
    return missing


def _apt_unknown(pkgs: List[str]) -> List[str]:
    """Quais destes nomes o apt local não conhece (release diferente, componente
    non-free/multiverse desabilitado, …)."""
    rc, out = capture(["apt-cache", "policy", *pkgs])
    if rc not in (0, 100):        # a própria ferramenta falhou: não dá para julgar
        return []
    known = set(re.findall(r"^([^\s:]+):$", out, re.M))
    return [p for p in pkgs if p not in known]


def _pacman_unknown(pkgs: List[str]) -> List[str]:
    rc, out = capture(["pacman", "-Si", *pkgs])
    if rc == 127:
        return []
    return sorted(set(re.findall(r"package '([^']+)' was not found", out)))


def _missing_pacman(pkgs: List[str]) -> List[str]:
    rc, out = capture(["pacman", "-Qq"])
    have = set(out.split()) if rc == 0 else set()
    return [p for p in pkgs if p not in have]


def phase_packages(host: Host, src: Sources, use_sudo: bool = True) -> None:
    step("Fase 1 — pacotes de sistema")

    if host.is_windows:
        missing = [(exe, wid) for exe, wid in WINGET_PACKAGES if not which(exe)]
        if not missing:
            ok("cmake, ninja, git e python já estão no PATH")
        elif not host.pkg:
            warn("winget não encontrado — instale à mão: " + ", ".join(w for _, w in missing))
        else:
            log("faltando: " + ", ".join(exe for exe, _ in missing))
            if confirm(f"instalar {len(missing)} pacote(s) via winget?"):
                for _, wid in missing:
                    run(["winget", "install", "--exact", "--id", wid,
                         "--accept-package-agreements", "--accept-source-agreements"],
                        check=False)
            else:
                skip("instalação via winget recusada")
        _report_windows_manual(host)
        return

    if host.pkg == "dnf":
        dnf = which("dnf5") or which("dnf")
        missing = _missing_rpm(src.fedora_packages)
        if not missing:
            ok(f"todos os {len(src.fedora_packages)} pacotes já instalados")
            return
        log(f"faltando {len(missing)} pacote(s):")
        for p in missing:
            print(f"{DIM}     {p}{OFF}")
        if not confirm("instalar agora?"):
            skip("instalação recusada — rode com --skip-packages para não perguntar")
            return
        cmd = ([which("sudo")] if use_sudo and os.geteuid() != 0 else []) + [
            dnf, "install", "-y", "--skip-unavailable", *missing
        ]
        run([c for c in cmd if c])
        ok("pacotes instalados")
        return

    if host.pkg in ("apt", "pacman"):
        pkgs = APT_PACKAGES if host.pkg == "apt" else PACMAN_PACKAGES
        missing = _missing_dpkg(pkgs) if host.pkg == "apt" else _missing_pacman(pkgs)
        if not missing:
            ok(f"todos os {len(pkgs)} pacotes já instalados")
            return

        unknown = _apt_unknown(missing) if host.pkg == "apt" else _pacman_unknown(missing)
        if unknown:
            warn(f"{len(unknown)} nome(s) que o seu {host.pkg} não conhece — "
                 f"pulados, resolva-os à parte:")
            for u in unknown:
                print(f"{DIM}     {u}{OFF}{_package_hint(host.pkg, u)}")
            missing = [p for p in missing if p not in unknown]
        if not missing:
            skip("nada resolvível para instalar")
            return

        log(f"faltando {len(missing)} pacote(s): " + " ".join(missing))
        if not confirm("instalar agora?"):
            skip("instalação recusada")
            return
        sudo = [which("sudo")] if use_sudo and os.geteuid() != 0 else []
        if host.pkg == "apt":
            run([c for c in sudo + ["apt-get", "update"] if c])
            run([c for c in sudo + ["apt-get", "install", "-y", *missing] if c], check=False)
        else:
            run([c for c in sudo + ["pacman", "-S", "--needed", "--noconfirm", *missing] if c],
                check=False)
        ok("pacotes instalados (confira acima se algum nome não existia)")
        return

    warn(f"gerenciador de pacotes não reconhecido em {host.distro or host.system} — "
         f"instale as deps à mão (a lista Fedora está no setup.sh)")


# Nomes que existem no repositório oficial mas num componente que muita gente
# não habilita — sem a dica, "não conhece esse pacote" não diz o que fazer.
_PACKAGE_HINTS = {
    "apt": {
        "libfdk-aac-dev": "  <- non-free (Debian) / multiverse (Ubuntu)",
        "libffmpeg-nvenc-dev": "  <- non-free (Debian) / multiverse (Ubuntu)",
    },
    "pacman": {},
}


def _package_hint(manager: str, package: str) -> str:
    return _PACKAGE_HINTS.get(manager, {}).get(package, "")


def _report_windows_manual(host: Host) -> None:
    """O que no Windows não dá para automatizar honestamente."""
    if host.vs_installs:
        ok(f"{len(host.vs_installs)} instalação(ões) do Visual Studio encontradas")
    else:
        warn("nenhum Visual Studio encontrado (vswhere não respondeu) — instale o VS 2026 "
             "com o workload 'Desktop development with C++'")
    if host.qt_dir:
        ok(f"Qt: {host.qt_dir}")
    else:
        warn(r"Qt não encontrado — instale um kit msvc*_64 (ex.: C:\Qt\6.11.2\msvc2022_64) "
             "e/ou exporte QTDIR; sem ele o preset vs2026 não configura")
    if host.vulkan_sdk:
        ok(f"Vulkan SDK: {host.vulkan_sdk}")
    else:
        warn("Vulkan SDK ausente — só faz falta para o app principal, que ainda não "
             "compila no Windows (docs/windows-msvc-context.md §6)")


# ══════════════════════════════════════════════════════════════════════════════
# Fase 2 — vcpkg
# ══════════════════════════════════════════════════════════════════════════════
def _vcpkg_ports(host: Host) -> List[str]:
    """No Linux só curl + reflectcpp: taglib e libwebp são compilados do fonte
    (setup.sh, fase 2). No Windows vale o vcpkg.json inteiro — lá o alvo é o app
    Qt e nada é compilado do external/."""
    if not host.is_windows:
        return ["curl", "reflectcpp[toml]"]
    manifest = REPO / "vcpkg.json"
    ports: List[str] = []
    if manifest.is_file():
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for dep in data.get("dependencies", []):
            if isinstance(dep, str):
                ports.append(dep)
            else:
                name = dep["name"]
                feats = dep.get("features") or []
                ports.append(f"{name}[{','.join(feats)}]" if feats else name)
    return ports or ["curl", "reflectcpp[toml]"]


def phase_vcpkg(host: Host) -> None:
    step(f"Fase 2 — vcpkg ({host.triplet})")
    root = host.vcpkg_root
    exe = root / ("vcpkg.exe" if host.is_windows else "vcpkg")
    bootstrap = root / ("bootstrap-vcpkg.bat" if host.is_windows else "bootstrap-vcpkg.sh")

    if not exe.is_file():
        if not bootstrap.is_file():
            if not confirm(f"vcpkg não existe em {root} — clonar lá?"):
                skip("vcpkg pulado")
                return
            run(["git", "clone", "https://github.com/microsoft/vcpkg.git", str(root)])
        log(f"bootstrap do vcpkg em {root}")
        run([str(bootstrap), "-disableMetrics"], cwd=root)

    if not DRY_RUN and not exe.is_file():
        die(f"vcpkg continua ausente em {exe}")

    if not host.is_windows and root != Path.home() / "vcpkg":
        warn(f"o CMakeLists resolve o vcpkg por $VCPKG_ROOT ou ~/vcpkg — exporte "
             f"VCPKG_ROOT={root} para o configure concordar")

    ports = _vcpkg_ports(host)
    log("vcpkg install " + " ".join(ports) + "  (no-op para o que já está construído)")
    # Modo clássico: rodar de dentro do $VCPKG_ROOT, que não tem vcpkg.json — de
    # dentro do repo o vcpkg entra em modo manifest e recusa argumentos de porta.
    run([str(exe), "install", *ports, "--triplet", host.triplet], cwd=root)
    ok(f"deps prontas em {root / 'installed' / host.triplet}")


# ══════════════════════════════════════════════════════════════════════════════
# Fase 3 — clone de external/
# ══════════════════════════════════════════════════════════════════════════════
def phase_clone(host: Host, src: Sources, force: bool = False) -> None:
    step("Fase 3 — fontes em external/ (refs fixados no setup.sh)")

    if host.is_windows and not force:
        skip("Windows: external/ é o stack de mídia do app Linux-only "
             "(docs/windows-msvc-context.md §6) — use --clone-anyway se quiser mesmo")
        return

    (REPO / "external").mkdir(exist_ok=True)
    for directory, url, ref, kind, flags in src.repos:
        dest = REPO / directory
        if (dest / ".git").is_dir():
            rc, out = capture(["git", "-C", str(dest), "describe", "--tags", "--always"])
            skip(f"{directory} já clonado ({out.strip() if rc == 0 else 'presente'})")
            continue
        log(f"clonando {DIM}{ref}{OFF} -> {directory}")
        args = ["git", "clone", "--depth", "1"]
        if kind in ("tag", "branch"):
            args += ["--branch", ref]
        if "recurse" in flags:
            args += ["--recurse-submodules", "--shallow-submodules"]
        run(args + [url, str(dest)])

    lp = REPO / "external" / "libplacebo"
    if (lp / ".git").is_dir():
        skip("external/libplacebo já clonado")
    else:
        log(f"clonando {DIM}{src.libplacebo_ref}{OFF} -> external/libplacebo")
        rc = run(["git", "clone", "--depth", "1", "--branch", src.libplacebo_ref,
                  src.libplacebo_url, str(lp)], check=False)
        if rc != 0:
            warn(f"VideoLAN falhou — tentando o espelho {src.libplacebo_mirror}")
            run(["git", "clone", "--depth", "1", "--branch", src.libplacebo_ref,
                 src.libplacebo_mirror, str(lp)])
    if (lp / ".git").is_dir() or DRY_RUN:
        # só os submódulos que o build precisa (nuklear é demo)
        run(["git", "-C", str(lp), "submodule", "update", "--init", "--depth", "1", "--",
             *src.libplacebo_submodules], check=False)
    ok("fontes presentes em external/")


# ══════════════════════════════════════════════════════════════════════════════
# Fase 4 — configure / build / run
# ══════════════════════════════════════════════════════════════════════════════
def app_binary(host: Host, config: str) -> Path:
    """Onde o executável realmente sai.

    No Linux vem do CMakeLists.txt:292-299 — repare que RelWithDebInfo vai para
    build/RELWITHDEBINFO/example_sdl3_vulkan_RELWITHDEBINFO, e NÃO para
    build/release-log/... como o build.sh supõe (o `--run release-log` dele erra
    o caminho)."""
    if host.is_windows:
        return REPO / "build" / "vs2026" / "QT" / "VulkanMedia" / config / "appVulkanMedia.exe"
    table = {
        "Debug": ("debug", "example_sdl3_vulkan_debug"),
        "Release": ("release", "example_sdl3_vulkan_release"),
        "RelWithDebInfo": ("RELWITHDEBINFO", "example_sdl3_vulkan_RELWITHDEBINFO"),
    }
    folder, name = table[config]
    return REPO / "build" / folder / name


def cmake_env(host: Host) -> Dict[str, str]:
    env: Dict[str, str] = {}
    if host.is_windows and host.qt_dir:
        env["QTDIR"] = to_target_path(host.qt_dir)   # o preset vs2026 lê $env{QTDIR}
    else:
        # o CMakeLists resolve o vcpkg por -DVCPKG_ROOT > $VCPKG_ROOT > ~/vcpkg
        env["VCPKG_ROOT"] = str(host.vcpkg_root)
    return env


def phase_configure(host: Host, fresh: bool = False) -> bool:
    step(f"Fase 4 — configure (preset '{host.preset}')")
    if not which("cmake"):
        warn("cmake não está no PATH — pulando o configure")
        return False
    if host.is_windows and not host.qt_dir:
        warn("sem Qt: o preset vs2026 precisa de QTDIR apontando para um kit msvc*_64. "
             "Configure pulado.")
        return False

    cache = host.build_dir / "CMakeCache.txt"
    # Não basta o CMakeCache.txt: um configure que morre depois de gravá-lo e
    # antes de gerar deixa a árvore sem os *.ninja / a solução, e todo build
    # seguinte morre em "loading 'build-Debug.ninja': No such file or directory".
    generated = (list(host.build_dir.glob("*.sln*")) if host.is_windows
                 else [p for p in [host.build_dir / "build-Debug.ninja"] if p.is_file()])
    if cache.is_file() and generated and not fresh:
        skip(f"{host.build_dir.relative_to(REPO)} já configurado (use --fresh para refazer)")
        return True

    cmd = ["cmake", "--preset", host.preset] + (["--fresh"] if fresh else [])
    rc = run(cmd, env=cmake_env(host), check=False)
    if rc != 0:
        warn("o configure falhou — veja a saída acima; as fases de IDE seguem mesmo assim")
        warn("se a queixa for um -dev/-devel faltando, rode a fase 1 "
             f"(`{'python' if host.is_windows else 'python3'} {Path(__file__).name} "
             "--skip-vcpkg --skip-clone --skip-ide`) e configure de novo")
        return False
    ok(f"configurado em build/{host.build_dir.name}")
    return True


def prebuild_media_stack(host: Host, config: str) -> None:
    """Os TUs do app incluem headers INSTALADOS pelos ExternalProjects de mídia, e
    o LINK_DEPENDS ordena o link, não a compilação — um build paralelo fresco
    corre na frente e morre em 'mpv/client.h' file not found. Idempotente."""
    if host.is_windows:
        return
    run(["cmake", "--build", str(host.build_dir), "--config", config,
         "--target", f"mpv_ep_{config}", f"ffmpegthumbnailer_ep_{config}"],
        env=cmake_env(host))


def phase_build(host: Host, configs: Sequence[str], jobs: Optional[int] = None) -> None:
    step("Build — " + ", ".join(configs))
    if not host.build_dir.is_dir():
        die(f"{host.build_dir} não existe — rode `python3 {Path(__file__).name}` antes")
    for config in configs:
        prebuild_media_stack(host, config)
        cmd = ["cmake", "--build", str(host.build_dir), "--config", config]
        if host.is_windows:
            cmd += ["--target", "appVulkanMedia"]
        if jobs:
            cmd += ["-j", str(jobs)]
        log(f"compilando {DIM}{config}{OFF} -> {app_binary(host, config)}")
        run(cmd, env=cmake_env(host))
        ok(f"{config} pronto")


def phase_run(host: Host, config: str) -> None:
    binary = app_binary(host, config)
    if not binary.is_file():
        die(f"binário não encontrado: {binary} (o build passou?)")
    env = {}
    if host.is_windows and host.qt_dir:
        env["PATH"] = str(host.qt_dir / "bin") + os.pathsep + os.environ.get("PATH", "")
    log(f"rodando {binary}")
    run([str(binary)], env=env, check=False)


# ══════════════════════════════════════════════════════════════════════════════
# doctor — relatório, sem tocar em nada
# ══════════════════════════════════════════════════════════════════════════════
def _tool_version(exe: str, *args: str) -> str:
    path = which(exe)
    if not path:
        return f"{RED}ausente{OFF}"
    rc, out = capture([path, *(args or ("--version",))])
    first = out.strip().splitlines()[0] if out.strip() else ""
    return f"{first or 'ok'}  {DIM}({path}){OFF}"


def doctor(host: Host, src: Optional[Sources] = None) -> None:
    step("Diagnóstico da máquina")
    label = host.distro or host.system
    print(f"  sistema        {platform.platform()}")
    if host.cross:
        print(f"  {YELLOW}alvo{OFF}           {host.system}"
              f"{' via ' + str(WIN_ROOT) if WIN_ROOT else ''}"
              f"  {DIM}(relatório do alvo, não desta máquina){OFF}")
    print(f"  distro/pkg     {label} / {host.pkg or '—'}")
    print(f"  repo           {REPO}")
    print(f"  preset do host {host.preset}  ->  build/{host.build_dir.name}")
    print(f"  triplet        {host.triplet}")
    print(f"  vcpkg          {to_target_path(host.vcpkg_root)} "
          f"{'' if (host.vcpkg_root / 'installed').is_dir() else RED + '(sem installed/)' + OFF}")
    print(f"  Qt             {to_target_path(host.qt_dir) if host.qt_dir else RED + 'não encontrado' + OFF}")
    print(f"  Vulkan SDK     {to_target_path(host.vulkan_sdk) if host.vulkan_sdk else DIM + 'não encontrado' + OFF}")

    print("\n  ferramentas" + (f"  {DIM}(desta máquina — o alvo tem as dele){OFF}"
                                 if host.cross else ""))
    for exe in ("cmake", "ninja", "git", "clang", "clang++", "clangd", "clang-format",
                "meson", "nasm", "pkg-config", "lldb", "gdb"):
        print(f"    {exe:<14} {_tool_version(exe)}")

    if host.is_windows:
        print("\n  Visual Studio")
        if not host.vs_installs:
            print(f"    {RED}nenhuma instalação encontrada{OFF}")
        for vs in host.vs_installs:
            print(f"    {vs.name}  {vs.version}  ({vs.product})")
            print(f"      {DIM}{to_target_path(vs.path)}{OFF}")
            for gen, toolsets in vs.toolsets.items():
                print(f"      {gen}: {', '.join(toolsets) if toolsets else DIM + 'vazio' + OFF}")
        cache = REPO / "build" / "vs2026" / "CMakeCache.txt"
        if cache.is_file():
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
                if "GENERATOR_INSTANCE" in line or "GENERATOR_TOOLSET" in line:
                    print(f"    cache: {line}")
        print(f"    {DIM}vswhere -latest e o CMake podem escolher instalações diferentes "
              f"— docs/windows-msvc-context.md §1{OFF}")

    if src:
        print("\n  external/ (refs do setup.sh)")
        missing = 0
        for directory, _url, ref, _kind, _flags in src.repos:
            present = (REPO / directory / ".git").is_dir()
            missing += 0 if present else 1
            mark = f"{GREEN}✓{OFF}" if present else f"{RED}✗{OFF}"
            print(f"    {mark} {directory:<34} {ref}")
        lp = (REPO / "external" / "libplacebo" / ".git").is_dir()
        missing += 0 if lp else 1
        print(f"    {(GREEN + '✓' + OFF) if lp else (RED + '✗' + OFF)} "
              f"{'external/libplacebo':<34} {src.libplacebo_ref}")
        if missing:
            print(f"    {YELLOW}{missing} repo(s) faltando — rode a fase clone{OFF}")

    print("\n  binários")
    for config in CONFIGS:
        binary = app_binary(host, config)
        state = f"{GREEN}existe{OFF}" if binary.is_file() else f"{DIM}não construído{OFF}"
        print(f"    {config:<15} {state}  {DIM}{binary.relative_to(REPO)}{OFF}")


# ══════════════════════════════════════════════════════════════════════════════
# Fase 5 — arquivos de IDE
# ══════════════════════════════════════════════════════════════════════════════
# .vscode/ e .idea/ estão no .gitignore (linhas 27-28), então nada disto entra no
# repositório: é config local, regerável por este script em qualquer máquina.

def _rel(path: Path) -> str:
    """Caminho relativo à raiz do repo, com barras normais (o VS Code aceita as
    duas no Windows). Fora do repo, devolve o absoluto."""
    try:
        return str(path.relative_to(REPO)).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")


def _task(label: str, args: List[str], group: Optional[str] = None,
          command: str = "cmake", detail: str = "", matcher: str = "$gcc") -> dict:
    task = {
        "label": label,
        "type": "shell",
        "command": command,
        "args": args,
        "options": {"cwd": "${workspaceFolder}"},
        "problemMatcher": [matcher],
        "presentation": {"reveal": "always", "panel": "shared", "clear": True},
    }
    if group:
        task["group"] = {"kind": group, "isDefault": label.endswith("Debug")}
    if detail:
        task["detail"] = detail
    return task


def ide_vscode(host: Host) -> None:
    step("IDE — VS Code (.vscode/)")
    vsc = REPO / ".vscode"
    build_dir = _rel(host.build_dir)
    # o MSVC não fala o formato de erro do gcc/clang
    matcher = "$msCompile" if host.is_windows else "$gcc"

    # ── settings ──────────────────────────────────────────────────────────────
    settings: Dict[str, object] = {
        "cmake.useCMakePresets": "always",
        "cmake.configureOnOpen": False,
        "cmake.sourceDirectory": "${workspaceFolder}",
        "files.associations": {"*.inl": "cpp", "*.hpp": "cpp", "*.ipp": "cpp"},
        "C_Cpp.formatting": "clangFormat",
        "editor.formatOnSave": True,
    }
    if host.is_windows:
        # No Windows o IntelliSense da MS é o que funciona com MSVC; o clangd
        # precisaria de um compile_commands.json que o gerador VS não emite.
        settings["cmake.environment"] = ({"QTDIR": to_target_path(host.qt_dir)}
                                         if host.qt_dir else {})
        settings["C_Cpp.intelliSenseEngine"] = "default"
        settings["C_Cpp.default.configurationProvider"] = "ms-vscode.cmake-tools"
    else:
        # Linux: clangd lê o compile_commands.json do build/all (o preset liga
        # CMAKE_EXPORT_COMPILE_COMMANDS). O IntelliSense da MS fica desligado
        # para os dois não brigarem por diagnósticos.
        settings["C_Cpp.intelliSenseEngine"] = "disabled"
        settings["clangd.arguments"] = [
            f"--compile-commands-dir=${{workspaceFolder}}/{build_dir}",
            "--background-index",
            "--clang-tidy",
            "--header-insertion=never",
            "--completion-style=detailed",
            "-j=4",
        ]
    merge_json(vsc / "settings.json", settings)

    # ── tasks ─────────────────────────────────────────────────────────────────
    tasks: List[dict] = [
        _task(f"cmake: configure ({host.preset})", ["--preset", host.preset],
              detail=f"cmake --preset {host.preset}", matcher=matcher),
    ]
    for config in CONFIGS:
        args = ["--build", build_dir, "--config", config]
        if host.is_windows:
            args += ["--target", "appVulkanMedia"]
        tasks.append(_task(f"build: {config}", args, group="build", matcher=matcher))
    if not host.is_windows:
        tasks.append(_task("build: all configs", ["--build", build_dir, "--target", "all_configs"],
                           detail="os três configs numa invocação"))
        tasks.append(_task("test: ctest (Debug)",
                           ["--test-dir", build_dir, "-C", "Debug", "--output-on-failure"],
                           command="ctest", group="test"))
    for config in CONFIGS:
        binary = app_binary(host, config)
        tasks.append({
            "label": f"run: {config}",
            # "process" e não "shell": executa direto, sem passar por
            # bash/PowerShell — caminho de repo com espaço não quebra.
            "type": "process",
            "command": "${workspaceFolder}/" + _rel(binary),
            "options": {"cwd": "${workspaceFolder}"},
            "dependsOn": f"build: {config}",
            "problemMatcher": [],
            "presentation": {"reveal": "always", "panel": "dedicated"},
        })
    tasks.append(_task("bootstrap: doctor", [Path(__file__).name, "doctor"],
                       command="python" if host.is_windows else sys.executable,
                       detail="relatório do ambiente", matcher=matcher))
    write_json(vsc / "tasks.json", {"version": "2.0.0", "tasks": tasks})

    # ── launch ────────────────────────────────────────────────────────────────
    if host.is_windows:
        qt_bin = to_target_path(host.qt_dir / "bin") if host.qt_dir else ""
        configurations = [{
            "name": f"VulkanMedia ({config})",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "${workspaceFolder}/" + _rel(app_binary(host, config)),
            "args": [],
            "cwd": "${workspaceFolder}",
            "environment": ([{"name": "PATH", "value": qt_bin + ";${env:PATH}"}] if qt_bin else []),
            "console": "integratedTerminal",
            "preLaunchTask": f"build: {config}",
        } for config in ("Debug", "RelWithDebInfo")]
    else:
        configurations = []
        for config in ("Debug", "RelWithDebInfo"):
            program = "${workspaceFolder}/" + _rel(app_binary(host, config))
            configurations.append({
                "name": f"lldb: {config}  (extensão CodeLLDB)",
                "type": "lldb",
                "request": "launch",
                "program": program,
                "args": [],
                "cwd": "${workspaceFolder}",
                "preLaunchTask": f"build: {config}",
                # o repo já traz um .lldbinit com os formatters do projeto
                "initCommands": ["command source ${workspaceFolder}/.lldbinit"],
            })
        configurations.append({
            "name": "gdb: Debug  (extensão C/C++)",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/" + _rel(app_binary(host, "Debug")),
            "args": [],
            "cwd": "${workspaceFolder}",
            "MIMode": "gdb",
            "preLaunchTask": "build: Debug",
            "setupCommands": [{
                "description": "pretty-printing do gdb",
                "text": "-enable-pretty-printing",
                "ignoreFailures": True,
            }],
        })
    write_json(vsc / "launch.json", {"version": "0.2.0", "configurations": configurations})

    # ── extensões recomendadas ────────────────────────────────────────────────
    recommendations = ["ms-vscode.cmake-tools", "twxs.cmake"]
    if host.is_windows:
        recommendations += ["ms-vscode.cpptools", "ms-vscode.cpptools-extension-pack"]
    else:
        recommendations += ["llvm-vs-code-extensions.vscode-clangd", "vadimcn.vscode-lldb",
                            "ms-vscode.cpptools"]
    write_json(vsc / "extensions.json", {"recommendations": recommendations})

    # ── c_cpp_properties (só onde o cpptools é o motor) ───────────────────────
    if host.is_windows:
        write_json(vsc / "c_cpp_properties.json", {
            "version": 4,
            "configurations": [{
                "name": "Win32",
                "configurationProvider": "ms-vscode.cmake-tools",
                "intelliSenseMode": "windows-msvc-x64",
                "cppStandard": "c++20",
                "includePath": ["${workspaceFolder}/**"]
                               + ([to_target_path(host.qt_dir / "include") + "/**"] if host.qt_dir else []),
            }],
        })

    link_compile_commands(host)


def link_compile_commands(host: Host) -> None:
    """compile_commands.json na raiz -> o do build tree (clangd olha para a raiz)."""
    source = host.build_dir / "compile_commands.json"
    target = REPO / "compile_commands.json"
    if not source.is_file():
        skip("compile_commands.json ainda não existe (aparece depois do primeiro configure)")
        return
    if DRY_RUN:
        print(f"{DIM}   [dry-run] ligaria compile_commands.json -> {_rel(source)}{OFF}")
        return
    if target.is_symlink() or target.exists():
        if target.is_symlink() and Path(os.readlink(target)) == Path(_rel(source)):
            skip("compile_commands.json já aponta para o build tree")
            return
        target.unlink()
    try:
        target.symlink_to(_rel(source))
        ok(f"compile_commands.json -> {_rel(source)}")
    except OSError:
        shutil.copy2(source, target)   # Windows sem Developer Mode: cópia
        ok("compile_commands.json copiado do build tree")


def ide_clion(host: Host, single_config: bool = False) -> None:
    step("IDE — CLion (.idea/)")
    idea = REPO / ".idea"

    def esc(s: str) -> str:
        return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")

    profiles: List[str] = []
    if host.is_windows:
        # TOOLCHAIN_NAME="Visual Studio" é o que o próprio CLion grava nesta
        # máquina (visto no .idea/cmake.xml do checkout Windows).
        opts = ['-G "Visual Studio 18 2026"', "-A x64", "-DBUILD_VULKAN_MEDIA_ONLY=ON"]
        if host.qt_dir:
            opts.append(f'-DCMAKE_PREFIX_PATH="{to_target_path(host.qt_dir)}"')
        for config in CONFIGS:
            profiles.append(
                f'      <configuration PROFILE_NAME="vs2026-{config}" ENABLED="{"true" if config == "Debug" else "false"}"'
                f' CONFIG_NAME="{config}" TOOLCHAIN_NAME="Visual Studio" GENERATION_DIR="build/vs2026"'
                f' GENERATION_OPTIONS="{esc(" ".join(opts))}" />'
            )
    elif single_config:
        # Uma árvore por config: CLion feliz com qualquer versão, mas cada árvore
        # recompila FFmpeg/mpv/libplacebo do zero. Por isso só o Debug vem ligado.
        base = ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                "-DCMAKE_LINKER_TYPE=LLD", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        for config in CONFIGS:
            profiles.append(
                f'      <configuration PROFILE_NAME="{config}" ENABLED="{"true" if config == "Debug" else "false"}"'
                f' CONFIG_NAME="{config}" GENERATION_DIR="build/clion-{config.lower()}"'
                f' GENERATION_OPTIONS="{esc(" ".join(base))}" />'
            )
    else:
        # Compartilha build/all com o build.sh e o VS Code: nada é recompilado
        # duas vezes. Exige um CLion que aceite geradores multi-config.
        opts = ['-G "Ninja Multi-Config"', "-DCMAKE_C_COMPILER=clang",
                "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_LINKER_TYPE=LLD",
                '-DCMAKE_CONFIGURATION_TYPES="Debug;Release;RelWithDebInfo"',
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        for config in CONFIGS:
            profiles.append(
                f'      <configuration PROFILE_NAME="all-{config}" ENABLED="{"true" if config == "Debug" else "false"}"'
                f' CONFIG_NAME="{config}" GENERATION_DIR="build/all"'
                f' GENERATION_OPTIONS="{esc(" ".join(opts))}" />'
            )

    write_file(idea / "cmake.xml", backup=False, content=
               '<?xml version="1.0" encoding="UTF-8"?>\n'
               '<project version="4">\n'
               '  <component name="CMakeSharedSettings">\n'
               '    <configurations>\n'
               + "\n".join(profiles) + "\n"
               '    </configurations>\n'
               '  </component>\n'
               '</project>\n')

    target = "appVulkanMedia" if host.is_windows else "example_sdl3_vulkan"
    project = "VulkanMedia" if host.is_windows else "example_sdl3_vulkan"
    # .run/<nome>.run.xml e não .idea/runConfigurations/: é onde o CLion desta
    # máquina grava as suas (visto no checkout Windows), e o .gitignore já cobre.
    for config in ("Debug", "RelWithDebInfo"):
        name = f"{target} ({config})"
        write_file(
            REPO / ".run" / f"{target}-{config}.run.xml", backup=False, content=
            '<component name="ProjectRunConfigurationManager">\n'
            f'  <configuration default="false" name="{name}" type="CMakeRunConfiguration"\n'
            f'                 factoryName="Application" PROGRAM_PARAMS="" REDIRECT_INPUT="false"\n'
            f'                 ELEVATE="false" USE_EXTERNAL_CONSOLE="false" EMULATE_TERMINAL="false"\n'
            f'                 WORKING_DIR="file://$CMakeProjectDir$" PASS_PARENT_ENVS_2="true"\n'
            f'                 PROJECT_NAME="{project}" TARGET_NAME="{target}"\n'
            f'                 CONFIG_NAME="{config}" RUN_TARGET_PROJECT_NAME="{project}"\n'
            f'                 RUN_TARGET_NAME="{target}">\n'
            '    <method v="2">\n'
            '      <option name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask"'
            ' enabled="true" />\n'
            '    </method>\n'
            '  </configuration>\n'
            '</component>\n')

    # Sem este marcador o CLion abre a pasta como projeto genérico: o cmake.xml
    # fica inerte e o workspace de CMake reporta "NotLoaded" (medido). É o mesmo
    # componente que o CLion grava sozinho quando você abre um CMakeLists.txt.
    misc = idea / "misc.xml"
    if not misc.is_file():
        write_file(misc, backup=False, content=
                   '<?xml version="1.0" encoding="UTF-8"?>\n'
                   '<project version="4">\n'
                   '  <component name="CMakeWorkspace" PROJECT_DIR="$PROJECT_DIR$" />\n'
                   '</project>\n')
    elif "CMakeWorkspace" not in misc.read_text(encoding="utf-8", errors="replace"):
        warn('.idea/misc.xml existe sem <component name="CMakeWorkspace" '
             'PROJECT_DIR="$PROJECT_DIR$" /> — acrescente essa linha, senão o '
             "CLion não anexa o projeto CMake")

    write_file(idea / ".gitignore", backup=False, content="# tudo em .idea/ é local\n/workspace.xml\n/usage.statistics.xml\n"
                                    "/shelf/\n/httpRequests/\n")
    print(f"{DIM}   o CLion também lê o CMakePresets.json direto (Settings > Build > CMake):{OFF}")
    print(f"{DIM}   se ele preferir os presets, use-os e apague o .idea/cmake.xml.{OFF}")


def ide_visual_studio(host: Host) -> None:
    step("IDE — Visual Studio")
    if not host.is_windows:
        skip("Visual Studio só existe no Windows — nada a gerar aqui "
             "(o CMakePresets.json já traz o preset 'vs2026')")
        return
    if not host.vs_installs:
        warn("nenhuma instalação do VS encontrada — instale o VS 2026 com 'Desktop "
             "development with C++' e rode de novo")
        return
    for vs in host.vs_installs:
        print(f"   {vs.name} {vs.version} ({vs.product})  {DIM}{to_target_path(vs.path)}{OFF}")
    print(f"{DIM}   atenção: `vswhere -latest` e o gerador do CMake podem escolher "
          f"instalações diferentes (docs/windows-msvc-context.md §1){OFF}")

    solutions = sorted(host.build_dir.glob("*.sln*")) if host.build_dir.is_dir() else []
    if solutions:
        ok(f"solução gerada: {_rel(solutions[0])}")
        print("   File > Open > Project/Solution — ou 'Open Folder' no repo, que o VS "
              "lê o CMakePresets.json sozinho.")
    else:
        warn("nenhuma solução em build/vs2026 — rode a fase configure "
             "(precisa de QTDIR apontando para um kit msvc*_64)")


# ══════════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════════
COMMANDS = ("setup", "doctor", "ide", "build", "run")


def _configs_from_args(values: Sequence[str]) -> List[str]:
    if not values or "all" in values:
        return list(CONFIGS)
    out = []
    for v in values:
        key = v.lower()
        if key not in CONFIG_ALIASES:
            die(f"config desconhecido '{v}' (use: debug | release | release-log | all)")
        out.append(CONFIG_ALIASES[key])
    return out


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog=Path(__file__).name,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__,
    )
    p.add_argument("command", nargs="?", default="setup", choices=COMMANDS,
                   help="setup (padrão) | doctor | ide | build | run")
    p.add_argument("configs", nargs="*", default=[],
                   help="para build/run: debug | release | release-log | all")

    p.add_argument("-y", "--yes", action="store_true", help="não perguntar nada")
    p.add_argument("-n", "--dry-run", action="store_true",
                   help="mostra o que faria, sem executar nem escrever")
    p.add_argument("--jobs", type=int, default=None, help="paralelismo do build")

    x = p.add_argument_group("outro checkout / outro SO")
    x.add_argument("--repo", type=Path, default=None,
                   help="agir sobre outro checkout em vez do diretório deste script")
    x.add_argument("--target-os", choices=("auto", "linux", "windows"), default="auto",
                   help="para qual SO gerar a config (padrão: o desta máquina)")
    x.add_argument("--windows-root", type=Path, default=None,
                   help="onde o C: do Windows está montado — implica --target-os windows "
                        "e faz a detecção ler o Qt/VS/vcpkg reais de lá")
    x.add_argument("--windows-drive", default="C:",
                   help="letra do drive que --windows-root representa (padrão: C:)")

    g = p.add_argument_group("fases do setup")
    g.add_argument("--skip-packages", action="store_true")
    g.add_argument("--skip-vcpkg", action="store_true")
    g.add_argument("--skip-clone", action="store_true")
    g.add_argument("--skip-configure", action="store_true")
    g.add_argument("--skip-ide", action="store_true")
    g.add_argument("--clone-anyway", action="store_true",
                   help="clonar external/ mesmo no Windows")
    g.add_argument("--fresh", action="store_true", help="reconfigurar do zero")
    g.add_argument("--no-sudo", action="store_true", help="nunca chamar sudo")
    g.add_argument("--vcpkg-root", type=Path, default=None)
    g.add_argument("--build", nargs="*", default=None, metavar="CONFIG",
                   help="também compilar ao final (sem argumento = Debug)")

    i = p.add_argument_group("IDEs")
    i.add_argument("--ide", default="all",
                   help="lista separada por vírgula: vscode,clion,vs,all,none")
    i.add_argument("--clion-single-config", action="store_true",
                   help="perfis CLion single-config (árvore por config) em vez de "
                        "compartilhar build/all com o Ninja Multi-Config")
    i.add_argument("--fix-clangd", action="store_true",
                   help="reescrever os -I absolutos obsoletos do .clangd para este repo")
    return p


def fix_clangd() -> None:
    """O .clangd traz -I absolutos de um caminho antigo do repo. Opt-in porque
    .clangd é versionado — mexer nele por padrão sujaria o diff de quem só queria
    configurar a máquina."""
    path = REPO / ".clangd"
    if not path.is_file():
        skip(".clangd não existe")
        return
    text = path.read_text(encoding="utf-8")
    stale = sorted(set(re.findall(r"-I(/[^\s]*?)/code(?:/|\b)", text)))
    roots = {s for s in stale if Path(s) != REPO}
    if not roots:
        ok(".clangd já aponta para este repo")
        return
    new = text
    for root in roots:
        new = new.replace(root, str(REPO))
    warn(f".clangd apontava para {', '.join(roots)}")
    write_file(path, new)


def summary(host: Host, ides: List[str]) -> None:
    step("Pronto")
    name = Path(__file__).name
    py = "python" if host.is_windows else "python3"
    print(f"""
  {GREEN}Construir{OFF}
    {DIM}{py} {name} build debug{OFF}          Debug
    {DIM}{py} {name} build all{OFF}            os três configs
    {DIM}{py} {name} run debug{OFF}            compila e roda
    {DIM}cmake --build {_rel(host.build_dir)} --config Debug{OFF}

  {GREEN}IDEs{OFF} {DIM}(gerado: {', '.join(ides) if ides else 'nada'}){OFF}""")
    if "vscode" in ides:
        print(f"    VS Code   abra a pasta; Ctrl+Shift+B = build, F5 = debug.")
    if "clion" in ides:
        print(f"    CLion     abra a pasta; os perfis vêm do .idea/cmake.xml "
              f"(só o Debug ligado).")
    if "vs" in ides and host.is_windows:
        print(f"    VS 2026   'Open Folder' no repo (lê o CMakePresets.json) ou abra "
              f"build/vs2026/*.slnx.")
    if not host.is_windows:
        print(f"""
  {DIM}O app principal (example_sdl3_vulkan) é Linux-only; no Windows o alvo é
  QT/VulkanMedia -> appVulkanMedia.exe. Ver docs/windows-msvc-context.md §6.{OFF}""")


def main(argv: Optional[List[str]] = None) -> int:
    global DRY_RUN, ASSUME_YES, REPO, WIN_ROOT, WIN_DRIVE
    args = build_parser().parse_args(argv)
    DRY_RUN = args.dry_run
    ASSUME_YES = args.yes

    if args.repo:
        REPO = args.repo.expanduser().resolve()
        if not (REPO / "CMakeLists.txt").is_file():
            die(f"{REPO} não parece um checkout deste projeto (sem CMakeLists.txt)")
    target_os = None if args.target_os == "auto" else args.target_os
    if args.windows_root:
        WIN_ROOT = args.windows_root.expanduser().resolve()
        WIN_DRIVE = args.windows_drive.rstrip("/\\")
        if not WIN_ROOT.is_dir():
            die(f"--windows-root {WIN_ROOT} não existe")
        target_os = target_os or "windows"

    host = detect_host(target_os)
    if args.vcpkg_root:
        host.vcpkg_root = args.vcpkg_root.expanduser().resolve()
    if IS_MAC:
        warn("macOS não é um alvo suportado deste projeto — seguindo mesmo assim, "
             "espere fases falhando")

    if args.command == "doctor":
        src = None
        try:
            src = parse_setup_sh()
        except SystemExit:
            pass
        doctor(host, src)
        return 0

    if host.cross and args.command in ("build", "run"):
        die(f"não dá para {args.command} um alvo {host.system} a partir de "
            f"{'windows' if IS_WINDOWS else 'linux'} — rode isto lá")

    if args.command == "build":
        phase_build(host, _configs_from_args(args.configs) or ["Debug"], args.jobs)
        return 0

    if args.command == "run":
        configs = _configs_from_args(args.configs or ["debug"])
        phase_build(host, configs[:1], args.jobs)
        phase_run(host, configs[0])
        return 0

    wanted = [s.strip().lower() for s in args.ide.split(",") if s.strip()]
    if "none" in wanted:
        ides: List[str] = []
    elif "all" in wanted:
        ides = ["vscode", "clion"] + (["vs"] if host.is_windows else [])
    else:
        ides = wanted
    for name in ides:
        if name not in ("vscode", "clion", "vs"):
            die(f"IDE desconhecido '{name}' (use vscode, clion, vs, all ou none)")

    if args.command == "ide":
        if "vscode" in ides:
            ide_vscode(host)
        if "clion" in ides:
            ide_clion(host, args.clion_single_config)
        if "vs" in ides:
            ide_visual_studio(host)
        if args.fix_clangd:
            fix_clangd()
        summary(host, ides)
        return 0

    # ── setup ─────────────────────────────────────────────────────────────────
    src = parse_setup_sh()
    log(f"Setup de {DIM}{REPO}{OFF}  "
        f"({host.distro or host.system}, preset '{host.preset}', "
        f"vcpkg {to_target_path(host.vcpkg_root)})")
    if host.cross:
        # Instalar pacote, rodar vcpkg.exe ou cmake do outro SO daqui não é
        # possível; o que atravessa é a config de IDE, que é texto.
        warn(f"alvo {host.system} a partir de {'windows' if IS_WINDOWS else 'linux'}: "
             f"só as fases de IDE rodam — pacotes, vcpkg, clone e configure ficam "
             f"para quando você bootar o alvo")
        args.skip_packages = args.skip_vcpkg = args.skip_clone = args.skip_configure = True
    if DRY_RUN:
        warn("--dry-run: nada será instalado, clonado ou escrito")

    if args.skip_packages:
        skip("fase 1 (pacotes)")
    else:
        phase_packages(host, src, use_sudo=not args.no_sudo)

    if args.skip_vcpkg:
        skip("fase 2 (vcpkg)")
    else:
        phase_vcpkg(host)

    if args.skip_clone:
        skip("fase 3 (clone)")
    else:
        phase_clone(host, src, force=args.clone_anyway)

    if args.skip_configure:
        skip("fase 4 (configure)")
    else:
        phase_configure(host, fresh=args.fresh)

    if args.skip_ide or not ides:
        skip("fase 5 (IDEs)")
    else:
        if "vscode" in ides:
            ide_vscode(host)
        if "clion" in ides:
            ide_clion(host, args.clion_single_config)
        if "vs" in ides:
            ide_visual_studio(host)

    if args.fix_clangd:
        fix_clangd()

    if args.build is not None:
        phase_build(host, _configs_from_args(args.build) or ["Debug"], args.jobs)

    summary(host, ides)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        die("interrompido", 130)
