#!/usr/bin/env python3
"""Summarize WeRead cache timing logs without exposing request credentials.

Usage:
  python tools/weread_cache_log_analyzer.py serial.log
  python tools/weread_cache_log_analyzer.py --json serial.log
  type serial.log | python tools/weread_cache_log_analyzer.py -
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from urllib.parse import urlsplit

HTTP_RE = re.compile(
    r"HTTP complete url=(\S+) bytes=(\d+) elapsed_ms=(\d+) connection_reused=(\d+)"
)
WORK_RE = re.compile(
    r"work done kind=(\d+) ok=(\d+) canceled=(\d+) elapsed_ms=(\d+)"
)
FETCH_RE = re.compile(
    r"cache fetch index=(\d+) bytes=(\d+) elapsed_ms=(\d+)"
    r"(?: reader_ms=(\d+) content_ms=(\d+) attempts=(\d+))? result=(\d+)"
)
FETCH_FAIL_RE = re.compile(
    r"cache fetch failed index=(\d+) chapter=\S+ elapsed_ms=(\d+)"
    r" reader_ms=(\d+) content_ms=(\d+) attempts=(\d+) error="
)
IMAGE_RE = re.compile(
    r"cache image id=(\d+) bytes=(\d+) elapsed_ms=(\d+) result=(\d+)"
)
CHAPTER_RE = re.compile(
    r"cache chapter book=(\S+) index=(\d+)/(\d+) html_bytes=(\d+)"
    r"(?: fetch_ms=(\d+) package_ms=(\d+))? elapsed_ms=(\d+) result=(\d+)"
)
METRICS_RE = re.compile(
    r"cache metrics chapters=(\d+) image_requests=(\d+) image_ok=(\d+) image_failed=(\d+)"
    r" image_unsupported=(\d+) image_bytes=(\d+) reader_ms=(\d+) content_ms=(\d+)"
    r" chapter_fetch_ms=(\d+) chapter_package_ms=(\d+) image_fetch_ms=(\d+)"
    r" image_zip_ms=(\d+) zip_crc_ms=(\d+) zip_write_ms=(\d+) file_read_ms=(\d+) total_ms=(\d+)"
)
TOC_RE = re.compile(r"cache toc book=(\S+) chapters=(\d+) elapsed_ms=(\d+)")
PREFETCH_WAIT_RE = re.compile(r"prefetch_wait_ms=(\d+)")
STORE_RE = re.compile(
    r"cache store metrics flush_ms=(\d+) fsync_ms=(\d+) close_ms=(\d+)"
    r" validate_ms=(\d+) rename_ms=(\d+) total_ms=(\d+) result=(\d+)"
)


def classify_url(url: str) -> str:
    parsed = urlsplit(url)
    host = (parsed.hostname or "").lower()
    path = parsed.path.lower()
    query = parsed.query.lower()
    combined = path + "?" + query
    if "chapterdownload" in path:
        return "eink_archive"
    if "chapterinfos" in path:
        return "toc"
    if ".tar" in combined or "tar" in path:
        return "tar"
    if any(token in combined for token in ("e_0", "e_1", "e_3", "/chapter/t_")):
        return "shard"
    if "reader" in path or "/web/book/read" in path:
        return "reader_or_read_api"
    if host.startswith("res.") or any(
        path.endswith(ext) for ext in (".jpg", ".jpeg", ".png", ".webp", ".gif")
    ):
        return "image_or_resource"
    return "other"


def parse_lines(lines: list[str]) -> dict:
    groups = defaultdict(
        lambda: {"requests": 0, "bytes": 0, "elapsed_ms": 0, "reused": 0}
    )
    report = {
        "http_requests": 0,
        "cache_fetch": {"count": 0, "bytes": 0, "elapsed_ms": 0, "failures": 0},
        "images": {"count": 0, "bytes": 0, "elapsed_ms": 0, "failures": 0},
        "chapters": {
            "count": 0,
            "html_bytes": 0,
            "elapsed_ms": 0,
            "failures": 0,
            "last_total": 0,
            "fetch_ms": 0,
            "package_ms": 0,
        },
        "toc": {"count": 0, "chapters": 0, "elapsed_ms": 0},
        "cache_metrics": None,
        "store_metrics": None,
        "prefetch_wait_ms": 0,
        "work": [],
    }

    for line in lines:
        match = HTTP_RE.search(line)
        if match:
            url, byte_count, elapsed, reused = match.groups()
            row = groups[classify_url(url)]
            row["requests"] += 1
            row["bytes"] += int(byte_count)
            row["elapsed_ms"] += int(elapsed)
            row["reused"] += int(reused)
            report["http_requests"] += 1
            continue

        match = WORK_RE.search(line)
        if match:
            kind, ok, canceled, elapsed = map(int, match.groups())
            report["work"].append(
                {
                    "kind": kind,
                    "ok": bool(ok),
                    "canceled": bool(canceled),
                    "elapsed_ms": elapsed,
                }
            )
            continue

        match = FETCH_RE.search(line)
        if match:
            _, byte_count, elapsed, _, _, _, result = match.groups()
            row = report["cache_fetch"]
            row["count"] += 1
            row["bytes"] += int(byte_count)
            row["elapsed_ms"] += int(elapsed)
            row["failures"] += int(result) == 0
            continue

        match = FETCH_FAIL_RE.search(line)
        if match:
            _, elapsed, _, _, _ = match.groups()
            row = report["cache_fetch"]
            row["count"] += 1
            row["elapsed_ms"] += int(elapsed)
            row["failures"] += 1
            continue

        match = IMAGE_RE.search(line)
        if match:
            _, byte_count, elapsed, result = match.groups()
            row = report["images"]
            row["count"] += 1
            row["bytes"] += int(byte_count)
            row["elapsed_ms"] += int(elapsed)
            row["failures"] += int(result) == 0
            continue

        match = CHAPTER_RE.search(line)
        if match:
            _, _, total, html_bytes, fetch_ms, package_ms, elapsed, result = match.groups()
            row = report["chapters"]
            row["count"] += 1
            row["html_bytes"] += int(html_bytes)
            row["elapsed_ms"] += int(elapsed)
            row["fetch_ms"] += int(fetch_ms or 0)
            row["package_ms"] += int(package_ms or 0)
            row["failures"] += int(result) == 0
            row["last_total"] = int(total)
            continue

        match = METRICS_RE.search(line)
        if match:
            names = (
                "chapters", "image_requests", "image_ok", "image_failed",
                "image_unsupported", "image_bytes", "reader_ms", "content_ms",
                "chapter_fetch_ms", "chapter_package_ms", "image_fetch_ms",
                "image_zip_ms", "zip_crc_ms", "zip_write_ms", "file_read_ms", "total_ms",
            )
            report["cache_metrics"] = dict(zip(names, map(int, match.groups())))
            continue

        match = STORE_RE.search(line)
        if match:
            names = ("flush_ms", "fsync_ms", "close_ms", "validate_ms", "rename_ms", "total_ms")
            values = list(map(int, match.groups()))
            report["store_metrics"] = dict(zip(names, values[:6]))
            report["store_metrics"]["result"] = bool(values[6])
            continue

        match = TOC_RE.search(line)
        if match:
            _, chapters, elapsed = match.groups()
            row = report["toc"]
            row["count"] += 1
            row["chapters"] = max(row["chapters"], int(chapters))
            row["elapsed_ms"] += int(elapsed)
            continue

        match = PREFETCH_WAIT_RE.search(line)
        if match:
            report["prefetch_wait_ms"] += int(match.group(1))

    report["http_by_type"] = dict(sorted(groups.items()))
    http_bytes = sum(row["bytes"] for row in groups.values())
    http_ms = sum(row["elapsed_ms"] for row in groups.values())
    report["http_total"] = {
        "requests": report["http_requests"],
        "bytes": http_bytes,
        "elapsed_ms": http_ms,
        "average_kib_per_s": (
            round(http_bytes / 1024 / (http_ms / 1000), 2) if http_ms else 0.0
        ),
    }
    report["cache_work_ms"] = max(
        (row["elapsed_ms"] for row in report["work"] if row["kind"] == 2),
        default=0,
    )
    # Coarse indicator only: request durations may include non-overlapping phases.
    report["request_time_minus_work_ms"] = (
        max(0, http_ms - report["cache_work_ms"])
        if report["cache_work_ms"]
        else None
    )
    return report


def render_text(report: dict) -> str:
    lines = [f"Cache work: {report['cache_work_ms']} ms"]
    total = report["http_total"]
    lines.append(
        f"HTTP: {total['requests']} requests, {total['bytes']} bytes, "
        f"{total['elapsed_ms']} ms summed, {total['average_kib_per_s']} KiB/s"
    )
    lines.append("HTTP by type:")
    for name, row in report["http_by_type"].items():
        rate = (
            row["bytes"] / 1024 / (row["elapsed_ms"] / 1000)
            if row["elapsed_ms"]
            else 0.0
        )
        lines.append(
            f"  {name}: {row['requests']} req, {row['bytes']} B, "
            f"{row['elapsed_ms']} ms, {rate:.2f} KiB/s, reused={row['reused']}"
        )
    lines.append(
        f"Chapter fetches: {report['cache_fetch']['count']} "
        f"({report['cache_fetch']['failures']} failed), "
        f"{report['cache_fetch']['elapsed_ms']} ms"
    )
    lines.append(
        f"Images: {report['images']['count']} "
        f"({report['images']['failures']} failed), "
        f"{report['images']['bytes']} B, {report['images']['elapsed_ms']} ms"
    )
    lines.append(
        f"Chapters packaged: {report['chapters']['count']} "
        f"({report['chapters']['failures']} failed), "
        f"{report['chapters']['elapsed_ms']} ms "
        f"(fetch={report['chapters']['fetch_ms']} ms, package={report['chapters']['package_ms']} ms)"
    )
    if report["cache_metrics"] is not None:
        m = report["cache_metrics"]
        lines.append(
            f"Package stages: reader={m['reader_ms']} ms, content={m['content_ms']} ms, "
            f"image-fetch={m['image_fetch_ms']} ms, image-zip={m['image_zip_ms']} ms, "
            f"zip-crc={m['zip_crc_ms']} ms, zip-write={m['zip_write_ms']} ms, "
            f"file-read={m['file_read_ms']} ms"
        )
        lines.append(
            f"Image summary: {m['image_ok']} OK / {m['image_failed']} failed / "
            f"{m['image_unsupported']} unsupported, {m['image_bytes']} bytes"
        )
    if report["store_metrics"] is not None:
        m = report["store_metrics"]
        lines.append(
            f"SD/store: flush={m['flush_ms']} ms, fsync={m['fsync_ms']} ms, "
            f"close={m['close_ms']} ms, validate={m['validate_ms']} ms, "
            f"rename={m['rename_ms']} ms, total={m['total_ms']} ms, ok={m['result']}"
        )
    lines.append(f"Measured prefetch wait: {report['prefetch_wait_ms']} ms")
    if report["request_time_minus_work_ms"] is not None:
        lines.append(
            "Request-time minus work-time (coarse overlap indicator, not proof): "
            f"{report['request_time_minus_work_ms']} ms"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", default="-", help="log file, or - for stdin")
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args()
    try:
        content = (
            sys.stdin.read()
            if args.log == "-"
            else Path(args.log).read_text(encoding="utf-8", errors="replace")
        )
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report = parse_lines(content.splitlines())
    print(
        json.dumps(report, ensure_ascii=False, indent=2)
        if args.json
        else render_text(report)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
