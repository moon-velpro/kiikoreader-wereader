# WeRead cache: open-source implementation notes

Date: 2026-11-16
Target branch: `codex/weread-cache-next-r1-20261116`

## MiuRead conservative HTTP keep-alive

References:

- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/http.lua
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/downloader.lua
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/miuread.koplugin/miuread/config.lua
- https://github.com/miumiupy98-art/miuread-koreader/blob/main/CHANGELOG.md

MiuRead's implementation has useful guardrails for a constrained client:

- Opt-in per request, only on the chapter-download path; login, shelf, progress, reading-time and annotations keep the old one-connection-per-request behavior.
- It excludes large streamed-to-file transfers from pooling.
- Pool key is origin plus address-family policy; one idle socket per origin is sufficient because requests are sequential.
- Idle timeout is 25 seconds and a socket is retired after 64 reuses.
- A socket is reusable only after a completed, self-delimiting response (Content-Length, chunked transfer, 204 or 304), with no `Connection: close`; HTTP/1.0 needs explicit keep-alive.
- It checks an idle socket for readability before reuse, because a FIN or unread bytes means the peer has already closed or contaminated the socket.
- A stale reused connection is retried only for GET/HEAD when no response bytes were received. POST is never automatically replayed.
- Pool sockets are closed at download completion, cancellation, error, network change and rate-limit cooldown.

MiuRead's release notes report a Kindle Oasis 2 A/B run where a 36-chapter book went from about 962 s to 729 s and connection opens went from 272 to 79. This is a project-reported result on Kindle, not a prediction for S31. It supports measuring handshake removal, but not copying LuaSocket/LuaSec internals into ESP-IDF.

**S31 porting rule:** retain the guardrails, but use ESP-IDF's supported `esp_http_client` transport. Do not transplant LuaSocket's `create` hook, monkey-patched socket factory or TLS internals. Keep the pool serialized by owner and host, exclude large streaming downloads until proven safe, and never replay a POST after an ambiguous send.

## ESP-IDF async client

References:

- https://github.com/espressif/esp-idf/blob/master/components/esp_http_client/docs/state_machine.md
- https://github.com/espressif/esp-idf/blob/master/components/esp_http_client/include/esp_http_client.h

ESP-IDF exposes `is_async` for HTTPS. In async mode, `esp_http_client_perform()` may return `ESP_ERR_HTTP_EAGAIN`; the caller retries the same handle to resume the state machine. Keep-alive can return the client to the connected state after a complete response.

This makes a **single-owner, cooperative HTTP state machine** a plausible way to let the cache worker do bounded local work between network progress steps. It does not justify concurrent calls on one handle or opening multiple TLS handles. Prototype this only behind an experiment switch and compare it with the verified blocking path.

## lwIP lower-level APIs

Reference:

- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/lwip.html

ESP-IDF documents that lwIP Raw API is not supported. Netconn has lower resource use and can avoid an extra copy, but Espressif explicitly says it is enabled yet unsupported/untested. Do not replace `esp_http_client` with raw lwIP or Netconn for this project.

## E-Ink archive transport

See `docs/WEREAD_CACHE_NEXT_RESEARCH.md` for the `/book/chapterdownload` flow, ZIP/ZipCrypto/AES envelope, separate E-Ink authorization, entitlement/version checks and AGPL-3.0-only boundary.

## Practical experiment order

1. Keep the known-good blocking Web path as control.
2. Add request/phase timing and run 5 same-book cold-cache repetitions.
3. Test serialized host-scoped keep-alive on metadata/small responses only; keep streamed TAR/image requests separate.
4. Test ESP-IDF async state-machine behavior with one handle and a bounded event loop; do not add concurrent TLS.
5. Separately prototype E-Ink chapter archives after explicit native authorization.
6. Benchmark storage API changes only after network timing is stable.

Reject any variant that changes chapter/image completeness, triggers IDLE watchdog, causes auth replay ambiguity, or consumes materially more internal RAM.
