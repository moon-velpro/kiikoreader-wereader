#include "weread_internal.h"
#include "weread_zip_stream.h"
#include "weread_prefetch.h"
#include "weread_tar.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_font_size.h"
#include "book_progress.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "read_pico_sd_access.h"
#include "zip_reader.h"
#include "zlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { FILE *file, *directory; uint16_t count; } epub_writer_t;

/* Observational counters only; no change to request order or buffer ownership. */
static uint64_t s_cache_image_elapsed_us;
#define WR_PREFETCH_CHAPTER 1
#define WR_IMG_TAR_LIMIT (8u * 1024u * 1024u)

static const void *s_active_tar;
static size_t s_active_tar_bytes;
static unsigned s_tar_image_hits;

typedef struct {
    const wr_book_t *book;
    const wr_chapter_t *chapters;
} cache_prefetch_input_t;

typedef struct {
    char *content;
    size_t content_size;
    bool plain;
    char *tar;
    size_t tar_size;
} cache_prefetch_result_t;

static void *cache_prefetch_fetch(void *arg, unsigned index, size_t *size, bool *plain);
static void cache_prefetch_release(void *result);

static char *cache_fetch_chapter(const wr_book_t *book, const wr_chapter_t *chapter,
                                 bool *plain, size_t *size, unsigned index) {
    int64_t started = esp_timer_get_time();
    wr_reader_t reader;
    char *content = NULL;
    for (unsigned attempt = 0; attempt < 3 && !content && !wr_cancelled(); ++attempt) {
        if (wr_reader(book->id, chapter->id, &reader))
            content = wr_chapter_content(book->id, chapter->id, &reader, plain, size);
        if (!content && attempt < 2) vTaskDelay(pdMS_TO_TICKS(400u * (attempt + 1)));
    }
    if (!content) {
        if (!wr_cancelled() && !strcmp(wr_error_message(), "操作失败，请检查网络后重试"))
            wr_error_set("缓存失败：章节下载失败（已自动重试 3 次）");
        ESP_LOGW("WEREAD", "cache fetch failed index=%u chapter=%s error=%s",
                 index + 1, chapter->id, wr_error_message());
        return NULL;
    }
    ESP_LOGI("WEREAD", "cache fetch index=%u bytes=%u elapsed_ms=%lld result=1",
             index + 1, (unsigned)*size,
             (long long)((esp_timer_get_time() - started) / 1000));
    return content;
}

static void le16(uint8_t *dest, unsigned value) {
    dest[0] = value;
    dest[1] = value >> 8;
}

static void le32(uint8_t *dest, uint32_t value) {
    le16(dest, value);
    le16(dest+2, value >> 16);
}

static bool zip_entry(epub_writer_t *zip, const char *name, const void *data, size_t size) {
    if (zip->count >= ZIP_ENTRY_MAX || size > ZIP_OUTPUT_MAX) return false;
    uint8_t local[30] = {0}, central[46] = {0};
    long offset = ftell(zip->file);
    if (offset < 0) return false;
    unsigned name_size = strlen(name);
    uint32_t crc = crc32(0, data, size);
    le32(local, 0x04034b50);
    le16(local+4, 20);
    le32(local+14, crc);
    le32(local+18, size);
    le32(local+22, size);
    le16(local+26, name_size);
    le32(central, 0x02014b50);
    le16(central+4, 20);
    memcpy(central+6, local+4, 24);
    le32(central+42, offset);
    ++zip->count;
    return fwrite(local, 1, sizeof(local), zip->file) == sizeof(local) &&
        fwrite(name, 1, name_size, zip->file) == name_size && fwrite(data, 1, size, zip->file) == size &&
        fwrite(central, 1, sizeof(central), zip->directory) == sizeof(central) &&
        fwrite(name, 1, name_size, zip->directory) == name_size;
}

static bool zip_finish(epub_writer_t *zip) {
    long start = ftell(zip->file), size = ftell(zip->directory);
    if (start < 0 || size < 0 || fseek(zip->directory, 0, SEEK_SET)) return false;
    uint8_t buffer[1024];
    for (long remaining = size; remaining > 0;) {
        size_t n = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, n, zip->directory) != n || fwrite(buffer, 1, n, zip->file) != n) return false;
        remaining -= n;
    }
    uint8_t end[22] = {0};
    le32(end, 0x06054b50);
    le16(end+8, zip->count);
    le16(end+10, zip->count);
    le32(end+12, size);
    le32(end+16, start);
    return fwrite(end, 1, sizeof(end), zip->file) == sizeof(end);
}

static bool write_xml(FILE *file, const char *text, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char *escaped = NULL;
        switch (text[i]) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        case '\'': escaped = "&apos;"; break;
        default: break;
        }
        if (escaped ? fputs(escaped, file) == EOF : fputc((unsigned char)text[i], file) == EOF) return false;
    }
    return true;
}

static bool file_entry(epub_writer_t *zip, const char *name, FILE *entry, size_t max) {
    if (!zip || !entry || zip->count >= ZIP_ENTRY_MAX) return false;
    /* Stream the existing chapter/OPF/nav staging file into the ZIP once,
     * instead of allocating and copying the entire file into PSRAM. */
    const size_t scratch_bytes = 8192;
    char *scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) return false;
    if (max > ZIP_OUTPUT_MAX) max = ZIP_OUTPUT_MAX;
    bool ok = wr_zip_append_stored_file(zip->file, zip->directory, entry,
                                         name, max, scratch, scratch_bytes);
    if (ok) ++zip->count;
    free(scratch);
    return ok;
}

static const char *image_extension(const char *data, size_t length, const char **mime) {
    if (length >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) { *mime = "image/jpeg"; return "jpg"; }
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) { *mime = "image/png"; return "png"; }
    if (book_webp_is((const uint8_t *)data, length)) { *mime = "image/webp"; return "webp"; }
    if (book_gif_is((const uint8_t *)data, length)) { *mime = "image/gif"; return "gif"; }
    return NULL;
}

/* Exact-source deduplication: only reuse images that have already been
 * successfully added to this EPUB. Tokens remain in PSRAM and are never logged. */
#define WR_IMAGE_REUSE_SLOTS 48
typedef struct { char *url; char entry[40]; } wr_image_reuse_t;
static EXT_RAM_BSS_ATTR wr_image_reuse_t s_image_reuse[WR_IMAGE_REUSE_SLOTS];
static unsigned s_image_reuse_count, s_image_reuse_hits;

static void image_reuse_reset(void) {
    for (unsigned i = 0; i < s_image_reuse_count; ++i) {
        free(s_image_reuse[i].url);
        s_image_reuse[i].url = NULL;
    }
    s_image_reuse_count = 0;
    s_image_reuse_hits = 0;
}

static bool image_reuse_find(const char *url, char name[40]) {
    for (unsigned i = 0; i < s_image_reuse_count; ++i) {
        if (!strcmp(url, s_image_reuse[i].url)) {
            snprintf(name, 40, "%s", s_image_reuse[i].entry);
            ++s_image_reuse_hits;
            return true;
        }
    }
    return false;
}

static void image_reuse_add(const char *url, const char *name) {
    if (s_image_reuse_count >= WR_IMAGE_REUSE_SLOTS) return;
    size_t size = strlen(url) + 1;
    char *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return; /* Deduplication is best-effort; preserve download path. */
    memcpy(copy, url, size);
    wr_image_reuse_t *slot = &s_image_reuse[s_image_reuse_count++];
    slot->url = copy;
    snprintf(slot->entry, sizeof(slot->entry), "%s", name);
}

static bool image_entry(epub_writer_t *zip, FILE *opf, const char *src, size_t size, unsigned id, char name[40]) {
    if (!size || size >= 2048) { name[0] = 0; return !wr_cancelled(); }
    static EXT_RAM_BSS_ATTR char url[2056];
    size_t used = 0;
    if (size >= 2 && !memcmp(src, "//", 2)) { memcpy(url, "https:", 6); used = 6; }
    for (size_t i = 0; i < size; ++i) {
        if (i+5 <= size && !memcmp(src+i, "&amp;", 5)) { url[used++] = '&'; i += 4; }
        else url[used++] = src[i];
    }
    url[used] = 0;
    if (image_reuse_find(url, name)) return !wr_cancelled();
    int64_t image_started = esp_timer_get_time();
    size_t length = 0;
    const void *borrowed = NULL;
    bool from_tar = s_active_tar && weread_tar_image_url(url, strlen(url)) &&
                    wr_tar_find_image(s_active_tar, s_active_tar_bytes,
                                      url, strlen(url), &borrowed, &length);
    char *data = from_tar ? (char *)borrowed : NULL;
    if (from_tar) {
        const char *tar_mime = NULL;
        if (length > 4u * 1024u * 1024u || !image_extension(data, length, &tar_mime)) {
            /* The archive can contain non-image or oversized entries. Keep the
             * old per-image path as the correctness fallback. */
            data = NULL;
            length = 0;
            from_tar = false;
        } else {
            ++s_tar_image_hits;
        }
    }
    for (unsigned attempt = 0; attempt < 3 && !data && !wr_cancelled(); ++attempt) {
        data = wr_http(url, NULL, NULL, 4*1024*1024, &length);
        if (!data && !strcmp(wr_error_message(), "微信读书图片地址不受支持")) break;
        if (!data && attempt < 2) vTaskDelay(pdMS_TO_TICKS(350u * (attempt + 1)));
    }
    if (!data) {
        s_cache_image_elapsed_us += (uint64_t)(esp_timer_get_time() - image_started);
        ESP_LOGW("WEREAD", "cache image id=%u elapsed_ms=%lld result=0 error=%s", id,
                 (long long)((esp_timer_get_time() - image_started) / 1000), wr_error_message());
        /* CrossMux treats embedded illustrations as best-effort: a transient
         * CDN failure must not discard otherwise complete chapter text. */
        name[0] = 0;
        return !wr_cancelled();
    }
    const char *mime, *ext = image_extension(data, length, &mime);
    if (ext) snprintf(name, 40, "i%u.%s", id, ext);
    bool ok = !ext || (zip_entry(zip, name, data, length) &&
        fprintf(opf, "<item id=\"i%u\" href=\"%s\" media-type=\"%s\"%s/>", id, name, mime,
                id == 0 ? " properties=\"cover-image\"" : "") >= 0);
    s_cache_image_elapsed_us += (uint64_t)(esp_timer_get_time() - image_started);
    ESP_LOGI("WEREAD", "cache image id=%u bytes=%u elapsed_ms=%lld result=%d", id,
             (unsigned)length, (long long)((esp_timer_get_time() - image_started) / 1000), ok);
    if (ok && ext) image_reuse_add(url, name);
    if (!ext) name[0] = 0;
    if (!from_tar) free(data);
    if (!ok) wr_error_set("缓存失败：图片写入 EPUB 失败");
    return ok;
}

static const char *tag_end(const char *start) {
    char quote = 0;
    for (const char *p = start; *p; ++p) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

static const char *image_src(const char *p, const char *end, size_t *length) {
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        const char *key = p;
        while (p < end && !strchr("= \r\n\t", *p)) ++p;
        size_t key_size = p-key;
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p == end || *p != '=') continue;
        do { ++p; } while (p < end && isspace((unsigned char)*p));
        if (p == end) break;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) ++p;
        if ((key_size == 3 && !strncasecmp(key,"src",3)) || (key_size == 9 && !strncasecmp(key,"xlink:href",9))) {
            *length = p-value;
            return value;
        }
        if (quote && p < end) ++p;
    }
    return NULL;
}

static bool weread_tar_image_url(const char *src, size_t length) {
    static const char *const prefixes[] = {
        "https://res.weread.qq.com/wrepub/",
        "http://res.weread.qq.com/wrepub/",
        "//res.weread.qq.com/wrepub/"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t n = strlen(prefixes[i]);
        if (length >= n && !strncasecmp(src, prefixes[i], n)) return true;
    }
    return false;
}

static bool chapter_has_tar_images(const char *data) {
    for (const char *p = data; p && *p;) {
        if (*p != '<') { ++p; continue; }
        bool image = !strncasecmp(p, "<img", 4) &&
                     (isspace((unsigned char)p[4]) || p[4] == '>');
        bool svg_image = !strncasecmp(p, "<image", 6) &&
                         isspace((unsigned char)p[6]);
        if (!image && !svg_image) { ++p; continue; }
        const char *end = tag_end(p);
        if (!end) return false;
        size_t length = 0;
        const char *src = image_src(p + (image ? 4 : 6), end, &length);
        if (src && weread_tar_image_url(src, length)) return true;
        p = end + 1;
    }
    return false;
}

static char *cache_fetch_tar(const wr_book_t *book, const wr_chapter_t *chapter,
                             const char *content, bool plain, size_t *tar_bytes) {
    if (tar_bytes) *tar_bytes = 0;
    if (!chapter->tar[0] || plain || !chapter_has_tar_images(content) || wr_cancelled())
        return NULL;
    char tar_url[384], reader_referer[384];
    const char *src = chapter->tar;
    int n = !strncmp(src, "//", 2)
          ? snprintf(tar_url, sizeof(tar_url), "https:%s", src)
          : !strncmp(src, "http://", 7)
          ? snprintf(tar_url, sizeof(tar_url), "https:%s", src + 7)
          : *src == '/'
          ? snprintf(tar_url, sizeof(tar_url), "https://res.weread.qq.com%s", src)
          : snprintf(tar_url, sizeof(tar_url), "%s", src);
    if (n <= 0 || n >= (int)sizeof(tar_url) ||
        !wr_reader_referer(book->id, chapter->id, reader_referer)) return NULL;
    int64_t started = esp_timer_get_time();
    char *data = wr_http(tar_url, NULL, reader_referer, WR_IMG_TAR_LIMIT, tar_bytes);
    ESP_LOGI("WEREAD", "cache chapter tar index=%u bytes=%u elapsed_ms=%lld ok=%d",
             chapter->index, (unsigned)(tar_bytes ? *tar_bytes : 0),
             (long long)((esp_timer_get_time() - started) / 1000), (int)(data != NULL));
    return data;
}

static void cache_prefetch_release(void *opaque) {
    cache_prefetch_result_t *result = opaque;
    if (!result) return;
    free(result->content);
    free(result->tar);
    free(result);
}

static void *cache_prefetch_fetch(void *arg, unsigned index, size_t *size, bool *plain) {
    cache_prefetch_input_t *input = arg;
    cache_prefetch_result_t *result = heap_caps_calloc(1, sizeof(*result),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) return NULL;
    result->content = cache_fetch_chapter(input->book, &input->chapters[index],
                                          &result->plain, &result->content_size, index);
    if (!result->content) {
        free(result);
        return NULL;
    }
    result->tar = cache_fetch_tar(input->book, &input->chapters[index],
                                  result->content, result->plain, &result->tar_size);
    if (size) *size = sizeof(*result);
    if (plain) *plain = result->plain;
    return result;
}

static unsigned utf16_at(const char *text, size_t bytes) {
    unsigned count = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned c = (uint8_t)text[i];
        if ((c & 0xc0) != 0x80) count += c >= 0xf0 ? 2 : 1;
    }
    return count;
}

static bool marker(FILE *output, unsigned offset) {
    return fprintf(output,"<!--wr-co:%u-->",offset) >= 0;
}

static bool gif_source(const char *src, size_t size) {
    // 阅读器只渲染 png/jpeg/webp；gif 条目会被跳过，写入反而留下坏图引用。
    // The reader renders only png/jpeg/webp; a gif entry would become a broken reference.
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (!strncasecmp(src+i, ".gif", 4) && (i+4 == size || src[i+4] == '?' || src[i+4] == '#'
                                               || src[i+4] == '&')) return true;
    }
    return false;
}

static bool rewrite_chapter(FILE *output, FILE *opf, epub_writer_t *zip, const char *data, bool plain, unsigned *image_id) {
    if (fputs("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>", output) == EOF) return false;
    if (plain) {
        const char *line = data;
        unsigned offset = 0;
        while (*line && !wr_cancelled()) {
            size_t length = strcspn(line, "\r\n");
            if (fputs("<p>", output) == EOF || !marker(output,offset)) return false;
            for (size_t i = 0; i < length; ++i) {
                unsigned c = (uint8_t)line[i];
                bool escape = c < 128 && strchr("&<>\"'",c);
                if (escape && !marker(output,offset)) return false;
                if (!write_xml(output,line+i,1)) return false;
                if ((c & 0xc0) != 0x80) offset += c >= 0xf0 ? 2 : 1;
                if (escape && !marker(output,offset)) return false;
            }
            line += length;
            while (*line == '\r' || *line == '\n') { ++line; ++offset; }
            if (fputs("</p>",output) == EOF) return false;
        }
    } else {
        const char *p = strcasestr(data, "<body");
        if (p) { p = tag_end(p); if (p) ++p; }
        if (!p) p = data;
        unsigned offset = utf16_at(data,p-data);
        while (*p && !wr_cancelled()) {
            if (!strncasecmp(p, "</body", 6)) break;
            bool image = !strncasecmp(p,"<img",4) && (isspace((unsigned char)p[4]) || p[4] == '>');
            bool svg = !strncasecmp(p,"<image",6) && isspace((unsigned char)p[6]);
            if (image || svg) {
                const char *end = tag_end(p);
                if (!end) return false;
                size_t length;
                const char *src = image_src(p+(image?4:6), end, &length);
                char name[40];
                if (src && gif_source(src, length)) {
                    offset += utf16_at(p,end+1-p);
                    p = end+1;
                    continue;
                }
                if (src) {
                    if (!image_entry(zip, opf, src, length, (*image_id)++, name)) return false;
                    if (name[0] && fprintf(output, "<img src=\"%s\"/>", name) < 0) return false;
                }
                offset += utf16_at(p,end+1-p);
                p = end+1;
            } else if (*p == '<') {
                const char *end = tag_end(p);
                if (!end || fwrite(p,1,end+1-p,output) != (size_t)(end+1-p)) return false;
                offset += utf16_at(p,end+1-p);
                p = end+1;
            } else {
                size_t n = strcspn(p,"<");
                if (!marker(output,offset) || fwrite(p,1,n,output) != n) return false;
                offset += utf16_at(p,n);
                p += n;
            }
        }
    }
    return !wr_cancelled() && fputs("</body></html>", output) != EOF;
}

static const cJSON *chapter_array(const cJSON *root, const char *book_id, unsigned depth) {
    if (!root || depth > 5) return NULL;
    if (cJSON_IsArray(root)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, root) {
            if (cJSON_GetObjectItemCaseSensitive(entry, "chapterUid")) return root;
            char entry_id[WR_ID];
            if (wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), entry_id, sizeof(entry_id)) &&
                strcmp(entry_id, book_id)) continue;
            const cJSON *chapters = chapter_array(entry, book_id, depth + 1);
            if (chapters) return chapters;
        }
    } else if (cJSON_IsObject(root)) {
        static const char *const keys[] = {"chapterInfos", "updated", "data"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
            const cJSON *chapters = chapter_array(cJSON_GetObjectItemCaseSensitive(root, keys[i]), book_id, depth + 1);
            if (chapters) return chapters;
        }
    }
    return NULL;
}

static struct {
    char owner[WR_ID];
    char book[WR_ID];
    wr_chapter_t *items;
    unsigned count;
    int64_t expires;
} s_chapter_cache;

static wr_chapter_t *get_chapters(const wr_book_t *book, unsigned *count) {
    int64_t started = esp_timer_get_time();
    char query[100];
    *count = 0;
    char owner[WR_ID] = {0};
    (void)wr_account(owner, NULL, NULL);
    if (s_chapter_cache.items && s_chapter_cache.expires > started &&
        !strcmp(owner, s_chapter_cache.owner) && !strcmp(book->id, s_chapter_cache.book)) {
        wr_chapter_t *copy = heap_caps_malloc(s_chapter_cache.count * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, s_chapter_cache.items, s_chapter_cache.count * sizeof(*copy));
            *count = s_chapter_cache.count;
            ESP_LOGI("WEREAD", "chapter list cache hit book=%s chapters=%u", book->id, *count);
            return copy;
        }
    }
    snprintf(query, sizeof(query), "{\"bookIds\":[\"%s\"]}", book->id);
    cJSON *toc = wr_json("/web/book/chapterInfos", query);
    if (!toc) return NULL;
    const cJSON *chapters = chapter_array(toc, book->id, 0);
    int n = cJSON_GetArraySize(chapters);
    wr_chapter_t *result = n > 0 && n <= ZIP_ENTRY_MAX - 4
        ? heap_caps_calloc(n, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!result) {
        ESP_LOGW("WEREAD", "cache toc unavailable count=%d updated_type=%d data_type=%d", n,
                 cJSON_GetObjectItemCaseSensitive(toc, "updated") ? cJSON_GetObjectItemCaseSensitive(toc, "updated")->type : 0,
                 cJSON_GetObjectItemCaseSensitive(toc, "data") ? cJSON_GetObjectItemCaseSensitive(toc, "data")->type : 0);
        wr_error_set(n == 0 ? "缓存失败：微信读书未返回可用章节目录" :
                     n > ZIP_ENTRY_MAX - 4 ? "缓存失败：章节数量超出 EPUB 支持范围" :
                     "缓存失败：内存不足，无法载入章节目录");
    }
    const cJSON *entry;
    if (result) cJSON_ArrayForEach(entry, chapters) {
        wr_chapter_t *item = &result[*count];
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry,"chapterUid"), item->id, sizeof(item->id)) || !wr_safe(item->id)) {
            ESP_LOGW("WEREAD", "cache toc invalid chapter at index=%u", *count);
            wr_error_set("缓存失败：章节目录中有无效的章节标识");
            free(result);
            result = NULL;
            break;
        }
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(entry, "chapterIdx");
        item->index = cJSON_IsNumber(index) && index->valuedouble >= 0 && index->valuedouble <= UINT32_MAX ? (uint32_t)index->valuedouble : *count;
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry,"title"), item->title, sizeof(item->title));
        const cJSON *tar = cJSON_GetObjectItemCaseSensitive(entry, "tar");
        if (cJSON_IsString(tar) && tar->valuestring &&
            strlen(tar->valuestring) < sizeof(item->tar))
            snprintf(item->tar, sizeof(item->tar), "%s", tar->valuestring);
        ++*count;
    }
    cJSON_Delete(toc);
    if (result && owner[0]) {
        wr_chapter_t *saved = heap_caps_malloc(*count * sizeof(*saved), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (saved) {
            memcpy(saved, result, *count * sizeof(*saved));
            free(s_chapter_cache.items);
            snprintf(s_chapter_cache.owner, sizeof(s_chapter_cache.owner), "%s", owner);
            snprintf(s_chapter_cache.book, sizeof(s_chapter_cache.book), "%s", book->id);
            s_chapter_cache.items = saved;
            s_chapter_cache.count = *count;
            s_chapter_cache.expires = started + 600000000;
            ESP_LOGI("WEREAD", "chapter list cache updated book=%s chapters=%u ttl_s=600", book->id, *count);
        }
    }
    ESP_LOGI("WEREAD", "cache toc book=%s chapters=%u elapsed_ms=%lld", book->id, *count,
             (long long)((esp_timer_get_time()-started)/1000));
    return result;
}

static bool package_book(const wr_book_t *book, FILE *target, const wr_chapter_t *chapters, unsigned count) {
    static const char container[] = "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char type[] = "application/epub+zip";
    FILE *central = fopen(WR_DIR "/central.part", "w+b");
    FILE *opf = fopen(WR_DIR "/opf.part", "w+b");
    FILE *nav = fopen(WR_DIR "/nav.part", "w+b");
    if (central) (void)setvbuf(central, NULL, _IOFBF, 4096);
    if (opf) (void)setvbuf(opf, NULL, _IOFBF, 2048);
    if (nav) (void)setvbuf(nav, NULL, _IOFBF, 2048);
    epub_writer_t zip = {.file=target, .directory=central};
    bool ok = central && opf && nav && zip_entry(&zip, "mimetype", type, sizeof(type)-1) &&
        zip_entry(&zip, "META-INF/container.xml", container, sizeof(container)-1);
    if (ok) ok = fputs("<package version=\"3.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"book\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"book\">", opf) != EOF &&
        write_xml(opf, book->id, strlen(book->id)) && fputs("</dc:identifier><dc:title>", opf) != EOF &&
        write_xml(opf, book->title, strlen(book->title)) && fputs("</dc:title><dc:creator>", opf) != EOF &&
        write_xml(opf, book->author, strlen(book->author)) &&
        fputs("</dc:creator><dc:language>zh</dc:language></metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>", opf) != EOF &&
        fputs("<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>", nav) != EOF;
    image_reuse_reset();
    s_cache_image_elapsed_us = 0;
    s_active_tar = NULL;
    s_active_tar_bytes = 0;
    s_tar_image_hits = 0;
    if (ok && book->cover[0]) {
        char name[40];
        ok = image_entry(&zip, opf, book->cover, strlen(book->cover), 0, name);
    }
    unsigned image_id = 1;
    cache_prefetch_input_t prefetch_input = {.book = book, .chapters = chapters};
    wr_prefetch_t prefetch = {0};
    bool prefetch_enabled = false;
#if WR_PREFETCH_CHAPTER
    if (ok && count > 1) {
        prefetch_enabled = wr_prefetch_begin(&prefetch, cache_prefetch_fetch,
                                             cache_prefetch_release, &prefetch_input);
        ESP_LOGI("WEREAD", "cache chapter prefetch enabled=%d psram_free=%u",
                 (int)prefetch_enabled,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
#endif
    uint64_t prefetch_wait_us = 0;
    unsigned prefetch_hits = 0;
    uint64_t fetch_elapsed_us = 0;
    uint64_t transform_elapsed_us = 0;
    uint64_t chapter_elapsed_us = 0;
    for (unsigned i = 0; ok && i < count && !wr_cancelled(); ++i) {
        int64_t chapter_started = esp_timer_get_time();
        uint64_t image_before_us = s_cache_image_elapsed_us;
        size_t size = 0, tar_bytes = 0;
        bool plain = false;
        int64_t fetch_started = esp_timer_get_time();
        cache_prefetch_result_t *prefetched = NULL;
        char *content = NULL, *tar_data = NULL;
        if (prefetch_enabled && i) {
            prefetched = wr_prefetch_take(&prefetch, i, NULL, NULL);
            prefetch_wait_us += (uint64_t)(esp_timer_get_time() - fetch_started);
            if (prefetched) {
                content = prefetched->content;
                size = prefetched->content_size;
                plain = prefetched->plain;
                tar_data = prefetched->tar;
                tar_bytes = prefetched->tar_size;
                ++prefetch_hits;
            }
        }
        if (!content && !wr_cancelled()) {
            content = cache_fetch_chapter(book, &chapters[i], &plain, &size, i);
            if (content)
                tar_data = cache_fetch_tar(book, &chapters[i], content, plain, &tar_bytes);
        }
        fetch_elapsed_us += (uint64_t)(esp_timer_get_time() - fetch_started);
        if (!content) {
            if (!wr_cancelled() && !strcmp(wr_error_message(),"操作失败，请检查网络后重试"))
                wr_error_set("缓存失败：章节下载失败（已自动重试 3 次）");
            ESP_LOGE("WEREAD","cache chapter failed book=%s chapter=%s title=%s error=%s",book->id,chapters[i].id,chapters[i].title,wr_error_message());
            ok = false;
            break;
        }
        if (size >= 4 && !memcmp(content, "PK\003\004", 4)) {
            ok = !i && !fseek(target, 0, SEEK_SET) && fwrite(content, 1, size, target) == size &&
                !fflush(target) && !ftruncate(fileno(target), size);
            if (prefetched) cache_prefetch_release(prefetched);
            else { free(content); free(tar_data); }
            goto cleanup;
        }
        /* The current chapter and its TAR are now both ready. Start fetching
         * the next chapter plus its TAR while this chapter is rewritten. */
        if (prefetch_enabled && i + 1 < count && !wr_cancelled())
            (void)wr_prefetch_arm(&prefetch, i + 1);
        s_active_tar = tar_data;
        s_active_tar_bytes = tar_bytes;
        int64_t transform_started = esp_timer_get_time();
        char name[40];
        snprintf(name, sizeof(name), "c%u.xhtml", i);
        FILE *chapter = fopen(WR_DIR "/chapter.part", "w+b");
        if (chapter) (void)setvbuf(chapter, NULL, _IOFBF, 4096);
        ok = chapter && !memchr(content, 0, size) && rewrite_chapter(chapter, opf, &zip, content, plain, &image_id) &&
            file_entry(&zip, name, chapter, HTML_TEXT_MAX_BYTES) &&
            fprintf(opf,"<item id=\"c%u\" href=\"%s\" media-type=\"application/xhtml+xml\"/>",i,name) >= 0 &&
            fprintf(nav,"<li><a href=\"%s\">",name) >= 0 && write_xml(nav,chapters[i].title,strlen(chapters[i].title)) &&
            fputs("</a></li>",nav) != EOF;
        if (chapter) fclose(chapter);
        s_active_tar = NULL;
        s_active_tar_bytes = 0;
        uint64_t transform_us = (uint64_t)(esp_timer_get_time() - transform_started);
        transform_elapsed_us += transform_us;
        chapter_elapsed_us += (uint64_t)(esp_timer_get_time() - chapter_started);
        ESP_LOGI("WEREAD", "cache chapter book=%s index=%u/%u html_bytes=%u fetch_ms=%lld transform_ms=%lld image_ms=%lld elapsed_ms=%lld result=%d",
                 book->id, i+1, count, (unsigned)size,
                 (long long)((esp_timer_get_time() - fetch_started) / 1000),
                 (long long)(transform_us / 1000),
                 (long long)((s_cache_image_elapsed_us - image_before_us) / 1000),
                 (long long)((esp_timer_get_time()-chapter_started)/1000), ok);
        if (!ok && !wr_cancelled() && !strcmp(wr_error_message(),"操作失败，请检查网络后重试"))
            wr_error_set("缓存失败：章节内容处理或图片下载失败");
        if (!ok) ESP_LOGE("WEREAD","cache package failed book=%s chapter=%u title=%s error=%s",book->id,i,chapters[i].title,wr_error_message());
        if (prefetched) cache_prefetch_release(prefetched);
        else { free(content); free(tar_data); }
        wr_work_progress(i+1, count);
        /* Let idle/UI and watchdog service run between CPU-heavy chapter conversions. */
        vTaskDelay(1);
    }
    if (ok && !wr_cancelled()) {
        ok = fputs("</manifest><spine>", opf) != EOF;
        for (unsigned i = 0; ok && i < count; ++i) ok = fprintf(opf, "<itemref idref=\"c%u\"/>", i) >= 0;
        ok = ok && fputs("</spine></package>", opf) != EOF && fputs("</ol></nav></body></html>", nav) != EOF &&
            file_entry(&zip, "book.opf", opf, HTML_TEXT_MAX_BYTES) &&
            file_entry(&zip, "nav.xhtml", nav, HTML_TEXT_MAX_BYTES) && zip_finish(&zip);
    } else ok = false;
    ESP_LOGI("WEREAD", "cache profile book=%s chapters=%u fetch_ms=%lld transform_ms=%lld image_ms=%lld chapter_wall_ms=%lld",
             book->id, count,
             (long long)(fetch_elapsed_us / 1000),
             (long long)(transform_elapsed_us / 1000),
             (long long)(s_cache_image_elapsed_us / 1000),
             (long long)(chapter_elapsed_us / 1000));
    ESP_LOGI("WEREAD", "cache prefetch hits=%u wait_ms=%lld enabled=%d",
             prefetch_hits, (long long)(prefetch_wait_us / 1000), (int)prefetch_enabled);
cleanup:
    if (prefetch_enabled) wr_prefetch_end(&prefetch);
    ESP_LOGI("WEREAD", "cache image reuse hits=%u stored=%u tar_images=%u",
             s_image_reuse_hits, s_image_reuse_count, s_tar_image_hits);
    image_reuse_reset();
    if (nav) fclose(nav);
    if (opf) fclose(opf);
    if (central) fclose(central);
    unlink(WR_DIR "/chapter.part");
    unlink(WR_DIR "/central.part");
    unlink(WR_DIR "/opf.part");
    unlink(WR_DIR "/nav.part");
    return ok;
}

bool wr_cache(const wr_book_t *book, const char *path) {
    if (access(path, F_OK) == 0) {
        book_epub_t *existing=NULL;
        bool valid=book_epub_open(path,&existing)==ESP_OK && book_epub_chapter_count(existing)>0;
        book_epub_close(existing);
        if (valid) return true;
        ESP_LOGW("WEREAD","remove invalid cached epub book=%s path=%s",book->id,path);
        if (unlink(path) && errno!=ENOENT) { wr_error_set("缓存失败：旧缓存损坏且无法从 SD 卡删除"); return false; }
    }
    unsigned count;
    wr_chapter_t *chapters = get_chapters(book, &count);
    if (!chapters) {
        if (strcmp(wr_error_message(), "操作失败，请检查网络后重试")) return false;
        wr_error_set("缓存失败：章节目录获取失败");
        return false;
    }
    char temp[WR_PATH+8];
    snprintf(temp, sizeof(temp), "%s.part", path);
    FILE *file = fopen(temp, "w+b");
    if (file) (void)setvbuf(file, NULL, _IOFBF, 8192);
    if (!file) { wr_error_set("缓存失败：无法在 SD 卡创建临时书籍文件"); ESP_LOGE("WEREAD","cache temp open failed book=%s path=%s errno=%d",book->id,temp,errno); }
    bool ok = file && package_book(book, file, chapters, count) && !fflush(file) && !fsync(fileno(file));
    if (!ok && strcmp(wr_error_message(), "操作失败，请检查网络后重试") && !wr_cancelled()) {
        /* Preserve a more precise HTTP/image error supplied by a lower layer. */
    } else if (!ok && !wr_cancelled()) wr_error_set(file ? "缓存失败：章节处理或 SD 卡写入失败" : "缓存失败：无法在 SD 卡创建临时书籍文件");
    if (file && fclose(file)) ok = false;
    if (ok && !wr_cancelled()) {
        book_epub_t *epub = NULL;
        ok = book_epub_open(temp, &epub) == ESP_OK && book_epub_chapter_count(epub) == count;
        book_epub_close(epub);
        if (ok && rename(temp, path) != 0) { ok = false; wr_error_set("缓存失败：SD 卡无法保存书籍文件"); }
        else if (!ok && !wr_cancelled()) wr_error_set("缓存失败：下载后的 EPUB 校验失败");
    } else ok = false;
    if (!ok) unlink(temp);
    free(chapters);
    return ok;
}

static bool map_position(const char *path, unsigned chapter, bool upload, unsigned offset, unsigned *mapped) {
    zip_reader_t *zip = NULL;
    if (zip_open(path,&zip) != ESP_OK) return false;
    char name[32];
    snprintf(name,sizeof(name),"c%u.xhtml",chapter);
    int entry = zip_find(zip,name);
    size_t size = zip_entry_size(zip,entry);
    char *data = size && size <= HTML_TEXT_MAX_BYTES ? heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : NULL;
    bool ok = data && zip_extract(zip,entry,data,size) == ESP_OK && html_weread_position(data,size,upload,offset,mapped);
    free(data);
    zip_close(zip);
    // Chapter starts also work with complete EPUBs delivered by WeRead.
    if (!ok && !offset) { *mapped = 0; return true; }
    return ok;
}

bool wr_progress(const wr_book_t *book, const char *path, bool upload, const char **why) {
    if (why) *why = NULL;
    wr_error_set("操作失败，请检查网络后重试");
    struct stat st;
    book_progress_t local = {0};
    if (stat(path,&st) || st.st_size <= 0 || st.st_size > UINT32_MAX) {
        if (why) *why = "本地书籍文件不可用，请重新缓存";
        return false;
    }
    if (upload && !book_progress_load(path,(uint32_t)st.st_size,&local)) {
        if (why) *why = "这本书还没有本地阅读进度，先读一段再上传";
        return false;
    }
    unsigned count, offset = 0, pct = 0;
    wr_chapter_t remote;
    wr_chapter_t *chapters = get_chapters(book,&count);
    if (!chapters) {
        const char *detail = wr_error_message();
        if (why) *why = detail && *detail ? detail : "无法获取云端章节列表";
        return false;
    }
    unsigned index = local.chapter;
    if (!upload) {
        if (!wr_remote_position_with_chapters(book->id,&remote,&offset,&pct,chapters,count)) {
            free(chapters);
            const char *detail = wr_error_message();
            if (why) *why = detail && *detail ? detail : "微信读书里还没有这本书的阅读进度";
            return false;
        }
        index = count;
        for (unsigned i = 0; i < count; ++i) if (!strcmp(chapters[i].id,remote.id)) { index = i; break; }
        if (index >= count) {
            free(chapters);
            if (why) *why = "云端进度所在章节与本地版本不一致，无法定位";
            return false;
        }
    }
    unsigned mapped = 0;
    bool ok = index < count && map_position(path,index,upload,upload?local.byte_off:offset,&mapped);
    if (!ok && why) *why = upload ? "本地位置无法映射到云端章节，翻一页后再试"
                                 : "云端位置无法映射到本地章节，请重新缓存这本书";
    if (ok && upload) {
        ok = wr_send_position(book->id,&chapters[index],mapped,local.pct);
        if (!ok && why) *why = "上传进度失败，请检查网络后重试";
    } else if (ok) {
        (void)book_progress_load(path, (uint32_t)st.st_size, &local);
        if (local.px < BOOK_READER_PX_MIN || local.px > BOOK_READER_PX_MAX)
            local.px = BOOK_READER_PX_DEFAULT;
        local.file_size = (uint32_t)st.st_size;
        local.chapter = (uint16_t)index;
        local.byte_off = mapped;
        local.pct = (uint8_t)pct;
        esp_err_t save_err = wr_cancelled() ? ESP_ERR_INVALID_STATE : book_progress_save(path,&local);
        ok = save_err == ESP_OK;
        if (!ok) ESP_LOGW("WEREAD", "download cloud progress local save failed err=%s", esp_err_to_name(save_err));
        if (!ok && why) *why = "云端进度已获取，但设备保存本地阅读位置失败";
    }
    free(chapters);
    return ok;
}
