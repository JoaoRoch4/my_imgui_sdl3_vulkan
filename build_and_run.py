#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_and_run.py — compila um config e já roda o app, com log e diagnóstico.

Um comando só para o ciclo de sempre: build.py <cfg> && roda o binário. A saída
do app vai para o terminal E para build/logs/, e quando ele morre (assert do
ImGui, segfault, validation layer do Vulkan) o script mostra as linhas que
interessam em vez de deixar o motivo enterrado no meio do log.

  python3 build_and_run.py                  Debug: compila e roda
  python3 build_and_run.py release          Release
  python3 build_and_run.py release-log      RelWithDebInfo
  python3 build_and_run.py --rebuild        recompila o app antes de rodar
  python3 build_and_run.py --no-build       só roda o que já está compilado
  python3 build_and_run.py --timeout 15     smoke test: mata o app após 15 s
  python3 build_and_run.py -- --flag        tudo depois de `--` vai para o app

Quem faz o quê, para não duplicar lógica:
  • build.py     — configure + pré-build da mídia + build (é chamado, não copiado).
  • bootstrap.py — detecção de host e o caminho real do executável (app_binary).
  • este script  — encadeia os dois, faz o tee do log e o post-mortem.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import List, Optional

REPO = Path(__file__).resolve().parent

try:
    sys.path.insert(0, str(REPO))
    import bootstrap  # noqa: E402  (vizinho no repo, não é dependência externa)
except ImportError as exc:                                    # pragma: no cover
    print(f"err  build_and_run.py precisa do bootstrap.py ao lado dele ({exc})",
          file=sys.stderr)
    raise SystemExit(1)

from bootstrap import DIM, OFF, RED, YELLOW, die, log, ok, warn  # noqa: E402

LOG_DIR = REPO / "build" / "logs"
LSAN_SUPPRESSIONS = REPO / "scripts" / "lsan.supp"

# Post-mortem: linhas que ajudam a explicar uma morte do app. Largo de
# propósito — só é usado quando o processo JÁ morreu, então ruído é barato.
CRASH_MARKERS = (
    "Assertion",
    "assert failed",
    "IM_ASSERT",
    "Segmentation fault",
    "terminate called",
    "std::bad_alloc",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "LeakSanitizer",
    "VUID-",
    "validation layer",
    "Vulkan error",
    "[vulkan_context] err",
    "err ",
    "error:",
    "failed",
)

# Sinais de que algo grave aconteceu MESMO com o processo saindo em 0 — um
# sanitizer que abortou uma thread, um assert de um processo filho. Aqui a
# lista é estreita: "failed" e afins aparecem em log normal (um fallback do
# yt-dlp, por exemplo) e não podem virar alarme falso.
FATAL_MARKERS = (
    "ERROR: AddressSanitizer",
    "runtime error:",           # UBSan
    "Assertion",
    "terminate called",
    "Segmentation fault",
)

# O LeakSanitizer é tratado à parte (ver scan_fatal): matar o app no --timeout
# deixa alocação de terceiro em voo, e o relatório oscila de execução para
# execução. Só é nosso problema quando alguma frame cai no nosso código — e
# cai sempre, já que só os objetos do app são instrumentados.
LSAN_HEADER = "ERROR: LeakSanitizer"


def config_name(keyword: str) -> str:
    """keyword do build.sh -> configuração do CMake (mesma tabela do build.py)."""
    try:
        return bootstrap.CONFIG_ALIASES[keyword.lower()]
    except KeyError:
        die(f"config desconhecido '{keyword}' (esperado: "
            + " | ".join(sorted(bootstrap.CONFIG_ALIASES)) + ")")


# ══════════════════════════════════════════════════════════════════════════════
# build
# ══════════════════════════════════════════════════════════════════════════════
def build(keyword: str, args: argparse.Namespace) -> None:
    """Delega ao build.py — ele é a fonte única do configure/pré-build/build."""
    script = REPO / "build.py"
    if not script.is_file():
        die("build.py não está ao lado do build_and_run.py")

    cmd: List[str] = [sys.executable, str(script), keyword]
    if args.rebuild:
        cmd.append("--rebuild")
    if args.fresh:
        cmd.append("--fresh")
    if args.jobs:
        cmd += ["-j", str(args.jobs)]

    log(f"Build de {keyword}")
    rc = subprocess.call(cmd, cwd=str(REPO))
    if rc != 0:
        die(f"build falhou (exit {rc}) — o app não foi executado", code=rc)


# ══════════════════════════════════════════════════════════════════════════════
# run (com tee para o log)
# ══════════════════════════════════════════════════════════════════════════════
def log_path(config: str) -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return LOG_DIR / f"{config}-{stamp}.log"


def link_last(path: Path) -> None:
    """build/logs/last.log aponta sempre para a execução mais recente."""
    last = LOG_DIR / "last.log"
    try:
        if last.is_symlink() or last.exists():
            last.unlink()
        last.symlink_to(path.name)
    except OSError:
        pass  # sem symlink (Windows sem privilégio): o log com timestamp basta


def sanitizer_env() -> dict:
    """ASan/UBSan defaults para o Debug (ENABLE_SANITIZERS=ON por padrão).

    Só preenche o que o ambiente ainda não definiu — quem exporta o seu próprio
    ASAN_OPTIONS continua no comando.

      • detect_container_overflow=0: só os objetos do app são instrumentados,
        a STL e os .a de thirdparty não; essa mistura gera falso positivo de
        container-overflow (CMakeLists.txt:69 já documenta isso).
      • suppressions: scripts/lsan.supp cala vazamentos de terceiros (libdbus
        via SDL) sem esconder os nossos.
    """
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_container_overflow=0")
    if LSAN_SUPPRESSIONS.is_file():
        env.setdefault("LSAN_OPTIONS",
                       f"suppressions={LSAN_SUPPRESSIONS}:print_suppressions=0")
    return env


def run_app(binary: Path, app_args: List[str], out: Path,
            timeout: Optional[float]) -> int:
    """Roda o app com stdout+stderr unidos, ecoando e gravando em `out`.

    O cwd é a raiz do repo: o app resolve assets, imgui.ini e build/cache a
    partir dela — rodar de outro diretório muda o que ele carrega.
    """
    log(f"Rodando {binary.relative_to(REPO)}"
        + (f" {' '.join(app_args)}" if app_args else ""))
    print(f"{DIM}   log: {out.relative_to(REPO)}{OFF}")

    proc = subprocess.Popen(
        [str(binary), *app_args],
        cwd=str(REPO),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,  # line buffered: o assert aparece na hora, não no fim
        env=sanitizer_env(),
    )

    def pump() -> None:
        assert proc.stdout is not None
        with out.open("w", encoding="utf-8", errors="replace") as fh:
            for line in proc.stdout:
                sys.stdout.write(line)
                fh.write(line)
            sys.stdout.flush()

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()

    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        # --timeout é o modo smoke test: subiu, desenhou, não morreu -> sucesso.
        warn(f"timeout de {timeout:g}s atingido — encerrando o app")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        reader.join(timeout=5)
        return 0
    except KeyboardInterrupt:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        reader.join(timeout=5)
        return 130

    reader.join(timeout=5)
    return proc.returncode


# ══════════════════════════════════════════════════════════════════════════════
# post-mortem
# ══════════════════════════════════════════════════════════════════════════════
def describe_exit(rc: int) -> str:
    """Traduz o código de saída — negativo do Popen = morreu por sinal."""
    if rc < 0:
        try:
            name = signal.Signals(-rc).name
        except ValueError:
            name = f"sinal {-rc}"
        return f"{name} ({rc})"
    if rc > 128:
        try:
            name = signal.Signals(rc - 128).name
        except ValueError:
            name = f"sinal {rc - 128}"
        return f"{name} (exit {rc})"
    return f"exit {rc}"


def read_log(out: Path) -> List[str]:
    try:
        return out.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def post_mortem(out: Path, rc: int, max_lines: int = 12) -> None:
    """Mostra as linhas do log que explicam a morte, do fim para o começo."""
    print(f"\n{RED}err{OFF} app terminou em {describe_exit(rc)}")
    lines = read_log(out)
    hits = [ln for ln in lines
            if any(marker in ln for marker in CRASH_MARKERS)]
    interesting = hits[-max_lines:] if hits else lines[-max_lines:]
    if not interesting:
        return

    print(f"{YELLOW}    últimas linhas relevantes:{OFF}")
    for line in interesting:
        print(f"    {line}")
    print(f"{DIM}    log completo: {out.relative_to(REPO)}{OFF}")


def scan_fatal(out: Path) -> "tuple[List[str], bool]":
    """Erros graves no log de uma execução que, ainda assim, saiu em 0.

    Acontece de verdade: o sanitizer aborta uma thread (ou um processo filho)
    e o processo principal segue até o fim. Sem esta varredura o script diria
    "saiu limpo" com um heap-use-after-free gravado no log.

    Devolve (linhas graves, houve_leak_de_terceiro).
    """
    lines = read_log(out)
    hits = [ln for ln in lines if any(m in ln for m in FATAL_MARKERS)]

    # Vazamento: só conta se alguma frame do relatório aponta para o nosso
    # código. Sem isso, todo smoke test terminaria com alarme por causa do
    # driver de vídeo / libdbus, que ficam com alocação em voo quando o
    # --timeout mata o processo no meio.
    ours = str(REPO / "code")
    third_party_leak = False
    for i, line in enumerate(lines):
        if LSAN_HEADER not in line:
            continue
        report = lines[i:]
        if any(ours in ln for ln in report):
            hits.append(line)
        else:
            third_party_leak = True
    return hits, third_party_leak


# ══════════════════════════════════════════════════════════════════════════════
# cli
# ══════════════════════════════════════════════════════════════════════════════
def parse_args(argv: List[str]) -> "tuple[argparse.Namespace, List[str]]":
    app_args: List[str] = []
    if "--" in argv:
        cut = argv.index("--")
        argv, app_args = argv[:cut], argv[cut + 1:]

    parser = argparse.ArgumentParser(
        prog="build_and_run.py",
        description="Compila um config e roda o app, com log e post-mortem.",
        epilog="Tudo após `--` é repassado ao app.",
    )
    parser.add_argument("config", nargs="?", default="debug",
                        help="debug | release | release-log (padrão: debug)")
    parser.add_argument("--rebuild", action="store_true",
                        help="recompila o app (preserva thirdparty e cache)")
    parser.add_argument("--fresh", action="store_true",
                        help="reconfigura do zero antes de compilar")
    parser.add_argument("-j", "--jobs", type=int, metavar="N",
                        help="paralelismo do build")
    parser.add_argument("--no-build", action="store_true",
                        help="pula o build e só roda o binário existente")
    parser.add_argument("--timeout", type=float, metavar="SEC",
                        help="mata o app após SEC segundos (smoke test)")
    return parser.parse_args(argv), app_args


def main(argv: List[str]) -> int:
    args, app_args = parse_args(argv)
    config = config_name(args.config)

    if not args.no_build:
        build(args.config, args)

    host = bootstrap.detect_host()
    binary = bootstrap.app_binary(host, config)
    if not binary.is_file():
        die(f"executável não encontrado: {binary} — compile antes "
            f"(python3 build.py {args.config})")

    out = log_path(config)
    rc = run_app(binary, app_args, out, args.timeout)
    link_last(out)

    if rc != 0:
        post_mortem(out, rc)
    else:
        fatal, third_party_leak = scan_fatal(out)
        if fatal:
            warn(f"app saiu em 0, mas o log tem erro grave "
                 f"({len(fatal)} linha(s)):")
            for line in fatal[:6]:
                print(f"    {line}")
            print(f"{DIM}    log completo: {out.relative_to(REPO)}{OFF}")
        else:
            ok(f"app saiu limpo — log em {out.relative_to(REPO)}")
            if third_party_leak:
                print(f"{DIM}   (o LeakSanitizer reclamou, mas nenhuma frame é "
                      f"nossa — driver/libdbus com alocação em voo){OFF}")
    # Um sinal vira 128+n, que é o que um shell espera ver.
    return rc if rc >= 0 else 128 - rc


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        print()
        raise SystemExit(130)
