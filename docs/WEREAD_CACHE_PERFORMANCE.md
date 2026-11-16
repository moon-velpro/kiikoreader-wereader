# WeRead cache performance investigation — 2026-11-16

## Scope and baseline

This branch is based on the repository's current `main`, not on the separately reported 81.5 s working-tree snapshot. The current repository source does not contain the reported chapter-TAR path, HTTP host pool, or chapter prefetch implementation. Do not interpret the 81.5 s measurements as measurements of this branch.

This PR builds an experimental high-performance pipeline on top of the older sequential cache source: Reader/shard gzip, shard Keep-Alive, bounded one-worker chapter prefetch, single-pass ZIP writing, image deduplication and progress redraw coalescing. It does not add concurrently active TLS sockets, change radio buffers or change panel waveforms. These changes are **not** a port of the missing 81.5-second TAR pipeline.

## Timing added

`main/book/weread_epub.c` now records:
- total chapter fetch time (reader + chapter content request path);
- EPUB rewrite / ZIP entry time;
- time spent inside remote image requests;
- per-chapter fetch, transform, image, and wall time;
- a per-book summary.

Image time is reported separately because it is nested inside the EPUB transform interval. Do not add fetch + transform + image: image is a subset of transform. The metrics are intended to identify the true critical path before scheduling changes.

## Implemented on this experimental branch

- **Reader response gzip**: only the streaming Reader fields request advertises gzip. The response decoder detects the 1F 8B header, uses zlib inflate with an allocator directed to PSRAM, and emits at most 2 KiB per callback. Identity responses continue to work unchanged. Truncated gzip, corrupt gzip, and decompression beyond 8 MiB fail cleanly. Full-response image / JSON requests retain identity encoding.
- **Chapter shard gzip**: requests under `/web/book/chapter/` now advertise gzip, with a bounded PSRAM decompression pass after the complete response. The original identity path is used when the body has no gzip signature. The decoded size is capped by the existing response limit. This reduces wire bytes where the server supports gzip, but requires hardware benchmarking to establish net savings.
- **Successful-image deduplication**: during one EPUB build, an identical normalized source URL reuses its prior ZIP entry name without another HTTPS download. Exact equality avoids collisions; this is a bounded best-effort cache of at most 48 URLs, and all URL copies are released at book completion/cancellation. It does not solve relative TAR paths or change CDN restrictions.
- **Cache-only 5% redraw coalescing**: avoids a full redraw for every chapter and reserves the final completion frame for the status transition. Other WeRead job types still redraw on progress changes. This does **not** yet turn on partial DU.

Host tests run with sanitizers:
```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Imain/book main/book/weread_body_decode.c tools/weread_body_decode_host_test.c \
  -lz -o /tmp/weread-decode && /tmp/weread-decode
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Imain/apps tools/weread_progress_host_test.c \
  -o /tmp/weread-progress && /tmp/weread-progress
```

No S31 firmware build or physical hardware benchmark is reported as passing at the time of writing.

## Aggressive pipeline additions

- **Cache-only POST shard Keep-Alive:** the existing `esp_http_client_perform()` reusable-handle path now handles `/web/book/chapter/` POST shards. A non-200 response, transport failure, or invalid gzip trips a **per-job circuit breaker**, drops the handle, and falls back to the older `open/fetch/read` code for the same request. Auth failures/cancel are not blindly replayed. Counters track fast hits and fallbacks.
- **Bounded one-worker prefetch:** after downloading chapter i, a PSRAM-stack task begins fetching chapter i+1 while the original worker converts/writes chapter i. The request path is guarded with one mutex so only **one TLS operation** is active at a time. One ready chapter slot, 6 MiB free-PSRAM minimum, automatic serial fallback, and explicit task shutdown are required. This targets CPU/network/SD overlap; it is not multi-TLS transfer.
- **Deferred cookie persistence:** cache HTTP response cookies stay updated in RAM; NVS persistence is deferred until the internal-stack cache work task finishes, so the PSRAM-stack worker never writes NVS directly.
- **Single-pass stored ZIP entries:** chapter XHTML, OPF, and nav staging files are appended to EPUB as stored ZIP members through a bounded 8 KiB scratch buffer with a signed data descriptor. The file is read once for CRC+write; no full-file PSRAM copy is needed. EPUB mimetype remains the original first stored entry.
- **Independent test job:** `weread-cache-host-tests` is isolated from unrelated existing repository host/board test failures. ZIP entries are cross-checked with Python `zipfile`; FreeRTOS task sequencing is tested with a pthread host shim and ASan/UBSan.

### Known verification constraints

- Existing unrelated `display-host-tests` fails before new tests because the repository license manifest does not list `licenses/LICENSE-CrossMux.txt`.
- Existing `board-reader-tests` fails at `txt_navigation_host_test.py` due to unresolved `book_index_cache_prepare_file` and `book_epub_cached_chapter_count` host symbols.
- The S31 full firmware build and physical device validation must be reviewed independently. These unrelated job failures do not establish a WeRead regression, but a passing host job alone does not establish firmware compatibility.
- The active branch is **not** the previously reported 81.5-second TAR/prefetch source. No new wall-clock claims should be made without same-book device runs.

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
3. Only if fetch waits dominate: prototype bounded prefetch with one network worker and a queue depth of at most two; do not add concurrent TLS sockets initially.
4. Only if TCP receive stalls dominate: test one lwIP/Wi-Fi parameter at a time, recording internal free heap and largest contiguous DMA block before Wi-Fi init and during download.
5. Only if panel blocking appears on the critical path: evaluate partial DU refresh with completion-frame arbitration and UNDERRUN monitoring.
6. Keep a byte-identical output/EPUB validation test alongside speed measurements.

## Acceptance gates

- No change to default radio settings, TLS concurrency, or CDN allowlist in this patch.
- Same chapter count, chapter order, image count, and valid EPUB output.
- No increased failed-cache rate over repeated runs.
- No watchdog/UNDERRUN regressions.
- Performance claims require device runs; host tests or iperf alone do not establish book-cache speedup.
