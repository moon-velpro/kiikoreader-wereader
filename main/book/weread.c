#include "weread_internal.h"
#include "weread_http_log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_progress.h"
#include "book_title.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "read_pico_sd.h"
#include "read_pico_sd_access.h"
#include "read_pico_transfer.h"
#include "settings.h"

#define WR_MAX_SHELF 8192
#define WR_REPORT_COUNT 16
#define WR_REPORT_STORE_MAGIC UINT32_C(0x57525231)
#define WR_INDEX WR_DIR "/shelf.bin"
#define WR_INDEX_TEMP WR_DIR "/shelf.part"
#define WR_CACHED WR_DIR "/cached.bin"
#define WR_SD_SCOPE(path) __attribute__((cleanup(read_pico_sd_access_scope_end))) \
    read_pico_sd_access_scope_t scope = read_pico_sd_access_scope(path)

typedef struct { char owner[WR_ID], token[384]; uint32_t library; } wr_account_t;
typedef struct { uint32_t day, today; uint64_t total; } wr_stats_t;
typedef struct { char owner[WR_ID]; uint32_t count; } wr_shelf_header_t;
typedef struct {
    wr_job_t kind;
    wr_book_t book;
    unsigned first, count;
} wr_work_t;
typedef struct {
    char book_id[WR_ID], owner[WR_ID];
    uint32_t library;
    unsigned seconds;
    unsigned failures;
    int64_t retry_at;
} wr_report_t;
typedef struct {
    char book_id[WR_ID], owner[WR_ID];
    uint32_t library, seconds;
} wr_report_store_entry_t;
typedef struct {
    uint32_t magic;
    wr_report_store_entry_t entries[WR_REPORT_COUNT];
} wr_report_store_t;
_Static_assert(sizeof(wr_report_store_t) <= 1900, "WeRead report queue must fit in one NVS blob");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
enum { WR_WORKER_STACK_BYTES = 16 * 1024, WR_HTTP_URL_BYTES = 2080 };
// 缓存和上报都会写闪存，任务栈必须位于内部 RAM。/ Both cache and report write flash, so the task stack must reside in internal RAM.
static DRAM_ATTR StackType_t s_worker_stack[WR_WORKER_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker;
static wr_work_t s_work;
static wr_report_t s_reports[WR_REPORT_COUNT];
static bool s_work_pending;
static bool s_processing_report;
static bool s_reader_scanning;
static int64_t s_reader_scan_end;
static esp_http_client_handle_t s_http_keepalive;
static char s_http_keepalive_host[128];
static struct {
    wr_report_t account_book;
    char chapter[WR_ID];
    wr_reader_t fields;
    int64_t expires;
} s_report_fields;
// Each HTTP request owns redirect and cookie-capture state, so bounded cache
// prefetch requests can overlap without corrupting one another.
typedef struct {
    char absolute[WR_HTTP_URL_BYTES];
    char redirected[WR_HTTP_URL_BYTES];
    char location[WR_HTTP_URL_BYTES];
    char current[WR_HTTP_URL_BYTES];
    char cookie_snapshot[1024];
    bool capture_service_cookies;
    bool capture_body;
    bool body_overflow;
    bool *post_started;
    size_t body_limit, body_used, body_capacity;
    char *body_data;
} wr_http_context_t;
static StaticSemaphore_t s_cookie_mutex_storage;
static SemaphoreHandle_t s_cookie_mutex;
static StaticSemaphore_t s_report_store_mutex_storage;
static SemaphoreHandle_t s_report_store_mutex;
static wr_result_t s_result;
static wr_job_t s_active_job;
static atomic_bool s_cancel;
static unsigned s_done, s_total;
static char s_qr[180];
static wr_account_t s_http_account;
static char s_cookies[1024];
static _Atomic bool s_expired;
static _Atomic bool s_http_open_failed;
static _Atomic(const char *) s_error_message = "操作失败，请检查网络后重试";
static const char *s_result_message = "操作失败，请检查网络后重试";
/* 阅读会话归属于开书时的账号：统计与上报都用这份快照，切号后不会写到新账号。
 * A reading session belongs to the account that opened the book; reports and stats use
 * this snapshot so a later account switch can never bill the new account. */
static struct {
    wr_book_t book;
    char owner[WR_ID];
    uint32_t library;
    int64_t last;
    uint32_t ms;
    bool attached;
} s_reading;
static bool s_radio_held;

static void restore_session_cookie(void);
static bool wr_url_allowed(const char *url, bool service);
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]);
static bool renew_session(void);

const char *wr_error_message(void) { return atomic_load(&s_error_message); }
void wr_error_set(const char *message) { if (message && *message) atomic_store(&s_error_message, message); }
const char *wr_result_message(void) {
    portENTER_CRITICAL(&s_lock);
    const char *message = s_result_message;
    portEXIT_CRITICAL(&s_lock);
    return message;
}

static bool read_account(wr_account_t *account) {
    memset(account, 0, sizeof(*account));
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(*account);
    esp_err_t result = nvs_get_blob(nvs, "account", account, &size);
    nvs_close(nvs);
    return result == ESP_OK && size == sizeof(*account) &&
        memchr(account->owner, 0, sizeof(account->owner)) && memchr(account->token, 0, sizeof(account->token));
}

static bool save_account(const wr_account_t *account) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(nvs, "account", account, sizeof(*account));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

static void save_session_cookies(void) {
    if (!s_cookies[0]) return;
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, "cookies", s_cookies) == ESP_OK) (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void cookie_lock(void) {
    if (s_cookie_mutex) (void)xSemaphoreTake(s_cookie_mutex, portMAX_DELAY);
}

static void cookie_unlock(void) {
    if (s_cookie_mutex) (void)xSemaphoreGive(s_cookie_mutex);
}

/* Persist only the durable queue payload. Monotonic retry deadlines are rebuilt
 * after boot, so no stale esp_timer_get_time() values enter NVS. */
static bool persist_report_queue(void) {
    if (!s_report_store_mutex ||
        xSemaphoreTake(s_report_store_mutex, portMAX_DELAY) != pdTRUE) return false;

    wr_report_store_t snapshot = {.magic = WR_REPORT_STORE_MAGIC};
    bool any = false;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < WR_REPORT_COUNT; ++i) {
        const wr_report_t *source = &s_reports[i];
        if (!source->seconds) continue;
        wr_report_store_entry_t *target = &snapshot.entries[i];
        memcpy(target->book_id, source->book_id, sizeof(target->book_id));
        memcpy(target->owner, source->owner, sizeof(target->owner));
        target->library = source->library;
        target->seconds = source->seconds;
        any = true;
    }
    portEXIT_CRITICAL(&s_lock);

    nvs_handle_t nvs;
    esp_err_t result = nvs_open("weread", NVS_READWRITE, &nvs);
    if (result == ESP_OK) {
        if (any) result = nvs_set_blob(nvs, "reports", &snapshot, sizeof(snapshot));
        else {
            result = nvs_erase_key(nvs, "reports");
            if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
        }
        if (result == ESP_OK) result = nvs_commit(nvs);
        nvs_close(nvs);
    }
    (void)xSemaphoreGive(s_report_store_mutex);
    return result == ESP_OK;
}

static void load_report_queue(void) {
    if (!s_report_store_mutex) return;
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) != ESP_OK) return;

    wr_report_store_t stored;
    size_t size = sizeof(stored);
    esp_err_t result = nvs_get_blob(nvs, "reports", &stored, &size);
    nvs_close(nvs);
    if (result == ESP_ERR_NVS_NOT_FOUND) return;
    if (result != ESP_OK || size != sizeof(stored) || stored.magic != WR_REPORT_STORE_MAGIC) {
        ESP_LOGW("WEREAD", "discard invalid persisted report queue err=%s size=%u",
                 esp_err_to_name(result), (unsigned)size);
        return;
    }

    unsigned loaded = 0;
    for (unsigned i = 0; i < WR_REPORT_COUNT; ++i) {
        const wr_report_store_entry_t *source = &stored.entries[i];
        if (!source->seconds ||
            !memchr(source->book_id, 0, sizeof(source->book_id)) ||
            !memchr(source->owner, 0, sizeof(source->owner)) ||
            !wr_safe(source->book_id) || !wr_safe(source->owner)) continue;
        wr_report_t *target = &s_reports[i];
        memcpy(target->book_id, source->book_id, sizeof(target->book_id));
        memcpy(target->owner, source->owner, sizeof(target->owner));
        target->library = source->library;
        target->seconds = source->seconds;
        target->failures = 0;
        target->retry_at = 0;
        ++loaded;
    }
    if (loaded) ESP_LOGI("WEREAD", "restored pending report queue entries=%u", loaded);
}

static bool clear_session_cookies(void) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_erase_key(nvs, "cookies");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    memset(s_cookies, 0, sizeof(s_cookies));
    return result == ESP_OK;
}

static bool session_cookie_has(const char *name) {
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) return true;
        item = *end ? end + 2 : end;
    }
    return false;
}

static bool session_cookie_copy(const char *name, char *out, size_t size) {
    if (!out || !size) return false;
    out[0] = 0;
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) {
            size_t value_length = (size_t)(end - item) - length - 1;
            if (!value_length || value_length >= size) return false;
            memcpy(out, item + length + 1, value_length);
            out[value_length] = 0;
            return true;
        }
        item = *end ? end + 2 : end;
    }
    return false;
}

static void stats_key(const char *owner, char key[15]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *owner; ++owner) hash = (hash ^ (unsigned char)*owner) * UINT64_C(1099511628211);
    snprintf(key, 15, "s%013llx", (unsigned long long)(hash & UINT64_C(0xfffffffffffff)));
}

static uint32_t current_day(void) {
    time_t now = time(NULL);
    return now >= 1704067200 ? (uint32_t)((now + 8 * 3600) / 86400) : 0;
}

static wr_stats_t read_stats(nvs_handle_t nvs, const char *key) {
    wr_stats_t stats = {0};
    size_t size = sizeof(stats);
    if (nvs_get_blob(nvs, key, &stats, &size) != ESP_OK || size != sizeof(stats)) stats = (wr_stats_t){0};
    if (stats.day != current_day()) { stats.day = current_day(); stats.today = 0; }
    return stats;
}

bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total) {
    wr_account_t account;
    const bool logged_in = read_account(&account) && account.token[0];
    if (id) snprintf(id, WR_ID, "%s", account.owner);
    wr_stats_t stats = {0};
    nvs_handle_t nvs;
    if (account.owner[0] && (today || total) && nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        char key[15];
        stats_key(account.owner, key);
        stats = read_stats(nvs, key);
        nvs_close(nvs);
    }
    if (today) *today = stats.today;
    if (total) *total = stats.total;
    return logged_in;
}

static bool add_stats(const char *owner, unsigned seconds) {
    char key[15];
    stats_key(owner, key);
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    wr_stats_t stats = read_stats(nvs, key);
    stats.today = seconds > UINT32_MAX-stats.today ? UINT32_MAX : stats.today+seconds;
    stats.total = seconds > UINT64_MAX-stats.total ? UINT64_MAX : stats.total+seconds;
    esp_err_t result = nvs_set_blob(nvs, key, &stats, sizeof(stats));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

bool wr_online(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.network_ready && status.mode == READ_PICO_TRANSFER_MODE_STA;
}

bool wr_safe(const char *value) {
    if (!value || !*value) return false;
    for (; *value; ++value) if (!isalnum((unsigned char)*value) && !strchr("-_.~", *value)) return false;
    return true;
}

bool wr_value(const cJSON *value, char *out, size_t size) {
    if (cJSON_IsString(value)) return strlcpy(out, value->valuestring, size) < size;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > 9007199254740991.0) return false;
    int n = snprintf(out, size, "%.0f", value->valuedouble);
    return n > 0 && (size_t)n < size;
}

bool wr_cancelled(void) { return !s_processing_report && atomic_load(&s_cancel); }
void wr_cancel(void) { atomic_store(&s_cancel, true); }

/* 阅读会话期间保持接收唤醒，退出时恢复原有省电策略。 */
static void radio_hold(bool on) {
    if (on == s_radio_held) return;
    s_radio_held = on;
    (void)read_pico_transfer_radio_hold(on);
}

static bool account_is(const char *owner, uint32_t library) {
    wr_account_t current;
    return read_account(&current) && current.token[0] &&
        !strcmp(current.owner, owner) && current.library == library;
}

bool wr_reader_fields_cached(const char *book, const char *chapter, wr_reader_t *reader) {
    if (!book || !chapter || !reader || s_report_fields.expires <= esp_timer_get_time() ||
        strcmp(s_http_account.owner, s_report_fields.account_book.owner) ||
        s_http_account.library != s_report_fields.account_book.library ||
        strcmp(book, s_report_fields.account_book.book_id) ||
        strcmp(chapter, s_report_fields.chapter)) return false;
    *reader = s_report_fields.fields;
    ESP_LOGI("WEREAD", "reader fields reused book=%s", book);
    return true;
}

static esp_err_t capture_cookie(esp_http_client_event_t *event) {
    wr_http_context_t *context = event->user_data;
    if (!context) return ESP_OK;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
    if (!strcasecmp(event->header_key,"Location")) {
        size_t n = strnlen(event->header_value, WR_HTTP_URL_BYTES);
        if (n && n < WR_HTTP_URL_BYTES) {
            memcpy(context->location,event->header_value,n);
            context->location[n]=0;
        }
        return ESP_OK;
    }
    if (strcasecmp(event->header_key,"Set-Cookie") ||
        !context->capture_service_cookies) return ESP_OK;
    const char *header = event->header_value;
    const char *equals = strchr(header,'=');
    if (!equals || strncmp(header,"wr_",3) || equals-header > 24) return ESP_OK;
    size_t key_len = equals-header, len = strcspn(header,";\r\n");
    if (len <= key_len+1 || len > 440) return ESP_OK;
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)header[i] <= ' ' || (unsigned char)header[i] >= 127) return ESP_OK;
    }
    cookie_lock();
    char *p = s_cookies;
    while (*p) {
        char *end = strstr(p,"; ");
        if (!end) end = p+strlen(p);
        if ((size_t)(end-p) > key_len && p[key_len]=='=' && !strncmp(p,header,key_len)) {
            if (*end) memmove(p,end+2,strlen(end+2)+1);
            else if (p>s_cookies) p[-2]=0;
            else *p=0;
            break;
        }
        p = *end ? end+2 : end;
    }
    size_t used = strlen(s_cookies);
    if (used+len+3 <= sizeof(s_cookies)) {
        if (used) { memcpy(s_cookies+used,"; ",2); used+=2; }
        memcpy(s_cookies+used,header,len);
        s_cookies[used+len]=0;
        save_session_cookies();
    }
    cookie_unlock();
    return ESP_OK;
}

static void http_keepalive_drop(void) {
    if (s_http_keepalive) {
        esp_http_client_cleanup(s_http_keepalive);
        s_http_keepalive = NULL;
    }
    s_http_keepalive_host[0] = 0;
}

static esp_err_t capture_keepalive_event(esp_http_client_event_t *event) {
    wr_http_context_t *request = event ? event->user_data : NULL;
    if (!request) return ESP_OK;
    if (event->event_id == HTTP_EVENT_HEADERS_SENT && request->post_started) {
        *request->post_started = true;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && request->capture_body) {
        if (wr_cancelled()) return ESP_FAIL;
        if (event->data_len < 0 || (size_t)event->data_len > request->body_limit - request->body_used) {
            request->body_overflow = true;
            return ESP_FAIL;
        }
        size_t needed = request->body_used + (size_t)event->data_len;
        if (needed > request->body_capacity) {
            size_t next = request->body_capacity ? request->body_capacity : 4096;
            while (next < needed) {
                size_t grown = next <= request->body_limit / 2 ? next * 2 : request->body_limit;
                if (grown <= next) { next = needed; break; }
                next = grown;
            }
            if (next > request->body_limit) next = request->body_limit;
            char *data = heap_caps_realloc(request->body_data, next + 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!data) return ESP_ERR_NO_MEM;
            request->body_data = data;
            request->body_capacity = next;
        }
        if (event->data_len) {
            memcpy(request->body_data + request->body_used, event->data, (size_t)event->data_len);
            request->body_used += (size_t)event->data_len;
        }
        return ESP_OK;
    }
    return capture_cookie(event);
}

static bool http_url_host(const char *url, char host[128]) {
    if (!url || strncmp(url, "https://", 8)) return false;
    const char *start = url + 8;
    const char *end = strpbrk(start, "/?#");
    if (!end) end = start + strlen(start);
    size_t length = (size_t)(end - start);
    if (!length || length >= 128) return false;
    memcpy(host, start, length);
    host[length] = 0;
    return true;
}

static esp_http_client_handle_t http_keepalive_get(const char *url, wr_http_context_t *request) {
    char host[128];
    if (!http_url_host(url, host)) return NULL;
    if (s_http_keepalive && strcasecmp(host, s_http_keepalive_host)) http_keepalive_drop();
    if (!s_http_keepalive) {
        esp_http_client_config_t config = {
            .url = url, .timeout_ms = 15000, .buffer_size = 4096, .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
            .keep_alive_enable = true, .keep_alive_idle = 10, .keep_alive_interval = 5,
            .keep_alive_count = 2, .user_agent = WR_AGENT,
            .event_handler = capture_keepalive_event, .user_data = request,
        };
        s_http_keepalive = esp_http_client_init(&config);
        if (!s_http_keepalive) return NULL;
        snprintf(s_http_keepalive_host, sizeof(s_http_keepalive_host), "%s", host);
    } else {
        if (esp_http_client_set_url(s_http_keepalive, url) != ESP_OK ||
            esp_http_client_set_event_handler(s_http_keepalive, capture_keepalive_event) != ESP_OK ||
            esp_http_client_set_user_data(s_http_keepalive, request) != ESP_OK) return NULL;
    }
    return s_http_keepalive;
}

static char *http_request_reusable(wr_http_context_t *request, const char *url, const char *body,
                                   const char *referer, size_t limit, size_t *length,
                                   bool *post_started) {
    request->capture_body = true;
    request->body_limit = limit;
    request->post_started = post_started;
    request->capture_service_cookies = url[0] == '/';
    bool service = request->capture_service_cookies;
    char *absolute = request->absolute;
    const bool upgrade = !service && !strncmp(url, "http://", 7);
    const char *prefix = service ? WR_HOST : upgrade ? "https://" : "";
    int n = snprintf(absolute, WR_HTTP_URL_BYTES, "%s%s", prefix, upgrade ? url + 7 : url);
    if (n < 0 || n >= WR_HTTP_URL_BYTES || (!service && !wr_url_allowed(absolute, false))) {
        wr_error_set("微信读书请求地址无效");
        return NULL;
    }
    for (const char *p = absolute; *p; ++p) {
        if ((unsigned char)*p <= ' ' || *p == '\\') {
            wr_error_set("微信读书请求地址无效");
            return NULL;
        }
    }
    cookie_lock();
    memcpy(request->cookie_snapshot, s_cookies, sizeof(request->cookie_snapshot));
    request->cookie_snapshot[sizeof(request->cookie_snapshot)-1] = 0;
    cookie_unlock();

    char request_host[128];
    bool reused = http_url_host(absolute, request_host) && s_http_keepalive &&
        !strcasecmp(request_host, s_http_keepalive_host);
    esp_http_client_handle_t http = http_keepalive_get(absolute, request);
    if (!http) { wr_error_set("无法创建网络请求"); http_keepalive_drop(); return NULL; }
    esp_http_client_delete_all_headers(http);
    esp_http_client_reset_redirect_counter(http);
    esp_http_client_set_method(http, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(http, "Accept-Encoding", "identity");
    esp_http_client_set_header(http, "Referer", referer ? referer : WR_HOST "/");
    if (service) {
        esp_http_client_set_header(http, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(http, "Origin", WR_HOST);
        if (request->cookie_snapshot[0])
            esp_http_client_set_header(http, "Cookie", request->cookie_snapshot);
    }
    int body_size = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(http, "Content-Type", "application/json;charset=UTF-8");
    if (esp_http_client_set_post_field(http, body, body_size) != ESP_OK) {
        wr_error_set("无法准备微信读书请求");
        http_keepalive_drop();
        return NULL;
    }
    int64_t started = esp_timer_get_time();
    int code = 0;
    for (unsigned redirects = 0;;) {
        request->location[0] = 0;
        free(request->body_data);
        request->body_data = NULL;
        request->body_used = request->body_capacity = 0;
        request->body_overflow = false;
        snprintf(request->current, sizeof(request->current), "%s", absolute);
        if (wr_cancelled()) { wr_error_set("操作已取消"); http_keepalive_drop(); return NULL; }
        esp_err_t result = esp_http_client_perform(http);
        if (result != ESP_OK) {
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
            if (request->body_overflow) wr_error_set("微信读书响应超出允许大小");
            else if (wr_cancelled()) wr_error_set("操作已取消");
            else if (tls_error != ESP_OK || result == ESP_ERR_HTTP_CONNECT) {
                s_http_open_failed = true;
                wr_error_set("微信读书网络连接失败，请检查 Wi-Fi 后重试");
            } else wr_error_set("读取微信读书响应失败");
            char log_url[256];
            wr_http_log_target(request->current, log_url, sizeof(log_url));
            ESP_LOGW("WEREAD", "HTTP perform failed url=%s err=%s tls=%s code=%d flags=0x%x bytes=%u",
                     log_url, esp_err_to_name(result), esp_err_to_name(tls_error), tls_code,
                     (unsigned)tls_flags, (unsigned)request->body_used);
            free(request->body_data);
            request->body_data = NULL;
            http_keepalive_drop();
            return NULL;
        }
        code = esp_http_client_get_status_code(http);
        if (code == 401 || code == 403) {
            if (service) { s_expired = true; wr_error_set("登录已失效，请重新扫码"); }
            else wr_error_set("微信读书图片请求未获授权");
        } else if (code < 200 || code >= 300) wr_error_set("微信读书接口返回错误");
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect) break;
        if (redirects++ >= 5) { wr_error_set("微信读书接口重定向次数过多"); http_keepalive_drop(); return NULL; }
        char redirected[WR_HTTP_URL_BYTES];
        if (!service) {
            if (!cover_redirect_url(request->current, request->location, redirected) ||
                !wr_url_allowed(redirected, false) || esp_http_client_set_url(http, redirected) != ESP_OK) {
                wr_error_set("微信读书图片重定向地址不受支持");
                http_keepalive_drop();
                return NULL;
            }
        } else {
            if (esp_http_client_set_redirection(http) != ESP_OK ||
                esp_http_client_get_url(http, redirected, sizeof(redirected)) != ESP_OK ||
                !wr_url_allowed(redirected, true)) {
                wr_error_set("微信读书接口重定向失败");
                http_keepalive_drop();
                return NULL;
            }
        }
        char log_from[256], log_to[256];
        wr_http_log_target(request->current, log_from, sizeof(log_from));
        wr_http_log_target(redirected, log_to, sizeof(log_to));
        ESP_LOGI("WEREAD", "HTTP redirect %s -> %s", log_from, log_to);
        snprintf(absolute, WR_HTTP_URL_BYTES, "%s", redirected);
        if (code == 301 || code == 302 || code == 303) {
            esp_http_client_set_method(http, HTTP_METHOD_GET);
            esp_http_client_set_post_field(http, NULL, 0);
            body_size = 0;
        }
        free(request->body_data);
        request->body_data = NULL;
    }
    if (code != 200 || !request->body_data) {
        if (code == 200 && !request->body_data && request->body_used == 0) {
            request->body_data = heap_caps_malloc(1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (code != 200 || !request->body_data) {
            free(request->body_data);
            request->body_data = NULL;
            http_keepalive_drop();
            return NULL;
        }
    }
    request->body_data[request->body_used] = 0;
    if (length) *length = request->body_used;
    char log_url[256];
    wr_http_log_target(request->current, log_url, sizeof(log_url));
    ESP_LOGI("WEREAD", "HTTP complete url=%s bytes=%u elapsed_ms=%lld connection_reused=%d",
             log_url, (unsigned)request->body_used,
             (long long)((esp_timer_get_time() - started) / 1000), reused);
    char *result = request->body_data;
    request->body_data = NULL;
    return result;
}

static void restore_session_cookie(void) {
    cookie_lock();
    if (s_cookies[0]) { cookie_unlock(); return; }
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(s_cookies);
        if (nvs_get_str(nvs, "cookies", s_cookies, &size) == ESP_OK && s_cookies[0]) {
            nvs_close(nvs);
            cookie_unlock();
            return;
        }
        nvs_close(nvs);
    }
    wr_account_t account;
    if (!read_account(&account) || !account.owner[0] || !account.token[0]) { cookie_unlock(); return; }
    /* CrossMux persists the same three WeRead cookies across reboot.  QR login
     * gives us the vid and skey; rebuild the durable pair before shelf calls. */
    snprintf(s_cookies, sizeof(s_cookies), "wr_vid=%s; wr_skey=%s", account.owner, account.token);
    cookie_unlock();
}

static bool wr_url_allowed(const char *url, bool service) {
    if (!url || strncmp(url,"https://",8)) return false;
    const char *host=url+8, *end=strpbrk(host,"/?#");
    if (!end) end=host+strlen(host);
    const char *port = memchr(host, ':', (size_t)(end-host));
    if (port) {
        if (end-port != 4 || memcmp(port, ":443", 4)) return false;
        end = port;
    }
    size_t n=(size_t)(end-host);
    if (!n || n>=128) return false;
    for (const char *p=host;p<end;++p)
        if (!isalnum((unsigned char)*p) && *p!='.' && *p!='-') return false;
    if (service) return n==strlen("weread.qq.com") && !strncasecmp(host,"weread.qq.com",n);
    static const char *const suffixes[]={".qq.com",".qpic.cn",".qlogo.cn",".myqcloud.com",".gfp.tencent-cloud.com"};
    for (size_t i=0;i<sizeof(suffixes)/sizeof(*suffixes);++i) {
        size_t suffix=strlen(suffixes[i]);
        if (n>suffix && !strncasecmp(end-suffix,suffixes[i],suffix)) return true;
    }
    return false;
}

static bool http_body_session_expired(const char *body) {
    if (!body || body[0] != '{') return false;
    const char *key = strstr(body, "\"errcode\"");
    if (!key) key = strstr(body, "\"errCode\"");
    if (!key) return false;
    const char *value = strchr(key, ':');
    if (!value) return false;
    ++value;
    while (isspace((unsigned char)*value)) ++value;
    bool quoted = *value == '"';
    if (quoted) ++value;
    if (strncmp(value, "-2012", 5)) return false;
    value += 5;
    return quoted ? *value == '"' :
        (*value == ',' || *value == '}' || isspace((unsigned char)*value));
}

static void log_rejected_image_url(const char *url) {
    const char *scheme = url ? strstr(url, "://") : NULL;
    const char *host = scheme ? scheme + 3 : (url ? url : "");
    const char *end = strpbrk(host, "/?#");
    if (!end) end = host + strlen(host);
    char safe_host[128];
    size_t length = (size_t)(end - host);
    if (length >= sizeof(safe_host)) length = sizeof(safe_host) - 1;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)host[i];
        safe_host[i] = (isalnum(c) || c == '.' || c == '-' || c == ':') ? (char)c : '?';
    }
    safe_host[length] = 0;
    ESP_LOGW("WEREAD", "image URL rejected scheme=%s host=%s length=%u",
             scheme ? (size_t)(scheme - url) == 5 ? "https" : (size_t)(scheme - url) == 4 ? "http" : "other" : "none",
             safe_host, (unsigned)(url ? strlen(url) : 0));
}

/* Resolve WeRead CDN redirects ourselves. ESP-IDF rejects HTTPS -> HTTP
 * redirects before callers can normalize them, while WeRead sometimes emits
 * exactly that URL form. Upgrade it to HTTPS, then apply the host allowlist. */
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]) {
    if (!current || !location || !*location) return false;
    char resolved[WR_HTTP_URL_BYTES];
    int n;
    if (!strncasecmp(location,"https://",8) || !strncasecmp(location,"http://",7)) {
        n=snprintf(resolved,sizeof(resolved),"%s",location);
    } else if (!strncmp(location,"//",2)) {
        n=snprintf(resolved,sizeof(resolved),"https:%s",location);
    } else {
        const char *authority_end=strpbrk(current+8,"/?#");
        if (!authority_end) authority_end=current+strlen(current);
        if (location[0]=='/') {
            n=snprintf(resolved,sizeof(resolved),"%.*s%s",(int)(authority_end-current),current,location);
        } else {
            const char *last=strrchr(current,'/');
            if (!last || last<authority_end) last=authority_end;
            n=snprintf(resolved,sizeof(resolved),"%.*s/%s",(int)(last-current),current,location);
        }
    }
    if (n<0 || (size_t)n>=sizeof(resolved)) return false;
    if (!strncmp(resolved,"http://",7)) {
        char *authority_end=strpbrk(resolved+7,"/?#");
        if (!authority_end) authority_end=resolved+strlen(resolved);
        char authority[256];
        size_t host_n=(size_t)(authority_end-(resolved+7));
        if (!host_n || host_n>=sizeof(authority)) return false;
        memcpy(authority,resolved+7,host_n); authority[host_n]=0;
        size_t keep=host_n;
        if (keep>3 && !strcmp(authority+keep-3,":80")) { keep-=3; authority[keep]=0; }
        n=snprintf(out,WR_HTTP_URL_BYTES,"https://%s%s",authority,authority_end);
    } else n=snprintf(out,WR_HTTP_URL_BYTES,"%s",resolved);
    return n>0 && (size_t)n<WR_HTTP_URL_BYTES;
}

void wr_reading_display(bool scanning) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_reader_scanning = scanning;
    if (!scanning) s_reader_scan_end = now;
    portEXIT_CRITICAL(&s_lock);
}

static bool report_display_busy(void) {
    portENTER_CRITICAL(&s_lock);
    bool busy = s_reader_scanning;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

static char *http_request(const char *url, const char *body, const char *referer, size_t limit,
                          size_t *length, wr_http_chunk_fn chunk, void *context,
                          bool *fields_received, bool *post_started) {
    const int64_t request_started_us = esp_timer_get_time();
    s_http_open_failed = false;
    if (wr_cancelled()) { s_error_message = "操作已取消"; return NULL; }
    if (!wr_online()) { http_keepalive_drop(); s_error_message = "Wi-Fi 未连接或网络尚未就绪"; return NULL; }
    if (url[0] == '/') restore_session_cookie();
    wr_http_context_t *request = heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) { s_error_message = "内存不足，无法创建微信读书请求"; return NULL; }
    char *absolute = request->absolute;
    const bool service = url[0] == '/';
    request->capture_service_cookies = service;
    bool upgrade = !service && !strncmp(url, "http://", 7);
    const char *prefix = service ? WR_HOST : upgrade ? "https://" : "";
    int url_size = snprintf(absolute, WR_HTTP_URL_BYTES, "%s%s", prefix, upgrade ? url + 7 : url);
    if (url_size < 0 || url_size >= WR_HTTP_URL_BYTES) {
        s_error_message = "微信读书请求地址过长";
        free(request);
        return NULL;
    }
    if (!service && !wr_url_allowed(absolute, false)) {
        log_rejected_image_url(absolute);
        s_error_message="微信读书图片地址不受支持";
        free(request);
        return NULL;
    }
    for (const char *p = absolute; *p; ++p) if ((unsigned char)*p <= ' ' || *p == '\\') { free(request); return NULL; }
    cookie_lock();
    memcpy(request->cookie_snapshot, s_cookies, sizeof(request->cookie_snapshot));
    request->cookie_snapshot[sizeof(request->cookie_snapshot)-1] = 0;
    cookie_unlock();
    /* Keep the proven open/fetch/read path for complete responses. The experimental
     * esp_http_client_perform() keep-alive path returned non-2xx during a real
     * WeRead cache request; do not reuse it until the server response is diagnosed. */
    http_keepalive_drop();
    esp_http_client_config_t config = {
        /* Keep the HTTP/TLS working set modest on S31: the panel and Wi-Fi
         * driver need internal RAM while mbedTLS allocates AES/GCM contexts. */
        .url = absolute, .timeout_ms = 15000, .buffer_size = 4096, .buffer_size_tx = 2048,
        /* open/fetch_headers/read is the streaming API; it needs an explicit
         * redirect loop because perform() is not used here. */
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .user_agent = WR_AGENT, .event_handler = capture_cookie, .user_data = request,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) { s_error_message = "无法创建网络请求"; free(request); return NULL; }
    esp_http_client_set_header(http, "Accept-Encoding", "identity");
    esp_http_client_set_header(http, "Referer", referer ? referer : WR_HOST "/");
    if (service) {
        esp_http_client_set_header(http,"Accept","application/json, text/plain, */*");
        esp_http_client_set_header(http,"Origin",WR_HOST);
        if (request->cookie_snapshot[0]) esp_http_client_set_header(http,"Cookie",request->cookie_snapshot);
    }
    if (body) {
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_header(http, "Content-Type", "application/json;charset=UTF-8");
    }
    char *data = NULL;
    size_t used = 0, capacity = 0;
    int64_t scan_wait_until = 0;
    int64_t reader_deadline = chunk && s_processing_report ? esp_timer_get_time() + 90000000 : 0;
    bool complete = false;
    bool allocation_failed = false, read_failed = false;
    // 提前声明：open 失败会 goto cleanup，那里要读 code 决定提示语。
    // Declared up front: the open failure jumps to cleanup, which reads code to pick a message.
    int code = 0;
    int body_size = body ? (int)strlen(body) : 0;
    int64_t declared = -1;
    snprintf(request->current, sizeof(request->current), "%s", absolute);
    for (unsigned redirects = 0;;) {
        request->location[0]=0;
        int64_t open_start = esp_timer_get_time();
        esp_err_t open_error = esp_http_client_open(http, body_size);
        if (open_error != ESP_OK) {
            s_http_open_failed = true;
            s_error_message = "微信读书网络连接失败，请检查 Wi-Fi 后重试";
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
            char log_url[256];
            wr_http_log_target(request->current, log_url, sizeof(log_url));
            ESP_LOGW("WEREAD", "HTTP open failed url=%s err=%s ms=%lld tls=%s code=%d flags=0x%x internal=%u largest=%u",
                     log_url, esp_err_to_name(open_error), (long long)((esp_timer_get_time()-open_start)/1000),
                     esp_err_to_name(tls_error), tls_code, (unsigned)tls_flags,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            goto cleanup;
        }
        if (post_started && body_size) *post_started = true;
        for (int sent = 0; sent < body_size;) {
            int n = esp_http_client_write(http, body + sent, body_size - sent);
            if (n <= 0) { s_error_message = "网络请求发送失败"; goto cleanup; }
            sent += n;
        }
        declared = esp_http_client_fetch_headers(http);
        code = esp_http_client_get_status_code(http);
        char log_url[256];
        wr_http_log_target(request->current, log_url, sizeof(log_url));
        ESP_LOGI("WEREAD", "HTTP %s code=%d declared=%lld chunked=%d", log_url, code, (long long)declared,
                 (int)esp_http_client_is_chunked_response(http));
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        char *redirected = request->redirected;
        if (!redirect) break;
        if (redirects >= 5) { s_error_message="微信读书封面重定向次数过多"; goto cleanup; }
        if (!service) {
            if (!cover_redirect_url(request->current,request->location,redirected)) {
                char log_location[256];
                wr_http_log_target(request->location, log_location, sizeof(log_location));
                ESP_LOGW("WEREAD","cover redirect invalid location='%s'",log_location);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
            if (!wr_url_allowed(redirected,false) || esp_http_client_set_url(http,redirected)!=ESP_OK) {
                char log_location[256], log_target[256];
                wr_http_log_target(request->location, log_location, sizeof(log_location));
                wr_http_log_target(redirected, log_target, sizeof(log_target));
                ESP_LOGW("WEREAD","cover redirect target rejected location='%s' target='%s'",log_location,log_target);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
        } else {
            if (esp_http_client_set_redirection(http) != ESP_OK ||
                esp_http_client_get_url(http,redirected,WR_HTTP_URL_BYTES) != ESP_OK) {
                s_error_message="微信读书接口重定向失败";
                goto cleanup;
            }
        }
        if (!wr_url_allowed(redirected, service)) {
            char log_target[256];
            wr_http_log_target(redirected, log_target, sizeof(log_target));
            ESP_LOGW("WEREAD","redirect target rejected service=%d target='%s'",service,log_target);
            s_error_message = "微信读书图片重定向地址不受支持";
            goto cleanup;
        }
        char log_from[256], log_to[256];
        wr_http_log_target(request->current, log_from, sizeof(log_from));
        wr_http_log_target(redirected, log_to, sizeof(log_to));
        ESP_LOGI("WEREAD", "HTTP redirect %s -> %s", log_from, log_to);
        snprintf(request->current,sizeof(request->current),"%s",redirected);
        if (code == 301 || code == 302 || code == 303) {
            body_size = 0;
            esp_http_client_set_method(http, HTTP_METHOD_GET);
        }
        esp_http_client_close(http);
        ++redirects;
    }
    if (service && (code == 401 || code == 403)) { s_expired = true; s_error_message = "登录已失效，请重新扫码"; }
    else if (code < 200 || code >= 300) s_error_message = "微信读书接口返回错误";
    if ((declared < 0 && !esp_http_client_is_chunked_response(http)) ||
        (declared > 0 && (uint64_t)declared > limit) || code != 200) goto cleanup;
    if (declared < 0) declared = 0;
    while (!wr_cancelled()) {
        if (chunk) {
            if (reader_deadline && esp_timer_get_time() >= reader_deadline) {
                ESP_LOGW("WEREAD", "report reader response deadline bytes=%u", (unsigned)used);
                goto cleanup;
            }
            if (s_processing_report && report_display_busy()) {
                if (!scan_wait_until) scan_wait_until = esp_timer_get_time() + 3000000;
                if (esp_timer_get_time() >= scan_wait_until) {
                    ESP_LOGI("WEREAD", "reader fields deferred during display scan bytes=%u", (unsigned)used);
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            scan_wait_until = 0;
            if (!data) {
                data = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!data) { allocation_failed = true; goto cleanup; }
            }
            size_t remaining = limit - used;
            if (!remaining) break;
            int n = esp_http_client_read(http, data, (int)(remaining < 2048 ? remaining : 2048));
            if (n < 0) { read_failed = true; goto cleanup; }
            used += (size_t)n;
            if (n && chunk(data, (size_t)n, context)) {
                *fields_received = true;
                break;
            }
            if (!n || esp_http_client_is_complete_data_received(http)) {
                complete = esp_http_client_is_complete_data_received(http);
                break;
            }
            if (s_processing_report) vTaskDelay(1);
            continue;
        }
        if (used == capacity) {
            if (used == limit) { complete = esp_http_client_is_complete_data_received(http); break; }
            size_t next = capacity ? capacity * 2 : declared ? (size_t)declared : 4096;
            if (next > limit) next = limit;
            char *grown = heap_caps_realloc(data, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { allocation_failed = true; goto cleanup; }
            data = grown;
            capacity = next;
        }
        int n = esp_http_client_read(http, data + used, (int)(capacity - used));
        if (n < 0) { read_failed = true; goto cleanup; }
        used += (size_t)n;
        if (!n || esp_http_client_is_complete_data_received(http)) {
            complete = esp_http_client_is_complete_data_received(http);
            break;
        }
    }
cleanup:
    esp_http_client_cleanup(http);
    free(request);
    if (chunk && (complete || *fields_received) && !wr_cancelled()) {
        *fields_received = true;
        ESP_LOGI("WEREAD", "reader HTML bytes=%u full_response=%d", (unsigned)used, complete);
        free(data);
        return NULL;
    }
    if (!complete || !data || wr_cancelled()) {
        if (allocation_failed) s_error_message = "内存不足，无法接收微信读书响应";
        else if (read_failed) s_error_message = "读取微信读书响应失败";
        else if (wr_cancelled()) s_error_message = "操作已取消";
        else if (code != 200) { /* Preserve the more specific HTTP/authentication message. */ }
        else s_error_message = "微信读书响应未完整接收";
        char log_url[256];
        wr_http_log_target(request->current, log_url, sizeof(log_url));
        ESP_LOGW("WEREAD", "HTTP read failed url=%s complete=%d bytes=%u", log_url, (int)complete, (unsigned)used);
        free(data); return NULL;
    }
    data[used] = 0;
    char log_url[256];
    wr_http_log_target(request->current, log_url, sizeof(log_url));
    ESP_LOGI("WEREAD", "HTTP complete url=%s bytes=%u elapsed_ms=%lld", log_url, (unsigned)used,
             (long long)((esp_timer_get_time()-request_started_us)/1000));
    if (length) *length = used;
    return data;
}

static char *http_request_with_session(const char *url, const char *body, const char *referer, size_t limit,
                                        size_t *length, wr_http_chunk_fn chunk, void *context,
                                        bool *fields_received, bool *post_started) {
    const bool renewal = url && !strcmp(url, "/web/login/renewal");
    const bool service = url && url[0] == '/' && !renewal;
    char *data = http_request(url, body, referer, limit, length, chunk, context,
                              fields_received, post_started);
    if (service && data && http_body_session_expired(data)) {
        free(data);
        data = NULL;
        s_expired = true;
        s_error_message = "登录已失效，请重新扫码";
    }
    bool streamed = fields_received && *fields_received;
    if (data || streamed || !service || !s_expired || wr_cancelled()) return data;

    /* A 401/403 or explicit -2012 response is a confirmed rejection, so retrying
     * this POST cannot duplicate an accepted report. Clear the first attempt marker. */
    if (post_started) *post_started = false;
    if (fields_received) *fields_received = false;
    if (!renew_session()) return NULL;
    data = http_request(url, body, referer, limit, length, chunk, context,
                        fields_received, post_started);
    if (service && data && http_body_session_expired(data)) {
        s_expired = true;
        s_error_message = "登录已失效，请重新扫码";
    }
    return data;
}

char *wr_http(const char *url, const char *body, const char *referer, size_t limit, size_t *length) {
    return http_request_with_session(url, body, referer, limit, length, NULL, NULL, NULL, NULL);
}

bool wr_http_reader_fields(const char *url, size_t limit, wr_http_chunk_fn chunk, void *context) {
    bool received = false;
    http_request_with_session(url, NULL, WR_HOST "/", limit, NULL, chunk, context, &received, NULL);
    return received;
}

char *wr_http_report(const char *body, const char *referer, bool *post_started) {
    return http_request_with_session("/web/book/read", body, referer, 16384, NULL, NULL, NULL, NULL, post_started);
}

static void *json_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static bool renew_session(void) {
    if (!session_cookie_has("wr_rt")) {
        ESP_LOGW("WEREAD", "session renewal unavailable refresh_cookie=0");
        return false;
    }
    static const char body[] = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    ESP_LOGI("WEREAD", "session renewal start refresh_cookie=1");
    s_expired = false;
    char *raw = wr_http("/web/login/renewal", body, NULL, 16384, NULL);
    if (!raw) {
        ESP_LOGW("WEREAD", "session renewal network/http failure expired=%d", s_expired);
        return false;
    }
    cJSON *json = cJSON_ParseWithOpts(raw, NULL, true);
    free(raw);
    if (!json) {
        s_expired = true;
        ESP_LOGW("WEREAD", "session renewal response parse failed");
        return false;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsObject(data)) data = json;
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(data, "succeed");
    if (!success) success = cJSON_GetObjectItemCaseSensitive(json, "succeed");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
    if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
    const bool no_error = !error || (cJSON_IsNumber(error) && error->valueint == 0) ||
        (cJSON_IsString(error) && !strcmp(error->valuestring, "0"));
    const bool succeeded = cJSON_IsTrue(success) || (cJSON_IsNumber(success) && success->valueint == 1) ||
        (cJSON_IsString(success) && (!strcmp(success->valuestring, "1") || !strcmp(success->valuestring, "true")));
    bool ok = no_error && succeeded;
    cJSON_Delete(json);
    if (!ok) {
        s_expired = true;
        ESP_LOGW("WEREAD", "session renewal rejected");
        return false;
    }
    /* Report-worker renewals run outside run_work(), where s_http_account is
     * intentionally cleared. Reload the account snapshot so a rotated skey is
     * persisted there too, not only in the cookie blob. */
    wr_account_t updated = s_http_account;
    if (!updated.owner[0]) (void)read_account(&updated);
    char token[sizeof(updated.token)];
    if (updated.owner[0] && session_cookie_copy("wr_skey", token, sizeof(token)) &&
        strcmp(updated.token, token)) {
        snprintf(updated.token, sizeof(updated.token), "%s", token);
        if (save_account(&updated)) s_http_account = updated;
    }
    s_expired = false;
    ESP_LOGI("WEREAD", "session renewal confirmed refresh_cookie=%d", session_cookie_has("wr_rt"));
    return true;
}

static cJSON *wr_json_limit(const char *url, const char *body, size_t limit) {
    char *data = wr_http(url, body, NULL, limit, NULL);
    if (!data) return NULL;
    // cJSON hooks are global; the other firmware clients can parse concurrently.
    cJSON *json = cJSON_ParseWithOpts(data, NULL, true);
    free(data);
    if (!json) { s_error_message = "书架数据解析失败"; return NULL; }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
    if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
    if ((cJSON_IsNumber(error) && error->valueint == -2012) ||
        (cJSON_IsString(error) && !strcmp(error->valuestring, "-2012"))) s_expired = true;
    return json;
}

cJSON *wr_json(const char *url, const char *body) {
    return wr_json_limit(url, body, 3 * 1024 * 1024);
}

static FILE *open_shelf(const char *owner, wr_shelf_header_t *header) {
    if (!read_pico_sd_io_ready() || !owner[0]) return NULL;
    FILE *file = fopen(WR_INDEX, "rb");
    if (!file) file = fopen(WR_DIR "/shelf.prev", "rb");
    if (file && (fread(header, sizeof(*header), 1, file) != 1 ||
                 strncmp(header->owner, owner, sizeof(header->owner)) || header->count > WR_MAX_SHELF)) {
        fclose(file);
        file = NULL;
    }
    return file;
}

static bool owned_path(const char *owner, const char *id, char path[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(id)) return false;
    wr_account_t account;
    if (!read_account(&account) || strcmp(account.owner,owner)) return false;
    // 直接落在书架根目录：放子目录会被扫描登记成「分类」，缓存完还要多点一层。
    // Land on the shelf root: a subdirectory turns into a shelf category and hides the book.
    const char *root = app_settings_books_dir();
    int n = snprintf(path, WR_PATH, "%s/wr-%08lx+%s.epub", root, (unsigned long)account.library, id);
    return n > 0 && n < WR_PATH;
}

static FILE *open_cached(const char *owner, const char *mode) {
    FILE *file = fopen(WR_CACHED,mode);
    if (!file) return NULL;
    wr_account_t account;
    uint32_t library;
    if (read_account(&account) && !strcmp(account.owner,owner) &&
        fread(&library,sizeof(library),1,file) == 1 && library == account.library) return file;
    fclose(file);
    return NULL;
}

static bool cached_record(const char *owner, const char *id) {
    FILE *file = open_cached(owner,"rb");
    if (!file) return false;
    wr_book_t item;
    bool found = false;
    for (unsigned i = 0; i < WR_MAX_SHELF && fread(&item,sizeof(item),1,file) == 1; ++i) {
        if (!strncmp(item.id,id,sizeof(item.id))) { found = true; break; }
    }
    fclose(file);
    return found;
}

static bool remember_cached(const wr_book_t *book) {
    if (cached_record(s_http_account.owner,book->id)) return true;
    FILE *file = open_cached(s_http_account.owner,"r+b");
    bool ok = true;
    if (!file) {
        file = fopen(WR_CACHED,"w+b");
        ok = file && fwrite(&s_http_account.library,sizeof(s_http_account.library),1,file) == 1;
    }
    ok = ok && !fseek(file,0,SEEK_END) && fwrite(book,sizeof(*book),1,file) == 1 && !fflush(file) && !fsync(fileno(file));
    if (file && fclose(file)) ok = false;
    return ok;
}

bool wr_book_path(const wr_book_t *book, char path[WR_PATH]) {
    char owner[WR_ID];
    wr_account(owner, NULL, NULL);
    if (!owned_path(owner, book->id, path)) return false;
    WR_SD_SCOPE(path);
    struct stat st;
    return scope.allowed && cached_record(owner,book->id) && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int visible_shelf(int requested, wr_book_t *book) {
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) return 0;
    char owner[WR_ID];
    bool logged = wr_account(owner, NULL, NULL);
    wr_shelf_header_t header;
    FILE *file = logged ? open_shelf(owner,&header) : open_cached(owner,"rb");
    if (!file) return 0;
    int result = 0;
    if (logged && requested < 0) result = (int)header.count;
    else if (logged && requested < (int)header.count &&
             !fseek(file, (long)(sizeof(header) + (size_t)requested*sizeof(*book)), SEEK_SET)) {
        result = fread(book, sizeof(*book), 1, file) == 1;
    } else if (!logged) {
        wr_book_t record;
        char path[WR_PATH];
        struct stat st;
        bool found = false;
        for (unsigned i = 0; i < WR_MAX_SHELF && fread(&record, sizeof(record), 1, file) == 1; ++i) {
            if (!owned_path(owner, record.id, path) || stat(path, &st) || st.st_size <= 0) continue;
            if (requested == result) { *book = record; found = true; break; }
            ++result;
        }
        if (requested >= 0) result = found;
    }
    fclose(file);
    return result;
}

int wr_shelf_count(void) { return visible_shelf(-1, NULL); }
bool wr_shelf_get(int index, wr_book_t *book) { return index >= 0 && visible_shelf(index, book) == 1; }

static bool cover_path(const char *owner, const wr_book_t *book, char out[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(book->id)) return false;
    return snprintf(out, WR_PATH, WR_DIR "/%s+%s.gray", owner, book->id) < WR_PATH;
}

bool wr_cover_get(const wr_book_t *book, uint8_t *pixels) {
    char owner[WR_ID], path[WR_PATH];
    wr_account(owner, NULL, NULL);
    if (!cover_path(owner, book, path)) return false;
    WR_SD_SCOPE(path);
    if (!scope.allowed) return false;
    struct stat st;
    if (stat(path, &st) || st.st_size != BOOK_COVER_W * BOOK_COVER_H) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(pixels, BOOK_COVER_W * BOOK_COVER_H, 1, file) == 1;
    fclose(file);
    return ok;
}

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path,&st)==0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (errno!=ENOENT) return false;
    for (unsigned attempt=0;attempt<3;++attempt) {
        if (mkdir(path,0775)==0) return true;
        int saved=errno;
        if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) return true;
        if (saved!=EBUSY && saved!=EAGAIN && saved!=EINTR && saved!=ENOENT) { errno=saved; return false; }
        vTaskDelay(pdMS_TO_TICKS(40u*(attempt+1)));
    }
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
}

static void remove_previews(const char *owner) {
    if (!wr_safe(owner)) return;
    DIR *dir = opendir(WR_DIR);
    if (!dir) return;
    char prefix[WR_ID+2], path[WR_PATH];
    size_t prefix_size = snprintf(prefix,sizeof(prefix),"%s+",owner);
    struct dirent *item;
    while ((item = readdir(dir))) {
        size_t n = strlen(item->d_name);
        if (n > prefix_size+5 && !memcmp(item->d_name,prefix,prefix_size) && !strcmp(item->d_name+n-5,".gray") &&
            snprintf(path,sizeof(path),WR_DIR "/%s",item->d_name) < sizeof(path)) unlink(path);
    }
    closedir(dir);
}

static bool write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    (void)setvbuf(file, NULL, _IOFBF, 4096);
    bool ok = fwrite(data, 1, size, file) == size && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (!ok) unlink(path);
    return ok;
}

static bool preview_from_epub(const char *path, uint8_t *pixels) {
    uint8_t *data = NULL;
    size_t size;
    bool png;
    bool ok = book_epub_cover_bounded(path,&data,&size,&png,2*1024*1024) == ESP_OK &&
        book_cover_thumbnail(data,size,png,pixels);
    free(data);
    return ok;
}

static void sync_covers(unsigned first, unsigned count) {
    uint8_t *pixels = json_alloc(BOOK_COVER_W * BOOK_COVER_H);
    if (!pixels) return;
    unsigned downloaded = 0;
    for (unsigned i = 0; i < count && !wr_cancelled(); ++i) {
        int64_t cover_start = esp_timer_get_time();
        wr_book_t book;
        char path[WR_PATH], url[sizeof(book.cover)+8];
        if (!wr_shelf_get((int)(first+i), &book)) break;
        if (!cover_path(s_http_account.owner, &book, path)) continue;
        struct stat st;
        if (!stat(path,&st) && st.st_size == BOOK_COVER_W * BOOK_COVER_H) {
            wr_work_progress(++downloaded,count);
            continue;
        }
        if (!stat(path,&st)) unlink(path); /* discard a partial file left by an interrupted write */
        // 已缓存的书先用本地 EPUB 封面，避免同步时重复请求同一本书的图片。
        // Cached books use their EPUB cover first, avoiding another image request during sync.
        char epub[WR_PATH];
        bool local = wr_book_path(&book,epub);
        int64_t local_start = esp_timer_get_time();
        bool ok = local && preview_from_epub(epub,pixels);
        int64_t local_ms = (esp_timer_get_time() - local_start) / 1000;
        if (!ok && local) ESP_LOGW("WEREAD","cached EPUB has no usable cover book=%s",book.id);
        snprintf(url, sizeof(url), "%s%s", !strncmp(book.cover, "//", 2) ? "https:" : "", book.cover);
        size_t length=0;
        char *image=NULL;
        int64_t net_start = esp_timer_get_time();
        for (unsigned attempt=0;!ok && book.cover[0] && attempt<3 && !image && !wr_cancelled();++attempt) {
            image=wr_http(url,NULL,NULL,4*1024*1024,&length);
            if (!image && attempt<2) vTaskDelay(pdMS_TO_TICKS(300u*(attempt+1)));
        }
        int64_t net_ms = (esp_timer_get_time() - net_start) / 1000;
        int64_t decode_start = esp_timer_get_time();
        if (!ok) ok = image && book_cover_thumbnail((uint8_t *)image,length,length>=8 && (uint8_t)image[0]==0x89,pixels);
        int64_t decode_ms = (esp_timer_get_time() - decode_start) / 1000;
        int64_t write_start = esp_timer_get_time();
        if (ok) {
            char temp[WR_PATH];
            int n=snprintf(temp,sizeof(temp),"%s.part",path);
            ok=n>0 && (size_t)n<sizeof(temp) && write_file(temp,pixels,BOOK_COVER_W*BOOK_COVER_H) &&
                (unlink(path)==0 || errno==ENOENT) && rename(temp,path)==0;
            if (!ok && n>0 && (size_t)n<sizeof(temp)) unlink(temp);
        }
        int64_t write_ms = (esp_timer_get_time() - write_start) / 1000;
        ESP_LOGI("WEREAD", "cover stage book=%s local_ms=%lld net_ms=%lld bytes=%u decode_ms=%lld write_ms=%lld total_ms=%lld result=%d",
                 book.id, (long long)local_ms, (long long)net_ms, (unsigned)length,
                 (long long)decode_ms, (long long)write_ms,
                 (long long)((esp_timer_get_time()-cover_start)/1000), ok);
        if (!ok) {
            char log_cover[256];
            wr_http_log_target(book.cover, log_cover, sizeof(log_cover));
            ESP_LOGW("WEREAD","cover unavailable book=%s url=%s error=%s",book.id,log_cover,wr_error_message());
        }
        else ++downloaded;
        free(image);
        wr_work_progress(downloaded, count);
    }
    free(pixels);
}

static bool sync_shelf(void) {
    /* Keep the shelf request independent from the small chapter JSON budget;
     * large libraries may exceed several megabytes.  The SAX adapter is being
     * introduced separately, so this compatibility path uses PSRAM-backed
     * storage with a generous firmware-wide ceiling. */
    cJSON *json = NULL;
    for (unsigned attempt = 0; attempt < 2 && !wr_cancelled(); ++attempt) {
        json = wr_json_limit("/web/shelf/sync", NULL, 16u * 1024u * 1024u);
        if (json || !s_http_open_failed || attempt != 0 || wr_cancelled()) break;
        ESP_LOGW("WEREAD", "shelf sync connect failed before HTTP response; retrying once");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!json) return false;
    cJSON *books = cJSON_GetObjectItemCaseSensitive(json, "books");
    if (!cJSON_IsArray(books) && cJSON_IsObject(json)) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data)) books = cJSON_GetObjectItemCaseSensitive(data, "books");
    }
    ESP_LOGI("WEREAD", "sync response json=%d books=%d", cJSON_IsObject(json), cJSON_IsArray(books));
    int count = cJSON_GetArraySize(books);
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) { s_error_message = "SD 卡不可用"; cJSON_Delete(json); return false; }
    if (!cJSON_IsArray(books)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(json,"errcode");
        if (!error) error = cJSON_GetObjectItemCaseSensitive(json,"errCode");
        if (cJSON_IsNumber(error) && error->valueint == -2012) {
            s_expired = true; s_error_message = "登录已失效，请重新扫码";
        } else s_error_message = "书架响应中缺少书籍列表";
        cJSON_Delete(json); return false;
    }
    if (count > WR_MAX_SHELF) { s_error_message = "书架书籍数量超出支持范围"; cJSON_Delete(json); return false; }
    if (!ensure_directory(WR_DIR)) { s_error_message = "无法创建书架索引目录"; cJSON_Delete(json); return false; }
    wr_book_t *records = json_alloc((size_t)(count+1)*sizeof(*records));
    wr_shelf_header_t header = {0};
    snprintf(header.owner, sizeof(header.owner), "%s", s_http_account.owner);
    cJSON *entry;
    if (records) cJSON_ArrayForEach(entry, books) {
        wr_book_t *record = &records[header.count];
        memset(record, 0, sizeof(*record));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), record->id, sizeof(record->id)) || !wr_safe(record->id)) {
            continue;
        }
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "title"), record->title, sizeof(record->title));
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "author"), record->author, sizeof(record->author));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "cover"), record->cover, sizeof(record->cover))) record->cover[0] = 0;
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(entry, "readUpdateTime");
        record->updated = cJSON_IsNumber(date) && date->valuedouble >= 0 && date->valuedouble <= UINT32_MAX ? (uint32_t)date->valuedouble : 0;
        ++header.count;
    }
    cJSON_Delete(json);
    if (!records) { s_error_message = "内存不足，无法读取书架"; return false; }
    wr_work_progress(0, header.count);
    /* WeRead's response may include pinned/unfinished entries in a different
     * order from its visible recent-reading shelf. Sort by its update timestamp,
     * descending; stable ties retain server order. */
    for (unsigned i=1;i<header.count;++i) {
        wr_book_t value=records[i]; unsigned j=i;
        while (j && records[j-1].updated < value.updated) { records[j]=records[j-1]; --j; }
        records[j]=value;
    }
    bool ok = records && !wr_cancelled();
    if (ok) {
        FILE *file = fopen(WR_INDEX_TEMP, "wb");
        ok = file && fwrite(&header, sizeof(header), 1, file) == 1;
        for (unsigned i = 0; ok && i < header.count; ++i) ok = fwrite(&records[i], sizeof(*records), 1, file) == 1;
        if (file) {
            if (fflush(file) || fsync(fileno(file))) ok = false;
            if (fclose(file)) ok = false;
        }
        if (ok && !wr_cancelled()) {
            // FAT rename cannot replace an existing destination.
            const char *backup = WR_DIR "/shelf.prev";
            unlink(backup);
            bool moved = rename(WR_INDEX, backup) == 0;
            ok = (moved || access(WR_INDEX, F_OK) != 0) && rename(WR_INDEX_TEMP, WR_INDEX) == 0;
            if (ok) unlink(backup);
            else if (moved) rename(backup, WR_INDEX);
        } else ok = false;
        if (!ok) unlink(WR_INDEX_TEMP);
    }
    free(records);
    if (!ok && !wr_cancelled()) s_error_message = "书架索引写入 SD 卡失败";
    /* Cover network I/O belongs to explicit shelf sync only. Keep the reader
     * and shelf-entry paths local-only, and report successful cached covers. */
    if (ok && header.count) sync_covers(0, header.count);
    return ok;
}

static bool check_session(void) {
    int64_t started = esp_timer_get_time();
    char path[192];
    int path_size = snprintf(path, sizeof(path), "/web/shelf/sync?onlyBookid=1&cbcount=1&userVid=%s",
                             s_http_account.owner);
    if (path_size < 0 || (size_t)path_size >= sizeof(path) || !wr_safe(s_http_account.owner)) {
        s_error_message = "无法验证微信读书登录状态，请稍后重试";
        return false;
    }
    cJSON *response = wr_json(path, NULL);
    if (!response) return false;
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (!cJSON_IsObject(payload)) payload = response;
    const char *const index_keys[] = {"bookIds", "bookids", "books", "updated"};
    bool valid = false;
    for (size_t i = 0; i < sizeof(index_keys) / sizeof(index_keys[0]); ++i) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, index_keys[i]);
        if (cJSON_IsArray(index) || cJSON_IsObject(index)) { valid = true; break; }
    }
    cJSON_Delete(response);
    if (valid) {
        ESP_LOGI("WEREAD", "session check mode=light elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
        return true;
    }
    if (s_expired) return false;
    ESP_LOGW("WEREAD", "session check light response incompatible; falling back to full shelf");
    response = wr_json("/web/shelf/sync", NULL);
    if (!response) return false;
    const cJSON *books = cJSON_GetObjectItemCaseSensitive(response, "books");
    if (!cJSON_IsArray(books)) {
        payload = cJSON_GetObjectItemCaseSensitive(response, "data");
        books = cJSON_GetObjectItemCaseSensitive(payload, "books");
    }
    valid = cJSON_IsArray(books);
    cJSON_Delete(response);
    if (valid) ESP_LOGI("WEREAD", "session check mode=full elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
    else if (!s_expired) s_error_message = "无法验证微信读书登录状态，请稍后重试";
    return valid;
}

static bool login(void) {
    s_http_account.token[0] = 0;
    /* Start a new QR login without replaying a stale account's refresh cookie. */
    if (!clear_session_cookies()) {
        s_error_message = "无法清理旧登录态，请重试";
        return false;
    }
    cJSON *json = wr_json("/api/auth/getLoginUid", NULL);
    if (!json) return false;
    char uid[128], path[220];
    const cJSON *uid_field = cJSON_GetObjectItemCaseSensitive(json,"uid");
    if (!uid_field) uid_field = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"data"),"uid");
    bool ok = wr_value(uid_field, uid, sizeof(uid)) && wr_safe(uid);
    cJSON_Delete(json);
    if (!ok) return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_qr, sizeof(s_qr), WR_HOST "/web/confirm?uid=%s", uid);
    portEXIT_CRITICAL(&s_lock);
    snprintf(path, sizeof(path), "/api/auth/getLoginInfo?uid=%s&otp=", uid);
    for (int i = 0; i < 120 && !wr_cancelled(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        json = wr_json(path, NULL);
        if (!json) {
            if (s_expired) return false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (!cJSON_IsObject(data)) data = json;
        const cJSON *success = cJSON_GetObjectItemCaseSensitive(data, "succeed");
        if (!success) success = cJSON_GetObjectItemCaseSensitive(json, "succeed");
        if (cJSON_IsTrue(success) || (cJSON_IsNumber(success) && success->valueint == 1) ||
            (cJSON_IsString(success) && (!strcmp(success->valuestring,"1") || !strcmp(success->valuestring,"true")))) {
            wr_account_t account = {0};
            const cJSON *vid = cJSON_GetObjectItemCaseSensitive(data, "webLoginVid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "vid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "userVid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "user_vid");
            ok = wr_value(vid, account.owner, sizeof(account.owner)) && wr_safe(account.owner) &&
                wr_value(cJSON_GetObjectItemCaseSensitive(data, "accessToken"), account.token, sizeof(account.token)) &&
                wr_safe(account.token) && !wr_cancelled();
            account.library = strcmp(account.owner,s_http_account.owner) ? esp_random() : s_http_account.library;
            if (ok) ok = save_account(&account);
            cJSON_Delete(json);
            if (!ok) return false;
            if (strcmp(account.owner, s_http_account.owner)) {
                WR_SD_SCOPE(WR_DIR);
                if (scope.allowed) {
                    // 只清「微信读书书架」的索引；旧书转普通本地书，正文与封面都留下。
                    // Drop only the WeRead shelf index: the books become plain local books,
                    // so the EPUB and its cached cover stay on the card.
                    unlink(WR_INDEX);
                    unlink(WR_DIR "/shelf.prev");
                    unlink(WR_CACHED);
                }
                // 换账号：旧会话立即失效，避免把旧账号的时长记到新账号。
                // Account switched: retire the old session so its time can never bill the new one.
                radio_hold(false);
                memset(&s_reading, 0, sizeof(s_reading));
            }
            s_http_account = account;
            return true;
        }
        cJSON_Delete(json);
    }
    return false;
}

bool wr_logout(bool remove_books) {
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) return false;
    wr_account_t account;
    if (!read_account(&account)) return true;
    if (remove_books) {
        WR_SD_SCOPE(WR_DIR);
        if (!scope.allowed || !wr_safe(account.owner)) return false;
        char path[WR_PATH];
        FILE *files = open_cached(account.owner,"rb");
        bool ok = true;
        wr_book_t item;
        while (files && fread(&item,sizeof(item),1,files) == 1) {
            if (!owned_path(account.owner,item.id,path) || (unlink(path) && errno != ENOENT)) { ok = false; break; }
            (void)book_progress_forget(path);
            (void)book_title_clear(path);
        }
        if (files) { ok = ok && !ferror(files); fclose(files); }
        if (!ok) return false;
        remove_previews(account.owner);
        if (unlink(WR_INDEX) && errno != ENOENT) return false;
        if (unlink(WR_DIR "/shelf.prev") && errno != ENOENT) return false;
        if (unlink(WR_CACHED) && errno != ENOENT) return false;
    }
    memset(account.token, 0, sizeof(account.token));
    if (!save_account(&account)) return false;
    return clear_session_cookies();
}

static void run_work(const wr_work_t *work) {
    char path[WR_PATH];
    const int64_t work_started_us = esp_timer_get_time();
    bool ok = false;
    const bool cache_radio_boost = work->kind == WR_CACHE && wr_online();
    if (cache_radio_boost) {
        radio_hold(true);
        ESP_LOGI("WEREAD", "cache radio performance mode enabled");
    }
    if (work->kind == WR_LOGIN) s_report_fields.expires = 0;
    ESP_LOGI("WEREAD", "work start kind=%d internal_free=%u largest=%u", work->kind,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_expired = false;
    s_error_message = "操作失败，请检查网络后重试";
    read_account(&s_http_account);
    s_cookies[0] = 0;
    restore_session_cookie();
    if (!s_cookies[0] && s_http_account.token[0])
        snprintf(s_cookies,sizeof(s_cookies),"wr_vid=%s; wr_skey=%s",s_http_account.owner,s_http_account.token);
    ESP_LOGI("WEREAD", "session cookies restored vid=%d skey=%d refresh=%d",
             session_cookie_has("wr_vid"), session_cookie_has("wr_skey"), session_cookie_has("wr_rt"));
    if (time(NULL) < 1704067200 && wr_online()) {
        uint32_t utc;
        (void)read_pico_transfer_sync_time_online(&utc);
    }
    if (work->kind == WR_LOGIN) ok = login();
    else if (s_http_account.token[0] && !wr_cancelled()) {
        switch (work->kind) {
        case WR_CHECK: ok = check_session(); break;
        case WR_SYNC: ok = sync_shelf(); break;
        case WR_CACHE: {
            WR_SD_SCOPE(WR_DIR);
            if (!scope.allowed) { s_error_message="缓存失败：SD 卡当前不可用"; ok=false; }
            else if (!ensure_directory(WR_DIR)) { s_error_message="缓存失败：无法创建缓存目录"; ok=false; }
            else if (!owned_path(s_http_account.owner,work->book.id,path)) { s_error_message="缓存失败：书籍标识无效"; ok=false; }
            else if (!ensure_directory(app_settings_books_dir())) { s_error_message="缓存失败：无法创建书籍保存目录"; ok=false; }
            else ok=wr_cache(&work->book,path);
            if (!ok) ESP_LOGE("WEREAD","cache failed book=%s phase=download-or-package error=%s errno=%d",work->book.id,s_error_message,errno);
            if (ok) {
                ok = remember_cached(&work->book);
                if (ok && work->book.title[0]) (void)book_title_set(path,work->book.title);
                if (!ok) s_error_message="缓存失败：书籍已下载，但本地缓存索引写入失败";
            }
            break;
        }
        case WR_UPLOAD: case WR_DOWNLOAD: {
            const char *why = NULL;
            if (!wr_book_path(&work->book, path)) {
                s_error_message = "这本书还没有缓存，无法同步进度";
                break;
            }
            ok = wr_progress(&work->book, path, work->kind == WR_UPLOAD, &why);
            // 需求要求区分「没有本地进度」「无法定位章节」，不再统一报网络错误。
            if (!ok && why) s_error_message = why;
            break;
        }
        default: break;
        }
    } else if (!wr_cancelled()) {
        s_error_message = "登录已失效，请重新扫码";
        s_expired = true;
    }
    if (cache_radio_boost) {
        radio_hold(false);
        ESP_LOGI("WEREAD", "cache radio performance mode restored");
    }
    if (s_expired && s_http_account.token[0]) {
        memset(s_http_account.token, 0, sizeof(s_http_account.token));
        (void)save_account(&s_http_account);
    }
    memset(&s_http_account, 0, sizeof(s_http_account));
    memset(s_cookies, 0, sizeof(s_cookies));
    ESP_LOGI("WEREAD","work done kind=%d ok=%d canceled=%d elapsed_ms=%lld stack_hwm=%u internal_free=%u psram_free=%u",
             work->kind,ok,wr_cancelled(),(long long)((esp_timer_get_time()-work_started_us)/1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&s_lock);
    s_qr[0] = 0;
    s_result_message = s_error_message;
    s_result = s_expired ? WR_EXPIRED : wr_cancelled() ? WR_CANCELLED : ok ? WR_OK : WR_FAILED;
    portEXIT_CRITICAL(&s_lock);
}

void wr_work_progress(unsigned done, unsigned total) {
    portENTER_CRITICAL(&s_lock);
    s_done = done;
    s_total = total;
    portEXIT_CRITICAL(&s_lock);
}

bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count) {
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_worker && s_result != WR_BUSY;
    if (accepted) {
        s_work.kind = job;
        s_work.first = first;
        s_work.count = count;
        if (book) s_work.book = *book;
        else memset(&s_work.book, 0, sizeof(s_work.book));
        s_work_pending = true;
        atomic_store(&s_cancel, false);
        s_result = WR_BUSY;
        s_active_job = job;
        s_done = s_total = 0;
        s_qr[0] = 0;
    } else if (!s_worker) {
        s_result_message = "微信读书后台未初始化，请重启设备";
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) xTaskNotifyGive(s_worker);
    return accepted;
}

wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total) {
    portENTER_CRITICAL(&s_lock);
    wr_result_t result = s_result;
    if (qr && qr_size) {
        size_t n = strnlen(s_qr, qr_size-1);
        memcpy(qr, s_qr, n);
        qr[n] = 0;
    }
    if (done) *done = s_done;
    if (total) *total = s_total;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

bool wr_reporting_book(const char *path, wr_book_t *book) {
    char expected[WR_PATH], prefix[16];
    wr_account_t account;
    if (!read_account(&account) || !account.token[0]) return false;
    const char *base = strrchr(path, '/');
    if (!base) return false;
    snprintf(prefix, sizeof(prefix), "/wr-%08lx+", (unsigned long)account.library);
    if (strncmp(base, prefix, strlen(prefix))) return false;
    const char *id = base + strlen(prefix), *ext = strrchr(id, '.');
    if (!ext || strcmp(ext, ".epub") || ext == id || (size_t)(ext-id) >= WR_ID) return false;
    memset(book, 0, sizeof(*book));
    memcpy(book->id, id, ext-id);
    return owned_path(account.owner, book->id, expected) && !strcmp(expected, path) && cached_record(account.owner,book->id);
}

typedef enum { REPORT_RETRY, REPORT_CONFIRMED, REPORT_UNKNOWN, REPORT_ACCOUNT_CHANGED } report_result_t;

static bool same_report_book(const wr_report_t *a, const wr_report_t *b) {
    return a->library == b->library && !strcmp(a->owner, b->owner) && !strcmp(a->book_id, b->book_id);
}

static report_result_t run_report(const wr_report_t *job, const char **stage) {
    *stage = "account";
    if (!account_is(job->owner, job->library)) return REPORT_ACCOUNT_CHANGED;
    s_expired = false;
    *stage = "wifi";
    if (!wr_online()) return REPORT_RETRY;
    wr_chapter_t chapter;
    unsigned offset, pct;
    *stage = "get_progress";
    if (!wr_remote_position(job->book_id, &chapter, &offset, &pct)) return REPORT_RETRY;

    bool cached = s_report_fields.expires > esp_timer_get_time() &&
        same_report_book(job, &s_report_fields.account_book) && !strcmp(chapter.id, s_report_fields.chapter);
    ESP_LOGI("WEREAD", "report reader fields cache_hit=%d seconds=%u", cached, job->seconds);
    if (!cached) {
        *stage = "reader_fields";
        if (report_display_busy()) return REPORT_RETRY;
        s_report_fields.expires = 0;
        if (!wr_reader(job->book_id, chapter.id, &s_report_fields.fields)) return REPORT_RETRY;
        s_report_fields.account_book = *job;
        memcpy(s_report_fields.chapter, chapter.id, sizeof(chapter.id));
        s_report_fields.expires = esp_timer_get_time() + 300000000;
    }

    *stage = "check_progress";
    wr_chapter_t current;
    unsigned current_offset, current_pct;
    if (!wr_remote_position(job->book_id, &current, &current_offset, &current_pct) ||
        strcmp(current.id, chapter.id) || current_offset != offset || current_pct != pct)
        return REPORT_RETRY;
    if (!account_is(job->owner, job->library)) return REPORT_ACCOUNT_CHANGED;
    *stage = "submit";
    if (report_display_busy()) return REPORT_RETRY;
    bool post_started = false;
    if (!wr_send_duration(job->book_id, &chapter, offset, pct, job->seconds,
                          &s_report_fields.fields, &post_started)) {
        if (post_started) s_report_fields.expires = 0;
        return post_started ? REPORT_UNKNOWN : REPORT_RETRY;
    }
    if (account_is(job->owner, job->library) && !add_stats(job->owner, job->seconds))
        ESP_LOGW("WEREAD", "report confirmed but local stats write failed seconds=%u", job->seconds);
    return REPORT_CONFIRMED;
}

static void process_report(unsigned slot, const wr_report_t *job) {
    const char *stage;
    s_processing_report = true;
    report_result_t result = run_report(job, &stage);
    s_processing_report = false;
    memset(s_cookies, 0, sizeof(s_cookies));
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    wr_report_t *pending = &s_reports[slot];
    if (result == REPORT_RETRY) {
        if (pending->failures < 5) ++pending->failures;
        pending->retry_at = now + (2000000LL << pending->failures);
    } else {
        pending->seconds -= job->seconds;
        pending->retry_at = 0;
        pending->failures = 0;
        if (result == REPORT_ACCOUNT_CHANGED) pending->seconds = 0;
    }
    unsigned remaining = pending->seconds;
    portEXIT_CRITICAL(&s_lock);
    if (!persist_report_queue())
        ESP_LOGW("WEREAD", "persist report queue failed after result=%d", result);
    const char *outcome = result == REPORT_CONFIRMED ? "confirmed" :
        result == REPORT_RETRY ? "retry" : result == REPORT_UNKNOWN ? "unknown" : "account_changed";
    ESP_LOGI("WEREAD", "report result=%s stage=%s seconds=%u pending=%u",
             outcome, stage, job->seconds, remaining);
}

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int slot = -1;
        bool pending = false;
        wr_report_t report;
        portENTER_CRITICAL(&s_lock);
        bool work = s_work_pending;
        if (work) s_work_pending = false;
        if (!work) {
            bool reader_idle = !s_reader_scanning && (!s_reader_scan_end || now - s_reader_scan_end >= 1200000);
            for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i) {
                if (!s_reports[i].seconds) continue;
                pending = true;
                if (reader_idle && s_reports[i].retry_at <= now &&
                    (slot < 0 || s_reports[i].retry_at < report.retry_at)) {
                    slot = (int)i;
                    report = s_reports[i];
                }
            }
        }
        portEXIT_CRITICAL(&s_lock);

        if (work) {
            // 发布完成前，WR_BUSY 保证任务参数不能被覆盖。/ WR_BUSY protects the job until its result is published.
            run_work(&s_work);
            http_keepalive_drop();
        } else if (slot >= 0) {
            process_report((unsigned)slot, &report);
            http_keepalive_drop();
        } else {
            ulTaskNotifyTake(pdTRUE, pending ? pdMS_TO_TICKS(500) : portMAX_DELAY);
        }
    }
}

void wr_init(void) {
    if (s_worker) return;
    if (!s_cookie_mutex) s_cookie_mutex = xSemaphoreCreateMutexStatic(&s_cookie_mutex_storage);
    if (!s_cookie_mutex) {
        ESP_LOGE("WEREAD", "cookie mutex initialization failed");
        return;
    }
    if (!s_report_store_mutex)
        s_report_store_mutex = xSemaphoreCreateMutexStatic(&s_report_store_mutex_storage);
    if (s_report_store_mutex) load_report_queue();
    else ESP_LOGE("WEREAD", "report queue persistence mutex initialization failed");
    s_worker = xTaskCreateStatic(worker_task, "weread", sizeof(s_worker_stack),
                                 NULL, 2, s_worker_stack, &s_worker_tcb);
    if (s_worker) ESP_LOGI("WEREAD", "worker ready stack=%u", (unsigned)sizeof(s_worker_stack));
    else ESP_LOGE("WEREAD", "worker initialization failed");
}

void wr_reading_start(const char *path, bool enabled, int64_t now_ms) {
    wr_reading_stop(true);
    if (s_reading.ms >= 1000) {
        ESP_LOGW("WEREAD", "new reading timer deferred; previous report is still pending seconds=%u",
                 s_reading.ms / 1000);
        return;
    }
    if (!enabled || !wr_reporting_book(path, &s_reading.book)) return;
    wr_account_t account;
    if (!read_account(&account) || !account.token[0]) { memset(&s_reading, 0, sizeof(s_reading)); return; }
    snprintf(s_reading.owner, sizeof(s_reading.owner), "%s", account.owner);
    s_reading.library = account.library;
    s_reading.attached = true;
    s_reading.last = now_ms;
    // 需求：这次阅读的上报期间保持 Wi-Fi 启用。
    radio_hold(true);
}

static bool report_reading(void) {
    const unsigned seconds = s_reading.ms / 1000;
    if (!seconds) return true;
    if (!s_reading.owner[0] || !s_worker) return false;
    wr_report_t job = { .library = s_reading.library, .seconds = seconds };
    memcpy(job.book_id, s_reading.book.id, sizeof(job.book_id));
    memcpy(job.owner, s_reading.owner, sizeof(job.owner));
    portENTER_CRITICAL(&s_lock);
    wr_report_t *empty = NULL, *entry = NULL;
    for (unsigned i = 0; i < WR_REPORT_COUNT; ++i) {
        if (!s_reports[i].seconds) {
            if (!empty) empty = &s_reports[i];
        } else if (same_report_book(&job, &s_reports[i])) {
            entry = &s_reports[i];
            break;
        }
    }
    bool accepted = false;
    if (entry && seconds <= UINT32_MAX - entry->seconds) {
        entry->seconds += seconds;
        accepted = true;
    } else if (empty) {
        *empty = job;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) return false;
    s_reading.ms %= 1000;
    if (!persist_report_queue())
        ESP_LOGW("WEREAD", "persist pending reading report failed seconds=%u", seconds);
    xTaskNotifyGive(s_worker);
    return true;
}

void wr_reading_tick(bool active, int64_t now_ms) {
    if (!s_reading.attached) {
        if (s_reading.ms >= 1000 && report_reading() && s_reading.ms < 1000)
            memset(&s_reading, 0, sizeof(s_reading));
        return;
    }
    if (!s_reading.last) { s_reading.last = now_ms; return; }
    int64_t elapsed = now_ms - s_reading.last;
    s_reading.last = now_ms;
    // 断网只停发送不停计时：连接恢复后把这段一起补上（需求 4「暂停发送，恢复后继续」）。
    if (active && elapsed > 0 && elapsed <= 5000) s_reading.ms += (unsigned)elapsed;
    if (!active) radio_hold(false);
    else if (wr_online()) radio_hold(true);
    if (s_reading.ms >= 60000) report_reading();
}

void wr_reading_stop(bool close) {
    bool queued = true;
    if (s_reading.attached || s_reading.ms >= 1000) queued = report_reading();
    if (close && s_reading.ms >= 1000 && !queued) {
        ESP_LOGW("WEREAD", "report queue full; retaining pending reading seconds=%u",
                 s_reading.ms / 1000);
        s_reading.attached = false;
        s_reading.last = 0;
        radio_hold(false);
        return;
    }
    radio_hold(false);
    s_reading.last = 0;
    if (close) memset(&s_reading, 0, sizeof(s_reading));
}
