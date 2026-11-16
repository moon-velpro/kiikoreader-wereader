# WeRead cache performance: next-stage research

Date: 2026-11-16
Base: `fix/weread-reliability-2026-11-16` (69 commits ahead of `main`)

## Scope and baseline facts

The 81.5 s result described in the local performance report is **not present in the public repository snapshot**. The repository's `main/book/weread_epub.c` still fetches chapters through the Web reader path and resolves images with individual HTTP requests; it does not contain `WR_PREFETCH_CHAPTER`, `WR_IMG_TAR_LIMIT`, or `WR_PROGRESS_AREA`. Do not claim a repository build reproduces 81.5 s until those exact changes are committed and built.

The reliability branch already records a previous dual-task prefetch experiment that was removed: about 66 s per prefetched chapter, repeated IDLE watchdog alerts, and extra SD temporary-file write + fsync + readback. Any new prefetch work must therefore avoid per-chapter staging-file round trips and must be bounded to one network worker unless measurements prove otherwise.

The local 81.5 s report remains the performance target/measurement reference, not evidence that this branch already contains that implementation.

## Open-source approaches reviewed

### 1. ESP-IDF HTTP client async state machine and keep-alive

Upstream references:

- https://github.com/espressif/esp-idf/blob/master/components/esp_http_client/docs/state_machine.md
- https://github.com/espressif/esp-idf/blob/master/components/esp_http_client/include/esp_http_client.h

ESP-IDF's client supports async/non-blocking operation and can reuse a connection when the server permits keep-alive. Reuse must remain serialized per handle: the upstream API explicitly says not to call the same handle simultaneously. This is a scheduling primitive, not a promise of higher raw throughput. For S31, compare a single-owner async state machine with the current synchronous open/fetch/read path before considering multiple live TLS handles.

### 2. ESP-IDF lwIP throughput profile

Upstream reference:

- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/lwip.html

Useful variables include TCP receive window/mailbox sizing, IRAM optimization, and lwIP task affinity/priority. Espressif recommends changing a few settings at a time and benchmarking the real workload. The existing S31 RX64/TX3 profile and measured negative AMPDU result are hard constraints; do not import the iPerf maximum-throughput profile wholesale.

### 3. ESP-IDF storage I/O

Upstream reference:

- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/speed.html

The v6.1 guidance suggests measuring POSIX `read/write` against stdio `fread/fwrite`, and testing larger stdio buffers. This is a candidate only if the actual SD/VFS path is benchmarked. Avoid adding a second copy or an fsync/readback stage to the hot path.

### 4. MiuRead E-Ink chapter archive protocol

Open-source reference (AGPL-3.0-only):

- https://github.com/miumiupy98-art/miuread-koreader
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/eink_client.lua
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/eink_archive.lua
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/eink_crypto.lua

The E-Ink path uses `/book/chapterInfos` and `/book/chapterdownload`, downloads an encrypted ZIP, obtains the `EncryptKey` response header, derives the archive password from the authorized VID, and validates ZIP entries/CRC. Its EPUB download request includes shared chapter `0` plus the selected chapter on the first archive for a book/version. This may combine body, CSS and images into fewer service requests than the Web reader + shard + image path.

Important limitations:

- The open-source implementation downloads one selected chapter plus shared chapter 0; it does **not** prove arbitrary multi-chapter batching works.
- It requires E-Ink client authorization and an additional encrypted-ZIP/AES/deflate implementation. Existing Web cookies alone must not be assumed sufficient.
- The catalog is checked for free/already-paid entitlement and version consistency. Keep those checks; do not trigger purchase flows.
- Benchmark on the same book/account/network before adopting it. Measure archive bytes, download time, decrypt/inflate time, image completeness, and server errors.
- MiuRead is AGPL-3.0-only. Use it as a protocol/architecture reference; do not copy its implementation into this differently licensed firmware without an explicit license review.

## Ranked experiments

1. **Instrumentation first**: collect per-request type, bytes, elapsed time, connection reuse, chapter fetch/parse/package time, SD write time, progress-refresh blocking time, and actual prefetch wait time. Keep logs free of cookies, signed query strings, and book content.
2. **Single-owner async HTTP**: test one task owning the HTTP handle and advancing non-blocking state; keep a bounded request queue. No concurrent use of a handle.
3. **Streaming storage benchmark**: compare current stdio path against POSIX I/O on the actual SD card and mount. Only adopt if the complete EPUB benchmark improves and watchdog headroom remains.
4. **E-Ink archive spike**: add as an optional transport behind explicit authorization; compare against the Web path. Keep Web fallback for unsupported/unpaid chapters and authorization/version mismatch.
5. **Bounded prefetch**: only after stage timings show idle network windows. Use one network worker, no per-chapter temp-file/fsync/readback, and a small queue. Do not repeat the old two-task staging implementation.
6. **DU progress rendering**: enable only after panel timing shows it reduces wall-clock blocking and does not cause underrun or completion-frame loss.

## Acceptance rules

- Same book, same account, same AP, same firmware/toolchain, same cache destination.
- Run at least 5 cold-cache repetitions per variant; report median, min/max, bytes, chapter/image completeness, and failure count.
- A faster result is rejected if it loses an image/chapter, trips IDLE watchdog, reduces stability, or requires unbounded TLS/PSRAM use.
- Keep current known-good network/radio settings unchanged during each isolated experiment.
