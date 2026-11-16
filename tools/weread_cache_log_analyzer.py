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
    r"cache fetch index=(\d+) bytes=(\d+) elapsed_ms=(\d+) result=(\d+)"
)
IMAGE_RE = re.compile(
    r"cache image id=(\d+) bytes=(\d+) elapsed_ms=(\d+) result=(\d+)"
)
CHAPTER_RE = re.compile(
    r"cache chapter book=(\S+) index=(\d+)/(\d+) html_bytes=(\d+) elapsed_ms=(\d+) result=(\d+)"
)
TOC_RE = re.compile(r"cache toc book=(\S+) chapters=(\d+) elapsed_ms=(\d+)")
PREFETCH_WAIT_RE = re.compile(r"prefetch_wait_ms=(\d+)")


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
        },
        "toc": {"count": 0, "chapters": 0, "elapsed_ms": 0},
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
            _, byte_count, elapsed, result = match.groups()
            row = report["cache_fetch"]
            row["count"] += 1
            row["bytes"] += int(byte_count)
            row["elapsed_ms"] += int(elapsed)
            row["failures"] += int(result) == 0
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
            _, _, total, html_bytes, elapsed, result = match.groups()
            row = report["chapters"]
            row["count"] += 1
            row["html_bytes"] += int(html_bytes)
            row["elapsed_ms"] += int(elapsed)
            row["failures"] += int(result) == 0
            row["last_total"] = int(total)
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
        f"{report['chapters']['elapsed_ms']} ms"
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
