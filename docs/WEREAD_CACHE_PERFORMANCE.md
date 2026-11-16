# WeRead cache performance investigation — 2026-11-16

## Scope and baseline

This branch is based on the repository's current `main`, not on the separately reported 81.5 s working-tree snapshot. The original `main` baseline did not contain the reported chapter-TAR path or chapter prefetch. This branch now independently implements TAR image bundles and a bounded prefetch pipeline, but it is not the same source snapshot and has not reproduced the 81.5 s device measurement.

The experimental pipeline combines Reader/shard gzip, cache-only shard Keep-Alive with a circuit breaker, one-worker chapter+TAR prefetch, single-pass ZIP writing, image deduplication and partial-DU progress updates. It does not add concurrently active TLS sockets, change radio buffers or change panel waveforms.

**Device acceptance baseline:** use the same 32-chapter book (56 images, 16 chapter resource packs, 15.37 MB total resources). The last user-reported intermediate firmware measured 175.1 s end-to-end. Acceptance for this branch is a same-device, same-AP, same-book run below 175.1 s; the separate 81.5 s working-tree result is a stretch reference, not a verified baseline for this branch.

## Timing added

`main/book/weread_epub.c` now records:
- total chapter fetch time (reader + chapter content + optional chapter TAR request path, including prefetch wait);
- EPUB rewrite / ZIP entry time;
- time spent inside remote image requests;
- per-chapter fetch, transform, image, and wall time;
- a per-book summary.

Image time is reported separately because it is nested inside the EPUB transform interval. Do not add fetch + transform + image: image is a subset of transform. The metrics are intended to identify the true critical path before scheduling changes.

## Implemented on this experimental branch

- **Reader response gzip**: only the streaming Reader fields request advertises gzip. The response decoder detects the 1F 8B header, uses zlib inflate with an allocator directed to PSRAM, and emits at most 2 KiB per callback. Identity responses continue to work unchanged. Truncated gzip, corrupt gzip, and decompression beyond 8 MiB fail cleanly. Full-response image / JSON requests retain identity encoding.
- **Chapter shard gzip**: requests under `/web/book/chapter/` now advertise gzip, with a bounded PSRAM decompression pass after the complete response. The original identity path is used when the body has no gzip signature. The decoded size is capped by the existing response limit. This reduces wire bytes where the server supports gzip, but requires hardware benchmarking to establish net savings.
- **Successful-image deduplication**: during one EPUB build, an identical normalized source URL reuses its prior ZIP entry name without another HTTPS download. Exact equality avoids collisions; this is a bounded best-effort cache of at most 48 URLs, and all URL copies are released at book completion/cancellation. Chapter TAR assets are now matched by source basename first; individual CDN GET remains the fallback for missing or unsupported members.
- **Cache-only 5% redraw coalescing + partial DU**: avoids a full redraw for every chapter and reserves the final completion frame for the status transition. When progress crosses the threshold, only the fixed progress-bar/text rectangle is repainted and sent with `APP_REDRAW_AREA` using the existing `MODE_DU` dynamic waveform. Other WeRead job types keep their previous progress behavior.

Host tests run with sanitizers:
```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Imain/book main/book/weread_body_decode.c tools/weread_body_decode_host_test.c \
  -lz -o /tmp/weread-decode && /tmp/weread-decode
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Imain/apps tools/weread_progress_host_test.c \
  -o /tmp/weread-progress && /tmp/weread-progress
```

The S31 and Pico firmware builds passed again at commit `ec6e387`, including the extracted XHTML/TAR helpers, SVG `xlink:href`, malformed-tag sanitizer cases, and partial-DU progress-area refresh. The independent WeRead host suite also passed on that run. No physical-device speed benchmark has been run for this branch.

## Aggressive pipeline additions

- **Cache-only POST shard Keep-Alive:** the existing `esp_http_client_perform()` reusable-handle path now handles `/web/book/chapter/` POST shards. A non-200 response, transport failure, or invalid gzip trips a **per-job circuit breaker**, drops the handle, and falls back to the older `open/fetch/read` code for the same request. Auth failures/cancel are not blindly replayed. Counters track fast hits and fallbacks.
- **Bounded body+TAR prefetch:** a 16 KiB PSRAM-stack task fetches chapter i+1 and, only when its XHTML references WeRead-hosted images, fetches that chapter's TAR bundle too. It does both while the original worker rewrites chapter i. The request path is guarded with one mutex so only **one TLS operation** is active at a time. One ready bundle slot, 4 MiB free-PSRAM admission, half-free-PSRAM response budgets, automatic serial fallback, and an ownership-aware release callback bound memory and cleanup. This targets network/CPU/SD overlap; it is not multi-TLS transfer.
- **Deferred cookie persistence:** cache HTTP response cookies stay updated in RAM; NVS persistence is deferred until the internal-stack cache work task finishes, so the PSRAM-stack worker never writes NVS directly.
- **Single-pass stored ZIP entries:** chapter XHTML, OPF, and nav staging files are appended to EPUB as stored ZIP members through a bounded 8 KiB scratch buffer with a signed data descriptor. The file is read once for CRC+write; no full-file PSRAM copy is needed. EPUB mimetype remains the original first stored entry.
- **TAR image bundling:** only chapters whose XHTML contains a `res.weread.qq.com/wrepub/` image URL request the chapter TAR from the `/wrco/` endpoint. The request sends the current WeRead session Cookie and chapter Reader Referer; the Cookie header is removed before following a signed CDN/COS redirect. Redirect destinations are validated against the existing HTTPS host allowlist. The POSIX TAR parser validates header checksums, bounds, duplicate basenames and truncated tails. Unsupported/missing entries fall back to the existing individual-image GET. The XHTML probe recognizes both `img src` and SVG `image xlink:href`, and is host-tested against truncated tags.
- **Independent test job:** `weread-cache-host-tests` is isolated from unrelated existing repository host/board test failures. ZIP entries are cross-checked with Python `zipfile`; TAR lookup/checksum and nested prefetch-result cleanup are tested with ASan/UBSan and a pthread host shim.

### Known verification constraints

- Existing unrelated `display-host-tests` fails before new tests because the repository license manifest does not list `licenses/LICENSE-CrossMux.txt`.
- Existing `board-reader-tests` fails at `txt_navigation_host_test.py` due to unresolved `book_index_cache_prepare_file` and `book_epub_cached_chapter_count` host symbols.
- S31 and Pico firmware builds passed at `ec6e387`, which includes the latest XHTML-probe extraction/SVG and partial-DU progress changes. Physical-device validation and same-book timing comparison remain outstanding.
- Existing unrelated host/board test failures do not establish a WeRead regression, but a passing WeRead host job alone does not establish firmware compatibility.
- The earlier 81.5-second result remains an external measurement. This branch has not reproduced it; no new wall-clock claim should be made without same-book device runs.

## Open-source optimization leads

### 1. ESP-IDF / lwIP throughput profile

Espressif's ESP32-S3 Wi-Fi performance guide documents an iperf-oriented profile with larger RX/TX pools, RX BA window, and TCP send/receive windows. It reports substantially higher *laboratory iperf* throughput, but the profile consumes much more internal memory and was measured with one stream in a shielded RF environment. It is not a drop-in recommendation for this S31 reader workload.

Reference:
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/lwip.html

Relevant documented levers:
- `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and `CONFIG_LWIP_TCP_WND_DEFAULT`;
- Wi-Fi dynamic RX/TX buffer counts and RX BA window;
- `CONFIG_ESP_WIFI_IRAM_OPT`, `CONFIG_ESP_WIFI_RX_IRAM_OPT`, `CONFIG_LWIP_IRAM_OPTIMIZATION`;
- lwIP task affinity / priority when application tasks compete with TCP/IP;
- `TCP_NODELAY` for latency-sensitive small writes.

The documented iperf profile lists up to 64 KB TCP send/window values and 64 dynamic RX/TX buffers, but the repo's S31 profile has different memory constraints and a tested custom radio policy. Do not import the iperf profile wholesale. Any candidate must be measured with cache workload and internal-largest-block logging.

### 2. Reuse the ESP-IDF HTTP client handle

ESP-IDF's HTTP client state-machine documentation explicitly describes Keep-Alive connection reuse and avoiding a new TCP/TLS handshake for subsequent requests:
- https://github.com/espressif/esp-idf/blob/master/components/esp_http_client/docs/state_machine.md

The current implementation should retain the existing request lifecycle and retry semantics. Do not retry a streamed body after partial delivery unless the consumer can reset its parser and discard already-consumed bytes.

### 3. CPU scheduling and latency

ESP-IDF lwIP guidance recommends changing one parameter at a time, considering TCP/IP task affinity when application tasks compete for CPU, and enabling IRAM optimization only when sufficient IRAM exists. These are experiments, not guaranteed wins for this board.

## Experiments to run after timing baseline

1. Capture at least three runs of the same book and network/AP, reporting median and range.
2. Separate fetch time, image request time, EPUB transformation time, SD write/flush time, and panel blocking time.
3. Validate the combined body+TAR prefetch against the serial path on the same book and AP; retain one active TLS request and a single ready bundle slot.
4. Only if TCP receive stalls dominate: test one lwIP/Wi-Fi parameter at a time, recording internal free heap and largest contiguous DMA block before Wi-Fi init and during download.
5. Measure the new partial-DU progress-area refresh on the S31: compare cache wall time, refresh latency, ghosting and UNDERRUN counters; retain a full completion-frame refresh.
6. Keep a byte-identical output/EPUB validation test alongside speed measurements.

## Device benchmark log analysis

After capturing a serial log from the same 32-chapter / image-heavy test book, summarize it with:

```sh
python3 tools/weread_perf_log_summary.py run-before.log run-after.log
python3 tools/weread_perf_log_summary.py --json run-after.log
```

Compare the per-book `fetch_ms`, `transform_ms`, `image_ms`, and `chapter_wall_ms` fields; TAR bytes/latency and TAR-hit versus individual-fallback counts; prefetch bundle hits/wait; and shard Keep-Alive hits/fallbacks. Image time is nested inside transform time and must not be added to it.

## Acceptance gates

- No change to default radio settings, TLS concurrency, or CDN allowlist in this patch.
- Same chapter count, chapter order, image count, and valid EPUB output.
- No increased failed-cache rate over repeated runs.
- No watchdog/UNDERRUN regressions.
- Performance claims require device runs; host tests or iperf alone do not establish book-cache speedup.
