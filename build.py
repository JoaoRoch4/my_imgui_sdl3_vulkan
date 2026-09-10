#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build.py — port multiplataforma do build.sh (Windows, Linux e macOS).

Mesmas etapas do script bash — configure, pré-build da stack de mídia, build,
test, perf, run — só que escolhendo o preset e os caminhos pelo host, em vez de
assumir Fedora + Ninja Multi-Config. A detecção de host (preset, QTDIR, vcpkg,
Visual Studio) vem do bootstrap.py, que já é a fonte única disso no repo.

  python3 build.py                  compila os TRÊS configs (padrão)
  python3 build.py debug            só Debug        -> build/debug
  python3 build.py release          só Release      -> build/release
  python3 build.py release-log      só RelWithDebInfo -> build/RELWITHDEBINFO
  python3 build.py all              os três, explicitamente

Flags (combináveis):
  --run        roda o app do config compilado (com vários, prefere o Debug).
  --perf       perfila com o `perf` do Linux (call-graph dwarf) -> perf.data.
               Prefere o RelWithDebInfo: otimizado e ainda com símbolos.
  --test       compila e roda a suíte (image_tests, image_job_tests,
               thumbnail_blob_tests) via ctest.
  --fresh      reconfigura do zero antes de compilar (no Ninja, apaga o
               .ninja_deps; nos outros geradores, `cmake --preset --fresh`).
  --rebuild    recompila o APP inteiro (TUs + PCH + link) PRESERVANDO as libs
               dos ExternalProjects (ffmpeg, mpv, sdl3, libplacebo) e o
               build/cache (thumbnails, cache de vídeo, TOML).
  -j N         paralelismo do build.
  --dry-run    só mostra os comandos, não executa nada.

Diferenças propositais em relação ao build.sh:
  • RelWithDebInfo sai em build/RELWITHDEBINFO/example_sdl3_vulkan_RELWITHDEBINFO
    (CMakeLists.txt:293-299). O build.sh procura em build/release-log/… e erra o
    caminho no --run/--perf; aqui o caminho vem do bootstrap.app_binary().
  • --test também compila thumbnail_blob_tests. O ctest roda os três testes
    registrados (CMakeLists.txt:577,640,679); compilar só dois deixa o terceiro
    falhando por executável ausente.
  • No Windows o preset é o vs2026 e o alvo é o appVulkanMedia — o preset liga
    BUILD_VULKAN_MEDIA_ONLY, então lá não existem os alvos de teste nem os
    ExternalProjects de mídia (CMakeLists.txt:34-49).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import List, Optional

REPO = Path(__file__).resolve().parent

try:
    sys.path.insert(0, str(REPO))
    import bootstrap  # noqa: E402  (vizinho no repo, não é dependência externa)
except ImportError as exc:                                    # pragma: no cover
    print(f"err  build.py precisa do bootstrap.py ao lado dele ({exc})",
          file=sys.stderr)
    raise SystemExit(1)

from bootstrap import (BLUE, DIM, OFF,  # noqa: E402
                       die, log, ok, run, warn, which)

CONFIG_KEYWORDS = ("debug", "release", "release-log")


def config_name(keyword: str) -> str:
    """keyword do build.sh -> configuração do CMake."""
    try:
        return bootstrap.CONFIG_ALIASES[keyword.lower()]
    except KeyError:
        die(f"config desconhecido '{keyword}' (esperado: "
            + " | ".join(CONFIG_KEYWORDS) + ")")


# ══════════════════════════════════════════════════════════════════════════════
# configure
# ══════════════════════════════════════════════════════════════════════════════
def generated_marker(host: "bootstrap.Host") -> Optional[Path]:
    """Um arquivo que só existe se o generate (não só o configure) terminou.

    Não basta olhar o CMakeCache.txt: um configure que morre DEPOIS de gravar o
    cache e ANTES de gerar deixa uma árvore com cache e sem build-Debug.ninja, e
    aí todo build morre em "loading 'build-Debug.ninja': No such file or
    directory" enquanto o configure segue sendo pulado (cache presente)."""
    if host.is_windows:
        sln = sorted(host.build_dir.glob("*.sln*"))
        return sln[0] if sln else None
    ninja = host.build_dir / "build-Debug.ninja"
    return ninja if ninja.is_file() else None


def configure(host: "bootstrap.Host", fresh: bool = False) -> None:
    if not which("cmake"):
        die("cmake não está no PATH")
    if (host.build_dir / "CMakeCache.txt").is_file() and generated_marker(host) \
            and not fresh:
        return
    log(f"Configurando o preset '{host.preset}'...")
    if host.is_windows and not host.qt_dir:
        warn("QTDIR não encontrado — o preset vs2026 lê $env{QTDIR} e vai falhar "
             "sem um kit msvc*_64 (rode `python bootstrap.py doctor`)")
    cmd = ["cmake", "--preset", host.preset]
    # No Ninja o --fresh já foi tratado apagando o .ninja_deps (é o que o
    # build.sh chama de --fresh); nos outros geradores não há .ninja_deps, então
    # aqui --fresh vira o do próprio CMake: joga o cache fora e regenera.
    if fresh and not uses_ninja(host):
        cmd.append("--fresh")
    run(cmd, env=bootstrap.cmake_env(host))


# ══════════════════════════════════════════════════════════════════════════════
# .ninja_deps (só faz sentido nos geradores Ninja)
# ══════════════════════════════════════════════════════════════════════════════
def uses_ninja(host: "bootstrap.Host") -> bool:
    return not host.is_windows


# Armadilha conhecida (memória do projeto "Ninja deps rebuild gotcha"): um
# build/all/.ninja_deps corrompido faz TODO build recompilar o app inteiro +
# SDL3 + thirdparty. A cura é apagar esse único arquivo e fazer um build limpo —
# NÃO destruir o CMake nem re-globar.
#
# ┌─ SUA CONTRIBUIÇÃO ──────────────────────────────────────────────────────────┐
# │ (o mesmo TODO que está aberto no build.sh; portado sem inventar política)   │
# │ Quão agressivo deve ser o auto-conserto?                                    │
# │   • heurística automática = nenhum rebuild-surpresa, mas um palpite errado  │
# │     joga fora estado incremental bom e custa um build lento;                │
# │   • devolver False sempre = previsível; você reseta com --fresh ao notar.   │
# │ Ideias: tamanho/idade (um .ninja_deps de muitos MB mais velho que o         │
# │ build.ninja é suspeito); sonda (um `ninja -n` que lista quase todo objeto); │
# │ sentinela (hash carimbado após cada build limpo).                           │
# └─────────────────────────────────────────────────────────────────────────────┘
def ninja_deps_looks_stale(host: "bootstrap.Host") -> bool:
    return False   # False = "parece são, não mexe"


def reset_ninja_deps(host: "bootstrap.Host") -> None:
    deps = host.build_dir / ".ninja_deps"
    if not deps.is_file():
        return
    warn(f"Resetando {deps.relative_to(REPO)} (força um build limpo)")
    if not bootstrap.DRY_RUN:
        deps.unlink()


# ══════════════════════════════════════════════════════════════════════════════
# build
# ══════════════════════════════════════════════════════════════════════════════
def app_target(host: "bootstrap.Host") -> str:
    return "appVulkanMedia" if host.is_windows else "example_sdl3_vulkan"


def app_object_dir(host: "bootstrap.Host", config: str) -> Optional[Path]:
    """A pasta de objetos do alvo do app, para o config pedido.

    Ninja/Makefiles: build/all/CMakeFiles/<alvo>.dir/<Config>.
    Visual Studio:   build/vs2026/QT/VulkanMedia/<alvo>.dir/<Config>.
    O glob cobre os dois sem fixar o caminho do subdiretório."""
    target = app_target(host)
    direct = host.build_dir / "CMakeFiles" / f"{target}.dir" / config
    if direct.is_dir():
        return direct
    for candidate in host.build_dir.glob(f"**/{target}.dir/{config}"):
        if candidate.is_dir():
            return candidate
    return None


def rebuild_clean_app(host: "bootstrap.Host", config: str) -> None:
    """Apaga só as SAÍDAS COMPILADAS do app: objetos, depfiles e o PCH compilado.

    Não apagar a pasta inteira: ela também guarda cmake_pch.hxx / cmake_pch.cxx,
    que o CMake gera no CONFIGURE e o ninja não sabe recriar — sem eles o build
    trava em "cmake_pch.hxx ... missing and no known rule to make it". O .pch é
    reconstruído a partir do cmake_pch.hxx, então removê-lo só força um PCH novo.

    Os ExternalProjects (build/*/thirdparty: ffmpeg, mpv, sdl3, libplacebo) e o
    build/cache ficam intactos: os stamps deles seguem válidos."""
    obj_dir = app_object_dir(host, config)
    binary = bootstrap.app_binary(host, config)
    if obj_dir is None:
        warn(f"ainda não há objetos do app para {config} — o build completo os cria")
        return
    log(f"Rebuild: limpando os objetos do app de {DIM}{config}{OFF} "
        f"(mantendo thirdparty + cache)")
    victims = [p for p in obj_dir.rglob("*") if p.is_file()
               and (p.suffix in (".o", ".obj", ".pch") or p.name.endswith(".o.d"))]
    if bootstrap.DRY_RUN:
        print(f"{DIM}   [dry-run] apagaria {len(victims)} arquivos em "
              f"{obj_dir.relative_to(REPO)} + {binary.name}{OFF}")
        return
    for path in victims:
        path.unlink()
    binary.unlink(missing_ok=True)
    print(f"{DIM}   {len(victims)} arquivos removidos em "
          f"{obj_dir.relative_to(REPO)}{OFF}")


def prebuild_media_stack(host: "bootstrap.Host", config: str) -> None:
    """Os TUs do app dão #include em headers INSTALADOS pelos ExternalProjects de
    mídia (<libavcodec/*>, <mpv/client.h>, libplacebo). Esses alvos são por-config
    e EXCLUDE_FROM_ALL, puxados só pelo LINK_DEPENDS do app — que ordena o LINK, e
    não a COMPILAÇÃO. Um build paralelo fresco então corre na frente com o PCH/os
    objetos e morre em "'mpv/client.h' file not found". Compilar a stack de mídia
    deste config ANTES resolve (mpv_ep puxa ffmpeg_ep + libplacebo_ep; o EP do
    thumbnailer dá a libffmpegthumbnailer.a). Idempotente: o ninja pula stamps
    já prontos."""
    if host.is_windows:
        return   # o preset vs2026 é BUILD_VULKAN_MEDIA_ONLY: não há esses EPs
    log(f"Pré-build da stack de mídia de {DIM}{config}{OFF} "
        f"(ffmpeg + libplacebo + mpv + thumbnailer)")
    run(["cmake", "--build", str(host.build_dir), "--config", config,
         "--target", f"mpv_ep_{config}", f"ffmpegthumbnailer_ep_{config}"],
        env=bootstrap.cmake_env(host))


def build_config(host: "bootstrap.Host", config: str, jobs: Optional[int]) -> None:
    prebuild_media_stack(host, config)
    binary = bootstrap.app_binary(host, config)
    rel = binary.relative_to(REPO) if binary.is_relative_to(REPO) else binary
    log(f"Compilando {DIM}{config}{OFF}  ->  {rel}")
    cmd = ["cmake", "--build", str(host.build_dir), "--config", config]
    if host.is_windows:
        cmd += ["--target", app_target(host)]
    if jobs:
        cmd += ["-j", str(jobs)]
    run(cmd, env=bootstrap.cmake_env(host))
    ok(f"{config} compilado")


# ══════════════════════════════════════════════════════════════════════════════
# run / perf / test
# ══════════════════════════════════════════════════════════════════════════════
def run_app(host: "bootstrap.Host", config: str) -> None:
    binary = bootstrap.app_binary(host, config)
    if not binary.is_file() and not bootstrap.DRY_RUN:
        die(f"binário não encontrado: {binary} (o build passou?)")
    env = {}
    if host.is_windows and host.qt_dir:
        # sem as DLLs do Qt no PATH o appVulkanMedia nem abre
        env["PATH"] = str(host.qt_dir / "bin") + os.pathsep + os.environ.get("PATH", "")
    log(f"Rodando {binary}")
    run([str(binary)], env=env or None, check=False)


def run_perf(host: "bootstrap.Host", config: str) -> None:
    binary = bootstrap.app_binary(host, config)
    if not binary.is_file() and not bootstrap.DRY_RUN:
        die(f"binário não encontrado: {binary} (o build passou?)")
    if host.is_windows or not bootstrap.IS_LINUX:
        die("--perf usa o `perf` do Linux. No Windows use o profiler do Visual "
            "Studio (Debug > Performance Profiler) ou o Superluminal; no macOS, "
            "o Instruments (`xcrun xctrace record --template 'Time Profiler'`)")
    if not which("perf"):
        die("perf não encontrado — instale as ferramentas 'perf' do Linux")
    if config != "RelWithDebInfo":
        warn(f"perfilando '{config}' — RelWithDebInfo (release-log) dá os "
             "hotspots mais fiéis")
    # --call-graph dwarf: build otimizado omite frame pointer, então o unwind vai
    # pelo CFI do DWARF que o -g emite (andar pela cadeia de fp estaria quebrado).
    log(f"Perfilando {binary} com perf record (call-graph dwarf)")
    run(["perf", "record", "-g", "--call-graph", "dwarf", "-o", "perf.data",
         "--", str(binary)], check=False)
    ok("perf.data escrito — veja com:  perf report -i perf.data")


TEST_TARGETS = ("image_tests", "image_job_tests", "thumbnail_blob_tests")


def run_tests(host: "bootstrap.Host", jobs: Optional[int]) -> None:
    if host.is_windows:
        die("o preset vs2026 configura só o appVulkanMedia "
            "(BUILD_VULKAN_MEDIA_ONLY): não há alvos de teste nesta árvore")
    log("Compilando + rodando os testes (" + ", ".join(TEST_TARGETS) + ")")
    cmd = ["cmake", "--build", str(host.build_dir), "--config", "Debug",
           "--target", *TEST_TARGETS]
    if jobs:
        cmd += ["-j", str(jobs)]
    run(cmd, env=bootstrap.cmake_env(host))
    run(["ctest", "--test-dir", str(host.build_dir), "-C", "Debug",
         "--output-on-failure"])
    ok("Testes passaram")


# ══════════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════════
def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog=Path(__file__).name,
        description="Port multiplataforma do build.sh (wrapper do preset do CMake).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Flags (combináveis):")[0].strip())
    p.add_argument("configs", nargs="*", metavar="CONFIG",
                   help="debug | release | release-log | all (padrão: all)")
    p.add_argument("--run", action="store_true", help="roda o app depois do build")
    p.add_argument("--perf", action="store_true", help="perfila com perf record (Linux)")
    p.add_argument("--test", action="store_true", help="compila e roda a suíte")
    p.add_argument("--fresh", action="store_true", help="reconfigura do zero")
    p.add_argument("--rebuild", action="store_true",
                   help="recompila o app preservando thirdparty e cache")
    p.add_argument("-j", "--jobs", type=int, metavar="N", help="paralelismo do build")
    p.add_argument("--dry-run", action="store_true", help="só mostra os comandos")
    return p.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    bootstrap.DRY_RUN = args.dry_run

    keywords: List[str] = []
    for value in args.configs:
        if value.lower() == "all":
            keywords = list(CONFIG_KEYWORDS)
        elif value.lower() in bootstrap.CONFIG_ALIASES:
            keywords.append(value.lower())
        else:
            die(f"argumento desconhecido '{value}' (tente --help)")
    if not keywords:
        keywords = list(CONFIG_KEYWORDS)         # padrão: compila tudo

    # dedup preservando a ordem pedida
    configs = list(dict.fromkeys(config_name(k) for k in keywords))

    host = bootstrap.detect_host()
    print(f"{BLUE}==>{OFF} host {DIM}{host.system}{OFF}, preset "
          f"{DIM}{host.preset}{OFF}, build dir "
          f"{DIM}build/{host.build_dir.name}{OFF}")

    if args.fresh and uses_ninja(host):
        reset_ninja_deps(host)
    configure(host, fresh=args.fresh)
    if uses_ninja(host) and ninja_deps_looks_stale(host):
        reset_ninja_deps(host)

    for config in configs:
        if args.rebuild:
            rebuild_clean_app(host, config)
        build_config(host, config, args.jobs)

    if args.test:
        run_tests(host, args.jobs)

    if args.perf:
        # Perfila o RelWithDebInfo quando ele está entre os compilados; senão o
        # primeiro que veio (com aviso dentro do run_perf).
        run_perf(host, "RelWithDebInfo" if "RelWithDebInfo" in configs else configs[0])

    if args.run:
        # Com vários configs compilados, o Debug é o padrão — igual ao build.sh.
        run_app(host, "Debug" if "Debug" in configs else configs[0])

    ok("Pronto.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print()
        raise SystemExit(130)
