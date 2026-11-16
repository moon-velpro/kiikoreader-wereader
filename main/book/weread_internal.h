#pragma once

#include "weread.h"
#include "cJSON.h"

#define WR_HOST "https://weread.qq.com"
#define WR_DIR "/sdcard/.weread"
#define WR_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0"

typedef struct { char ps[128], pc[128], token[128], referer[384]; } wr_reader_t;

bool wr_value(const cJSON *value, char *out, size_t size);
bool wr_safe(const char *value);
bool wr_cancelled(void);
void wr_error_set(const char *message);
char *wr_http(const char *url, const char *body, const char *referer, size_t limit, size_t *length);
typedef bool (*wr_http_chunk_fn)(const char *data, size_t length, void *context);
bool wr_http_reader_fields(const char *url, size_t limit, wr_http_chunk_fn chunk, void *context);
char *wr_http_report(const char *body, const char *referer, bool *post_started);
cJSON *wr_json(const char *url, const char *body);
void wr_work_progress(unsigned done, unsigned total);
bool wr_reader(const char *book, const char *chapter, wr_reader_t *reader);
bool wr_reader_referer(const char *book, const char *chapter, char out[384]);
bool wr_reader_fields_cached(const char *book, const char *chapter, wr_reader_t *reader);
char *wr_chapter_content(const char *book, const char *chapter, const wr_reader_t *reader, bool *plain, size_t *length);
bool wr_remote_position(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct);
bool wr_remote_position_with_chapters(const char *id, wr_chapter_t *chapter, unsigned *offset, unsigned *pct,
                                      const wr_chapter_t *chapters, unsigned chapter_count);
bool wr_send_position(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct);
bool wr_send_duration(const char *id, const wr_chapter_t *chapter, unsigned offset, unsigned pct,
                      unsigned seconds, const wr_reader_t *reader, bool *post_started);
bool wr_cache(const wr_book_t *book, const char *path);
/// 上传/下载进度；失败时把可直接展示的原因写入 why（可为 NULL）。
/// Upload/download progress; on failure fills why with a user-facing reason (may be NULL).
bool wr_progress(const wr_book_t *book, const char *path, bool upload, const char **why);
