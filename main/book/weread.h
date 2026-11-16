#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WR_ID 48
#define WR_PATH 288

typedef struct {
    char id[WR_ID], title[160], author[96], cover[384];
    uint32_t updated;
} wr_book_t;

typedef struct {
    char id[WR_ID], title[128], tar[256];
    unsigned index;
} wr_chapter_t;

typedef enum { WR_LOGIN, WR_SYNC, WR_CACHE, WR_UPLOAD, WR_DOWNLOAD, WR_REPORT, WR_CHECK } wr_job_t;
typedef enum { WR_IDLE, WR_BUSY, WR_OK, WR_FAILED, WR_EXPIRED, WR_CANCELLED } wr_result_t;

bool wr_online(void);
void wr_init(void);
bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total);
const char *wr_error_message(void);
const char *wr_result_message(void);
bool wr_logout(bool remove_books);
int wr_shelf_count(void);
bool wr_shelf_get(int index, wr_book_t *book);
bool wr_book_path(const wr_book_t *book, char path[WR_PATH]);
bool wr_cover_get(const wr_book_t *book, uint8_t *pixels);
bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count);
wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total);
void wr_cancel(void);
bool wr_reporting_book(const char *path, wr_book_t *book);
void wr_reading_start(const char *path, bool enabled, int64_t now_ms);
void wr_reading_tick(bool active, int64_t now_ms);
void wr_reading_stop(bool close);
void wr_reading_display(bool scanning);
