#!/usr/bin/env python3
"""Summarize WeRead cache timing/resource counters from an ESP-IDF log."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

PREFIXES = {
    "profile": "cache profile ",
    "prefetch": "cache prefetch ",
    "images": "cache image reuse ",
    "tar": "cache chapter tar ",
    "keepalive": "cache shard keep-alive ",
    "chapter": "cache chapter book=",
    "work": "work done ",
}
NUMBER_FIELDS = {
    "chapters", "fetch_ms", "transform_ms", "image_ms", "chapter_wall_ms",
    "hits", "wait_ms", "enabled", "stored", "tar_images", "tar_fallbacks",
    "index", "bytes", "limit", "free_psram", "elapsed_ms", "ok",
    "fallbacks", "disabled", "html_bytes", "result", "kind", "canceled",
    "stack_hwm", "internal_free", "psram_free",
}


def parse_fields(line: str, prefix: str) -> dict:
    start = line.find(prefix)
    if start < 0:
        return {}
    tail = line[start + len(prefix):]
    fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", tail))
    for key in list(fields):
        if key in NUMBER_FIELDS:
            try:
                fields[key] = int(fields[key], 10)
            except ValueError:
                pass
    return fields


def summarize(path: Path) -> dict:
    result = {
        "file": str(path),
        "profiles": [],
        "prefetch": [],
        "images": [],
        "tar": [],
        "keepalive": [],
        "chapters": [],
        "work": [],
    }
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        for key, prefix in PREFIXES.items():
            if prefix not in line:
                continue
            fields = parse_fields(line, prefix)
            if fields:
                result[key].append(fields)
            break
    result["tar_summary"] = {
        "requests": len(result["tar"]),
        "successful_requests": sum(1 for x in result["tar"] if x.get("ok") == 1),
        "bytes": sum(x.get("bytes", 0) for x in result["tar"]),
        "elapsed_ms": sum(x.get("elapsed_ms", 0) for x in result["tar"]),
    }
    result["chapter_summary"] = {
        "count": len(result["chapters"]),
        "successful": sum(1 for x in result["chapters"] if x.get("result") == 1),
        "wall_ms_sum": sum(x.get("elapsed_ms", 0) for x in result["chapters"]),
    }
    return result


def print_report(report: dict) -> None:
    print(f"Log: {report['file']}")
    for p in report["profiles"]:
        print("Book: {book} | chapters={chapters} | fetch={fetch_ms} ms | "
              "transform={transform_ms} ms | image={image_ms} ms | "
              "chapter-wall={chapter_wall_ms} ms".format(**p))
    for p in report["prefetch"]:
        print(f"Prefetch: enabled={p.get('enabled')} | bundles={p.get('hits')} | "
              f"wait={p.get('wait_ms')} ms")
    for p in report["images"]:
        print(f"Images: tar_hits={p.get('tar_images')} | "
              f"individual_fallbacks={p.get('tar_fallbacks')} | "
              f"url_reuse_hits={p.get('hits')} | stored={p.get('stored')}")
    t = report["tar_summary"]
    print(f"TAR: requests={t['requests']} | successful={t['successful_requests']} | "
          f"bytes={t['bytes']} | elapsed={t['elapsed_ms']} ms")
    for p in report["keepalive"]:
        print(f"Shard Keep-Alive: hits={p.get('hits')} | fallbacks={p.get('fallbacks')} | "
              f"disabled={p.get('disabled')}")
    for p in report["work"]:
        print(f"End-to-end: ok={p.get('ok')} | canceled={p.get('canceled')} | "
              f"elapsed={p.get('elapsed_ms')} ms | stack_hwm={p.get('stack_hwm')} | "
              f"internal_free={p.get('internal_free')} | psram_free={p.get('psram_free')}")
    if report["chapters"]:
        slow = sorted(report["chapters"], key=lambda x: x.get("elapsed_ms", 0), reverse=True)[:5]
        print("Slowest chapters:")
        for p in slow:
            print(f"  chapter {p.get('index')} elapsed={p.get('elapsed_ms')} ms "
                  f"fetch={p.get('fetch_ms')} ms transform={p.get('transform_ms')} ms")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="ESP-IDF log files")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    reports = [summarize(path) for path in args.logs]
    if args.json:
        print(json.dumps(reports, ensure_ascii=False, indent=2))
    else:
        for i, report in enumerate(reports):
            if i:
                print()
            print_report(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
