#!/usr/bin/env python3
"""Fallback video downloader for the SDL3/Vulkan media app.

Invoked by VideoDownloader (C++) as the *second* attempt, after the system
`yt-dlp` binary fails and before the dumb curl last-resort. It prefers the
**vendored** yt-dlp checkout under ``external/yt-dlp`` (which is newer than the
distro's system binary, so it may carry extractor fixes the binary lacks) and
disables yt-dlp's unsafe-extension guard -- the same effect as the CLI's
``--compat-options allow-unsafe-ext`` -- so CDNs that stream video through a
script path (e.g. ``.../remote_control.php`` -> yt-dlp derives ext='php' and
refuses) download normally. A remux to mp4 lands the file at the requested
``.mp4`` path regardless of the source container.

Usage:
    video_download.py <url> <output.mp4> <format-selector>

Exit codes:
    0  success: a non-empty file exists at <output.mp4>
    1  yt-dlp ran but produced no usable file
    2  bad arguments
    3  yt-dlp could not be imported
"""

import os
import sys


def _repo_root():
    # <repo>/scripts/video_download.py  ->  <repo>
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _disable_unsafe_extension_guard():
    """Neutralise yt-dlp's unsafe-extension mitigation (GHSA-79w7-vh3h-8g4j).

    Handles both flavours seen across versions: the newer ``_enabled`` flag
    (2026.06.09) and the older approach of replacing ``sanitize_extension``
    (2026.03.17). Either alone is enough; we set both defensively.
    """
    try:
        from yt_dlp.utils._utils import _UnsafeExtensionError
    except Exception:
        return
    try:
        _UnsafeExtensionError._enabled = False
    except Exception:
        pass
    try:
        _UnsafeExtensionError.sanitize_extension = staticmethod(
            lambda ext, *args, **kwargs: ext)
    except Exception:
        pass


def _progress_hook(d):
    """Print progress in the same shape as the yt-dlp CLI.

    The C++ side (VideoDownloader::parse_progress_line) parses
    ``[download] <pct>% of <size>`` lines from this process's output, so this
    fallback reports progress exactly like attempt 1 does.
    """
    if d.get("status") != "downloading":
        return
    total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
    done = d.get("downloaded_bytes") or 0
    if not total:
        return
    pct = 100.0 * done / total
    print(f"[download] {pct:.1f}% of {total / (1024 * 1024):.2f}MiB",
          file=sys.stderr, flush=True)


def main(argv):
    if len(argv) < 3:
        print("usage: video_download.py <url> <output> <format>", file=sys.stderr)
        return 2
    url, out, fmt = argv[0], argv[1], argv[2]

    # Prefer the vendored checkout over any pip-installed copy.
    vendored = os.path.join(_repo_root(), "external", "yt-dlp")
    if os.path.isdir(os.path.join(vendored, "yt_dlp")):
        sys.path.insert(0, vendored)

    try:
        import yt_dlp
    except Exception as exc:  # noqa: BLE001 - report any import failure verbatim
        print(f"[py-fallback] cannot import yt_dlp: {exc}", file=sys.stderr)
        return 3

    _disable_unsafe_extension_guard()

    # Clear a stale 0-byte remnant so success can be judged by file presence.
    try:
        os.remove(out)
    except FileNotFoundError:
        pass

    print(f"[py-fallback] using yt_dlp from: {os.path.dirname(yt_dlp.__file__)}",
          file=sys.stderr)

    # Use %(ext)s rather than a literal .mp4 in the template: a fixed extension
    # collides with the remuxer (it would append a second .mp4 -> "x.mp4.mp4")
    # and with the source's real ext. Let yt-dlp choose the name, then move the
    # produced file to the canonical <out> the C++ side polls for.
    base = os.path.splitext(out)[0]
    opts = {
        "outtmpl": base + ".%(ext)s",
        "format": fmt,
        "merge_output_format": "mp4",
        "noplaylist": True,
        "no_warnings": True,
        "quiet": True,
        "progress_hooks": [_progress_hook],
        # Remux whatever container we got (e.g. the 'php'-typed fmp4) to mp4.
        "postprocessors": [
            {"key": "FFmpegVideoRemuxer", "preferedformat": "mp4"},
        ],
    }

    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(url, download=True)
    except Exception as exc:  # noqa: BLE001 - any yt-dlp error = this attempt failed
        print(f"[py-fallback] download raised: {exc}", file=sys.stderr)
        return 1

    produced = None
    for rd in (info or {}).get("requested_downloads") or []:
        path = rd.get("filepath") or rd.get("_filename")
        if path and os.path.isfile(path):
            produced = path
            break

    if produced and os.path.abspath(produced) != os.path.abspath(out):
        os.replace(produced, out)

    ok = os.path.isfile(out) and os.path.getsize(out) > 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
