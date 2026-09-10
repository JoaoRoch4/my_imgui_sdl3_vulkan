#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_doctor.py — saúde da árvore de build: preset, gerador, deps do ninja,
                  PCH, compile_commands e o vcpkg (portas E features).

  python3 scripts/build_doctor.py            relatório
  python3 scripts/build_doctor.py --json     estruturado

Cobre as armadilhas que já morderam este projeto:
  * árvore meio-configurada: CMakeCache.txt presente sem os build-<Config>.ninja
    — todo build morre em "loading 'build-Debug.ninja': No such file or directory"
    enquanto o configure segue sendo pulado (build.sh:74-84);
  * .ninja_deps corrompido: todo build recompila app + SDL3 + thirdparty. Aqui o
    diagnóstico é medido com um `ninja -n` (dry run), não adivinhado pelo tamanho;
  * cmake trocado no meio do caminho (env conda com cmake próprio) — a árvore
    guarda o CMAKE_COMMAND com que foi gerada;
  * vcpkg com a porta certa e a FEATURE faltando: foi assim que o reflectcpp
    ficou sem [toml] e o link morreu em rfl::toml::Writer::add_object_to_array.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from doctorlib import (BAD, BUILD_DIR, CONFIGS, DIM, GREEN, OFF, OK, RED,  # noqa: E402
                       REPO, YELLOW, Findings, capture, conda_shadow, head,
                       human, line, main_wrapper, mtime, read_cache, stamp,
                       tool_version)

# Acima disto, um build "sem nada para fazer" não é mais incremental: é o
# .ninja_deps pedindo para ser jogado fora. Dezenas de passos são normais (um
# .cpp tocado, um alvo de teste nunca compilado); o sintoma da corrupção é a
# árvore inteira, na casa dos milhares. O número oscila entre medições quando o
# CMake regenera no meio (re-glob), então leia a ordem de grandeza, não o valor.
PENDING_EDGES_SUSPECT = 200


def ninja_pending(config: str) -> Tuple[int, List[str]]:
    """Quantas arestas o ninja executaria agora. 0-2 = árvore em dia."""
    ninja_file = BUILD_DIR / f"build-{config}.ninja"
    if not ninja_file.is_file() or not shutil.which("ninja"):
        return -1, []
    rc, out = capture(["ninja", "-C", str(BUILD_DIR), "-f", str(ninja_file), "-n"],
                      timeout=180)
    if rc != 0:
        return -1, [l for l in out.splitlines() if l.strip()][:5]
    # O ninja numera cada passo como "[i/N]"; o N é o total que ele executaria.
    # Contar linhas erraria por causa do "ninja: Entering directory" e afins.
    totals = [int(m.group(1)) for m in re.finditer(r"^\[\d+/(\d+)\]", out, re.M)]
    steps = [l for l in out.splitlines() if l.startswith("[")]
    return (max(totals) if totals else 0), steps[:5]


def vcpkg_installed() -> Dict[str, Dict[str, object]]:
    """Lê vcpkg_installed/vcpkg/status: {porta: {version, features:[...]}}."""
    status = REPO / "vcpkg_installed" / "vcpkg" / "status"
    out: Dict[str, Dict[str, object]] = {}
    if not status.is_file():
        return out
    for block in status.read_text(encoding="utf-8", errors="replace").split("\n\n"):
        fields = dict(
            (k.strip(), v.strip())
            for k, _, v in (ln.partition(":") for ln in block.splitlines() if ":" in ln))
        name = fields.get("Package")
        if not name or "install ok installed" not in fields.get("Status", ""):
            continue
        entry = out.setdefault(name, {"version": fields.get("Version", "?"),
                                      "features": []})
        if feat := fields.get("Feature"):
            entry["features"].append(feat)          # type: ignore[union-attr]
        elif fields.get("Version"):
            entry["version"] = fields["Version"]
    return out


def vcpkg_manifest() -> List[Tuple[str, List[str]]]:
    """As dependências declaradas em vcpkg.json, com as features de cada uma."""
    mf = REPO / "vcpkg.json"
    if not mf.is_file():
        return []
    try:
        data = json.loads(mf.read_text(encoding="utf-8", errors="replace"))
    except json.JSONDecodeError:
        return []
    out = []
    for dep in data.get("dependencies", []):
        if isinstance(dep, str):
            out.append((dep, []))
        else:
            out.append((dep.get("name", "?"), list(dep.get("features", []))))
    return out


def collect() -> Tuple[dict, Findings]:
    f = Findings()
    cache = read_cache()
    data: dict = {
        "build_dir": str(BUILD_DIR),
        "configured": bool(cache),
        "cache": {k: cache.get(k, "") for k in (
            "CMAKE_GENERATOR", "CMAKE_COMMAND", "CMAKE_CXX_COMPILER",
            "CMAKE_C_COMPILER", "CMAKE_LINKER_TYPE", "CMAKE_CONFIGURATION_TYPES",
            "CMAKE_EXPORT_COMPILE_COMMANDS")},
        "tools": {t: tool_version(t, *args) for t, args in (
            ("cmake", ()), ("ninja", ("--version",)), ("clang", ()),
            ("clang++", ()), ("ld.lld", ("--version",)), ("ccache", ()),
            ("pkg-config", ()))},
        "generated": {}, "ninja_deps": {}, "pch": {}, "pending": {},
        "vcpkg": {}, "conda": conda_shadow(),
    }

    if not cache:
        f.error(f"{BUILD_DIR.relative_to(REPO)} não está configurado — "
                "rode `python3 build.py` (ou `cmake --preset all`)")
        return data, f

    # 1. generate completo? (a árvore meio-configurada do build.sh:74-84)
    for cfg in CONFIGS:
        p = BUILD_DIR / f"build-{cfg}.ninja"
        data["generated"][cfg] = {"file": str(p), "exists": p.is_file()}
        if not p.is_file():
            f.error(f"falta {p.relative_to(REPO)} — cache presente e generate "
                    "incompleto; reconfigure com `cmake --preset all`")

    # 2. o cmake que gerou a árvore ainda é o do PATH?
    generator_cmake = cache.get("CMAKE_COMMAND", "")
    current = shutil.which("cmake") or ""
    data["cmake_same"] = (generator_cmake == current)
    if generator_cmake and current and generator_cmake != current:
        f.warn(f"a árvore foi gerada com {generator_cmake} e o PATH agora tem "
               f"{current} — construir com o outro força regeneração completa")
    if data["conda"]:
        f.warn("env conda ativo sombreando ferramentas de build: "
               + "; ".join(data["conda"]))

    # 3. .ninja_deps + quanto trabalho um build faria agora
    deps = BUILD_DIR / ".ninja_deps"
    data["ninja_deps"] = {
        "path": str(deps), "exists": deps.is_file(),
        "size": deps.stat().st_size if deps.is_file() else 0,
        "older_than_build_ninja": (deps.is_file()
                                   and mtime(deps) < mtime(BUILD_DIR / "build.ninja")),
    }
    for cfg in CONFIGS:
        count, sample = ninja_pending(cfg)
        data["pending"][cfg] = {"edges": count, "sample": sample}
        if count > PENDING_EDGES_SUSPECT:
            f.error(f"{cfg}: um build agora executaria ~{count} passos sem que nada "
                    "tenha mudado — sintoma clássico de .ninja_deps corrompido; "
                    "apague build/all/.ninja_deps (ou `build.py --fresh`)")

    # 4. PCH: cmake_pch.hxx é gerado no CONFIGURE e o ninja não sabe recriá-lo
    for cfg in CONFIGS:
        d = BUILD_DIR / "CMakeFiles" / "example_sdl3_vulkan.dir" / cfg
        hxx = d / "cmake_pch.hxx"
        objs = len(list(d.rglob("*.o"))) if d.is_dir() else 0
        data["pch"][cfg] = {"dir": str(d), "cmake_pch": hxx.is_file(), "objects": objs}
        if d.is_dir() and not hxx.is_file():
            f.error(f"{cfg}: falta {hxx.relative_to(REPO)} — o ninja não tem regra "
                    "para recriá-lo; reconfigure em vez de apagar a pasta do alvo")

    # 5. compile_commands.json — o link da raiz é o que o clangd lê
    link = REPO / "compile_commands.json"
    target = BUILD_DIR / "compile_commands.json"
    entries = 0
    if target.is_file():
        try:
            entries = len(json.loads(target.read_text(errors="replace")))
        except json.JSONDecodeError:
            f.warn("build/all/compile_commands.json não é JSON válido")
    data["compile_commands"] = {
        "link": str(link), "is_symlink": link.is_symlink(),
        "resolves": link.exists(), "target": str(target),
        "target_exists": target.is_file(), "entries": entries}
    if not link.exists():
        f.error("compile_commands.json da raiz não resolve — clangd fica cego "
                "(`ln -sf build/all/compile_commands.json .`)")

    # 6. vcpkg: porta instalada não basta, a FEATURE é que traz os símbolos
    installed = vcpkg_installed()
    manifest = vcpkg_manifest()
    missing_ports, missing_features = [], []
    for name, feats in manifest:
        got = installed.get(name)
        if got is None:
            missing_ports.append(name)
            continue
        for feat in feats:
            if feat not in got["features"]:                # type: ignore[operator]
                missing_features.append(f"{name}[{feat}]")
    data["vcpkg"] = {
        "manifest_deps": len(manifest), "installed_ports": len(installed),
        "missing_ports": missing_ports, "missing_features": missing_features,
        "installed": {k: v for k, v in sorted(installed.items())},
    }
    if missing_features:
        f.error("vcpkg com feature faltando: " + ", ".join(missing_features)
                + " — reinstale com --recurse (sem ele o vcpkg recusa o REBUILD "
                  "e o link morre em símbolo indefinido)")
    if missing_ports:
        f.note("portas do manifesto não instaladas neste triplet: "
               + ", ".join(missing_ports[:8])
               + (" ..." if len(missing_ports) > 8 else "")
               + " — normal no Linux, onde o CMakeLists só consome curl e reflectcpp")
    return data, f


def render(d: dict) -> None:
    print(f"{DIM}build_doctor — {d['build_dir']}{OFF}")

    head("ferramentas")
    for name, ver in d["tools"].items():
        line(name, ver)

    head("cache do CMake")
    for k, v in d["cache"].items():
        line(k, v or f"{DIM}<vazio>{OFF}")
    line("cmake do PATH == o que gerou?",
         OK if d.get("cmake_same") else f"{YELLOW}não{OFF}")

    head("generate por config")
    for cfg, info in d["generated"].items():
        line(cfg, OK if info["exists"] else BAD)

    head("trabalho pendente (ninja -n) e .ninja_deps")
    nd = d["ninja_deps"]
    line(".ninja_deps", f"{human(nd['size'])}"
         + (f"  {YELLOW}mais velho que o build.ninja{OFF}"
            if nd["older_than_build_ninja"] else "") if nd["exists"] else BAD)
    for cfg, info in d["pending"].items():
        n = info["edges"]
        state = (f"{DIM}não medido{OFF}" if n < 0 else
                 f"{GREEN}{n} passos{OFF}" if n <= 5 else
                 f"{YELLOW}{n} passos{OFF}" if n <= 200 else f"{RED}{n} passos{OFF}")
        line(cfg, state)

    head("PCH e objetos do app")
    for cfg, info in d["pch"].items():
        line(cfg, f"cmake_pch.hxx {OK if info['cmake_pch'] else BAD}   "
                  f"{info['objects']} objetos")

    head("compile_commands.json")
    cc = d["compile_commands"]
    line("link da raiz", (OK if cc["resolves"] else BAD)
         + (f" {DIM}(symlink){OFF}" if cc["is_symlink"] else ""))
    line("entradas", str(cc["entries"]))

    head("vcpkg")
    v = d["vcpkg"]
    line("declaradas no vcpkg.json", str(v["manifest_deps"]))
    line("instaladas no triplet", str(v["installed_ports"]))
    line("features faltando", ", ".join(v["missing_features"]) or OK)
    for name, info in list(v["installed"].items())[:40]:
        feats = ",".join(info["features"]) or "-"
        print(f"    {name:<28} {info['version']:<14} {DIM}features: {feats}{OFF}")


if __name__ == "__main__":
    raise SystemExit(main_wrapper(collect, render, sys.argv[1:]))
