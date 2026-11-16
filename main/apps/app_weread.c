#include "app_weread.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "book_cover.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_root_style.h"
#include "ui_wifi_qr.h"
#include "weread.h"
#include "weread_progress_policy.h"

extern const app_desc_t app_book, app_transfer;

typedef enum { SHELF, ACCOUNT, ACTIONS, LOGOUT, ASK_WIFI, WORK } wr_view_t;
/* 书架槽位由样式决定：网格 9、列表 8。数组按上限开，用前先夹紧，改样式不会越界。 */
#define WR_SLOTS 9
static wr_view_t s_view;
static wr_job_t s_job, s_after_wifi;
static bool s_wait_wifi, s_wifi_open, s_from_home, s_back_to_weread, s_return_once;
static bool s_check_pending;
static int64_t s_session_verified_us;
static int s_page, s_count;
static unsigned s_selected_index;
static wr_book_t s_books[WR_SLOTS], s_selected;
static bool s_cached[WR_SLOTS], s_selected_cached, s_logged;
static int s_pressed = -1;          /* 0 同步 / 1 账户：右上控件的按下反馈 */
static int64_t s_pressed_until;
static int s_pending_action = -1;   /* 按下反馈画完后要执行的动作 */
static wr_job_t s_retry_job;        /* 失败后允许一键重试的动作 */
static bool s_has_retry;
static char s_open[WR_PATH], s_offline[WR_PATH], s_qr[180];
static const char *s_message;
static uint8_t *s_cover_pixels;
static unsigned s_done, s_total;

#define WR_SESSION_VERIFY_TTL_US (60LL * 1000LL * 1000LL)

static bool session_verified_recently(void) {
    int64_t verified = s_session_verified_us;
    return verified > 0 && esp_timer_get_time() - verified < WR_SESSION_VERIFY_TTL_US;
}

static void session_verification_result(wr_result_t status) {
    if (status == WR_OK) s_session_verified_us = esp_timer_get_time();
    else if (status == WR_EXPIRED) s_session_verified_us = 0;
}

static int pages(void) {
    int slots = app_book_shelf_slots();
    return s_count ? (s_count+slots-1)/slots : 1;
}

static void refresh_shelf(void) {
    s_logged = wr_account(NULL, NULL, NULL);
    s_count = wr_shelf_count();
    if (s_page >= pages()) s_page = pages()-1;
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i = 0; i < slots; ++i) {
        char path[WR_PATH];
        memset(&s_books[i], 0, sizeof(s_books[i]));
        s_cached[i] = wr_shelf_get(s_page*slots+i, &s_books[i]) && wr_book_path(&s_books[i], path);
    }
}

static void connect_wifi(app_ctx_t *ctx) {
    app_transfer_request_wifi_then_return();
    ctx->request_app = &app_transfer;
}

static void open_book(app_ctx_t *ctx, bool offline) {
    if (offline) snprintf(s_offline, sizeof(s_offline), "%s", s_open);
    if (s_from_home ? app_book_request_open_from_home(s_open) : app_book_request_open(s_open)) {
        s_return_once = s_back_to_weread;
        ctx->request_app = &app_book;
    }
    s_open[0] = 0;
    s_view = SHELF;
}

bool app_weread_take_offline(const char *path) {
    if (!s_offline[0] || strcmp(s_offline, path)) return false;
    s_offline[0] = 0;
    return true;
}

bool app_weread_take_return(void) {
    bool back = s_return_once;
    s_return_once = false;
    return back;
}

bool app_weread_confirm_open(app_ctx_t *ctx, const char *path, bool from_home) {
    wr_book_t book;
    if (wr_online() || !wr_reporting_book(path, &book)) return false;
    snprintf(s_open, sizeof(s_open), "%s", path);
    s_from_home = from_home;
    s_back_to_weread = false;
    s_view = ASK_WIFI;
    ctx->request_app = &app_weread;
    return true;
}

static app_redraw_t start(app_ctx_t *ctx, wr_job_t job) {
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) {
        s_message = "请等待当前操作完成";
        return APP_REDRAW_PAGE;
    }
    if (!wr_online()) {
        s_wait_wifi = true;
        s_after_wifi = job;
        connect_wifi(ctx);
        return APP_REDRAW_NONE;
    }
    s_message = NULL;
    s_has_retry = false;
    s_job = job;
    s_done = s_total = 0;
    s_qr[0] = 0;
    s_view = WORK;
    if (!wr_start(job, &s_selected, s_selected_index, 0)) {
        s_view = job == WR_LOGIN ? ACCOUNT : SHELF;
        s_message = wr_result_message();
    }
    return APP_REDRAW_PAGE;
}

static void enter(app_ctx_t *ctx) {
    refresh_shelf();
    if (!s_cover_pixels) s_cover_pixels = heap_caps_malloc(BOOK_COVER_W*BOOK_COVER_H,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (s_wifi_open) {
        if (wr_online()) { s_wifi_open = false; open_book(ctx,false); return; }
        s_view = ASK_WIFI;
    } else if (s_wait_wifi) {
        s_wait_wifi = false;
        if (wr_online()) (void)start(ctx,s_after_wifi);
        else s_message = "Wi-Fi 未连接";
    } else if (s_logged && !session_verified_recently()) {
        s_check_pending = true;
        if (wr_online() && wr_status(NULL,0,NULL,NULL) != WR_BUSY) {
            s_check_pending = false;
            (void)start(ctx,WR_CHECK);
        }
    }
}

static app_redraw_t nav_reselect(app_ctx_t *ctx) {
    (void)ctx;
    s_page = 0;
    s_view = SHELF;
    s_message = NULL;
    s_has_retry = false;
    refresh_shelf();
    if (s_logged && !session_verified_recently()) {
        s_check_pending = true;
        if (wr_online() && wr_status(NULL,0,NULL,NULL) != WR_BUSY) {
            s_check_pending = false;
            return start(ctx,WR_CHECK);
        }
    }
    return APP_REDRAW_PAGE;
}

static void leave(app_ctx_t *ctx) {
    (void)ctx;
    if (s_view == WORK) { wr_cancel(); s_view = SHELF; }
    free(s_cover_pixels);
    s_cover_pixels = NULL;
}

static EpdRect action_rect(int i) { return (EpdRect){96, 390+i*99, 492, 76}; }
static EpdRect retry_rect(void) { return (EpdRect){190, 566, 300, 68}; }
/* 右上控件位置由书架统一给出，两个书架不再各存一份常量。 */
static EpdRect sync_rect(void) { return app_book_shelf_action_rect(false); }
static EpdRect account_rect(void) { return app_book_shelf_action_rect(true); }

static void shelf_title_fit(const char *src, char dst[160], int px, int width) {
    size_t used=0;
    dst[0]=0;
    while (src[used]) {
        unsigned char c=(unsigned char)src[used];
        size_t bytes=(c<0x80)?1:(c<0xe0)?2:(c<0xf0)?3:4;
        size_t remain=strlen(src+used);
        if (bytes>remain) bytes=1;
        size_t next=used+bytes;
        bool more=src[next]!=0;
        char trial[160];
        if (next+sizeof("…")>sizeof(trial)) break;
        memcpy(trial,src,next);
        if (more) memcpy(trial+next,"…",sizeof("…")); else trial[next]=0;
        if (ui_text_fixed_context_width_px(px,trial,src)>width) break;
        memcpy(dst,trial,strlen(trial)+1);
        used=next;
    }
    if (src[used] && !dst[0] && ui_text_fixed_context_width_px(px,"…",src)<=width)
        memcpy(dst,"…",sizeof("…"));
}

static void button(uint8_t *fb, EpdRect r, const char *label, bool enabled) {
    ui_fill_round_rect(fb,r,16,enabled?UI_GRAY_WHITE:0xe0);
    ui_draw_control_frame(fb,r,16,enabled?0x50:0xa0);
    if (enabled) ui_text_box(fb,(EpdRect){r.x+8,r.y+4,r.width-16,r.height-8},30,label,EPD_DRAW_ALIGN_CENTER,false);
    else ui_text_fixed_ink_vc(fb,r.x+r.width/2,r.y+r.height/2,ui_text_effective_px(30),label,EPD_DRAW_ALIGN_CENTER,0x80);
}

static void title(uint8_t *fb, const char *label) {
    ui_text_box(fb,(EpdRect){36,86,370,70},48,label,EPD_DRAW_ALIGN_LEFT,false);
}

static void draw_cover(uint8_t *fb, const wr_book_t *book, EpdRect rect) {
    if (s_cover_pixels && wr_cover_get(book, s_cover_pixels)) {
        for (int y = 0; y < rect.height; ++y) {
            const uint8_t *row = s_cover_pixels+(size_t)(y*BOOK_COVER_H/rect.height)*BOOK_COVER_W;
            for (int x = 0; x < rect.width; ++x) epd_draw_pixel(rect.x+x,rect.y+y,row[x*BOOK_COVER_W/rect.width],fb);
        }
    } else {
        ui_fill_rect_fast(rect,0xe0,fb);
        ui_draw_icon(fb,rect.x+rect.width/2,rect.y+rect.height/2,rect.width/2,UI_ICON_BOOK_OPEN,0x60);
    }
    ui_draw_round_rect(fb,rect,0,UI_GRAY_BLACK);
}

static void shelf(uint8_t *fb) {
    title(fb,"微信读书");
    app_book_shelf_header(fb,false,"同步",s_pressed==0);
    app_book_shelf_header(fb,true,"账户",s_pressed==1);
    bool list = app_settings_shelf_style() == 5;
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i = 0; i < slots && s_books[i].id[0]; ++i) {
        EpdRect r = app_book_shelf_slot(i), cover = app_book_shelf_cover(i);
        const char *cached = s_cached[i] ? "已缓存" : "未缓存";
        char short_title[160];
        int title_px=ui_text_effective_px(24);
        if (title_px>30) title_px=30;
        shelf_title_fit(s_books[i].title,short_title,title_px,list?492:r.width-8);
        draw_cover(fb,&s_books[i],cover);
        if (list) {
            ui_text_fixed_context_vc(fb,114,r.y+27,title_px,short_title,s_books[i].title,EPD_DRAW_ALIGN_LEFT,false);
            ui_text_box(fb,(EpdRect){114,r.y+50,365,34},20,s_books[i].author,EPD_DRAW_ALIGN_LEFT,false);
            ui_text_box(fb,(EpdRect){479,r.y+50,150,34},20,cached,EPD_DRAW_ALIGN_RIGHT,false);
            ui_hairline(fb,r.y+r.height-1,r.x,r.width,0xc0);
        } else {
            ui_text_fixed_context_vc(fb,r.x+r.width/2,r.y+242,title_px,short_title,s_books[i].title,EPD_DRAW_ALIGN_CENTER,false);
            ui_fill_round_rect(fb,(EpdRect){cover.x+4,cover.y+4,cover.width-8,39},7,0xf0);
            ui_text_box(fb,(EpdRect){cover.x+6,cover.y+5,cover.width-12,37},18,cached,EPD_DRAW_ALIGN_CENTER,false);
        }
    }
    app_book_shelf_furniture(fb);
    if (!s_count) ui_text_vc(fb,342,430,28,s_logged?"点击同步获取微信读书书架":"请在账户中扫码登录",EPD_DRAW_ALIGN_CENTER,false);
    char footer[64];
    snprintf(footer,sizeof(footer),"%d 本 · 第 %d / %d 页",s_count,s_page+1,pages());
    ui_pager_draw(fb,footer,s_page,pages(),false,UI_PAGER_NONE);
}

static bool action_enabled(int action) {
    static const bool cache_required[] = {true, false, true, true, false};
    if (cache_required[action] && !s_selected_cached) return false;
    return action == 0 || action == 4 || (s_logged && (action != 1 || !s_selected_cached));
}

static const EpdRect WORK_PROGRESS_AREA = {40,460,604,112};

static void draw_work_progress_area(uint8_t *fb) {
    ui_fill_rect_fast(WORK_PROGRESS_AREA, UI_GRAY_WHITE, fb);
    if (s_total || s_job == WR_SYNC) {
        char progress[64];
        if (s_job == WR_SYNC && !s_total)
            snprintf(progress,sizeof(progress),"正在连接并获取书架数据…");
        else if (s_job == WR_SYNC)
            snprintf(progress,sizeof(progress),"%u / %u 本封面已下载",s_done,s_total);
        else
            snprintf(progress,sizeof(progress),"%u / %u",s_done,s_total);
        EpdRect bar = {122,482,440,24};
        ui_fill_round_rect(fb,bar,12,0xd8);
        unsigned pct = !s_total ? 0 : s_done >= s_total ? 100 :
                       (unsigned)(((uint64_t)s_done*100u)/s_total);
        int fill = (int)((bar.width-4)*pct/100u);
        if (fill > 0)
            ui_fill_round_rect(fb,(EpdRect){bar.x+2,bar.y+2,fill,bar.height-4},10,0x50);
        ui_text_vc(fb,342,542,30,progress,EPD_DRAW_ALIGN_CENTER,false);
    }
}

static EpdRect work_progress_area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return WORK_PROGRESS_AREA;
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    if ((s_view == SHELF || s_view == ACTIONS) && app_settings_shelf_style() != 5) ui_root_background(fb);
    ui_nav_status(fb);
    if (s_view == SHELF || s_view == ACTIONS) shelf(fb);
    if (s_view == ACCOUNT || s_view == LOGOUT) {
        title(fb,"微信读书账户");
        uint32_t today;
        uint64_t total;
        bool logged = wr_account(NULL,&today,&total);
        char stats[96];
        ui_text_vc(fb,342,263,30,logged?"已登录":"未登录",EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"今日上报  %u 分 %u 秒",(unsigned)today/60,(unsigned)today%60);
        ui_text_vc(fb,342,351,27,stats,EPD_DRAW_ALIGN_CENTER,false);
        snprintf(stats,sizeof(stats),"累计上报  %llu 分 %llu 秒",(unsigned long long)(total/60),(unsigned long long)(total%60));
        ui_text_vc(fb,342,436,27,stats,EPD_DRAW_ALIGN_CENTER,false);
        button(fb,action_rect(2),logged?"退出登录":"扫码登录",true);
        button(fb,action_rect(3),"返回书架",true);
    }
    if (s_view == ACTIONS || s_view == ASK_WIFI || s_view == LOGOUT) {
        EpdRect box = {58,268,568,657};
        ui_fill_round_rect(fb,box,20,UI_GRAY_WHITE);
        ui_draw_control_frame(fb,box,20,0x50);
        const char *heading = s_view==ASK_WIFI?(s_wifi_open?"正在连接 Wi-Fi…":"是否启用阅读时长上报"):s_view==LOGOUT?"是否保留所有缓存书籍":s_selected.title;
        ui_text_box(fb,(EpdRect){83,298,518,75},32,heading,EPD_DRAW_ALIGN_CENTER,false);
        if (s_view == ACTIONS) {
            static const char *const actions[] = {"阅读书籍","缓存书籍","上传本地进度","下载云端进度","关闭"};
            for (int i = 0; i < 5; ++i) button(fb,action_rect(i),actions[i],action_enabled(i));
        } else if (s_view == ASK_WIFI) {
            button(fb,action_rect(0),"启用，连接 Wi-Fi",true);
            button(fb,action_rect(1),"不启用，继续阅读",true);
            button(fb,action_rect(2),"取消",true);
        } else {
            button(fb,action_rect(0),"保留并退出",true);
            button(fb,action_rect(1),"删除并退出",true);
            button(fb,action_rect(2),"取消",true);
        }
    }
    if (s_view == WORK) {
        title(fb,"微信读书");
        static const char *const labels[] = {"请用微信扫码登录","正在同步书架与封面","正在缓存图文","正在上传本地进度","正在下载云端进度","正在上报阅读时长","正在验证登录状态"};
        ui_text_vc(fb,342,228,29,labels[s_job],EPD_DRAW_ALIGN_CENTER,false);
        if (s_qr[0]) ui_wifi_qr_draw(fb,(EpdRect){137,300,410,410});
        draw_work_progress_area(fb);
        button(fb,action_rect(4),"取消",true);
    }
    /* Status messages are modal: keep them off the shelf artwork so covers and
     * text never show through the notification.  A tap dismisses the notice. */
    if (s_message) {
        EpdRect notice = {72,430,540,230};
        ui_fill_round_rect(fb,notice,22,UI_GRAY_WHITE);
        ui_draw_control_frame(fb,notice,22,0x50);
        ui_text_vc(fb,342,s_has_retry?468:480,27,"微信读书",EPD_DRAW_ALIGN_CENTER,false);
        // 失败时留一个重试入口：缓存中断后不必退出弹窗重来。
        ui_text_box(fb,(EpdRect){104,s_has_retry?496:520,476,s_has_retry?56:64},25,s_message,
                    EPD_DRAW_ALIGN_CENTER,false);
        if (s_has_retry) {
            button(fb,retry_rect(),"重试",true);
            ui_text_vc(fb,342,640,18,"轻点其它位置关闭",EPD_DRAW_ALIGN_CENTER,false);
        } else {
            ui_text_vc(fb,342,622,20,"轻点任意位置关闭",EPD_DRAW_ALIGN_CENTER,false);
        }
    }
    ui_nav_draw(fb,UI_TAB_WEREAD);
}

static app_redraw_t tick(app_ctx_t *ctx) {
    if (s_wifi_open && s_view == ASK_WIFI && wr_online()) {
        s_wifi_open = false;
        open_book(ctx,false);
        return APP_REDRAW_NONE;
    }
    if (s_logged && !wr_online()) {
        s_session_verified_us = 0;
        s_check_pending = true;
    }
    if (s_check_pending && s_view == SHELF && wr_online() &&
        wr_status(NULL,0,NULL,NULL) != WR_BUSY) {
        s_check_pending = false;
        return start(ctx,WR_CHECK);
    }
    if (s_pressed >= 0 && ctx->now_ms >= s_pressed_until) {
        // 按下反馈先画一帧，再执行动作，和本地书架头部按钮的手感一致。
        const int pending = s_pending_action;
        s_pressed = -1;
        s_pending_action = -1;
        if (pending == 0) {
            if (s_logged) return start(ctx,WR_SYNC);
            s_view = ACCOUNT;
            s_message = "请先登录";
            s_has_retry = false;
        } else if (pending == 1) {
            s_view = ACCOUNT;
            s_message = NULL;
            s_has_retry = false;
        }
        return APP_REDRAW_PAGE;
    }
    unsigned done, total;
    char qr[sizeof(s_qr)];
    wr_result_t status = wr_status(qr,sizeof(qr),&done,&total);
    if (s_view == WORK) {
        if (status != WR_BUSY) {
            ui_wifi_qr_clear();
            if (status == WR_OK && s_job == WR_LOGIN) {
                session_verification_result(status);
                s_view = ACCOUNT;
                refresh_shelf();
                return start(ctx,WR_SYNC);
            }
            session_verification_result(status);
            if (status == WR_OK && s_job == WR_CACHE) book_store_notify_changed();
            if (s_job == WR_CHECK) {
                s_view = status == WR_EXPIRED ? ACCOUNT : SHELF;
                s_message = status == WR_EXPIRED ? "登录已失效，请重新扫码" :
                            status == WR_OK ? NULL : wr_result_message();
                refresh_shelf();
                return APP_REDRAW_PAGE;
            }
            s_view = s_job == WR_LOGIN || status == WR_EXPIRED ? ACCOUNT : SHELF;
            s_message = status == WR_OK ? "操作完成" : status == WR_EXPIRED ? "登录已失效，请重新扫码" :
                        status == WR_CANCELLED ? "已取消" : wr_result_message();
            // 失败时给出重试入口；登录失败要重新扫码，不给一键重试。
            s_has_retry = status == WR_FAILED &&
                (s_job == WR_CACHE || s_job == WR_UPLOAD || s_job == WR_DOWNLOAD || s_job == WR_SYNC);
            if (s_has_retry) s_retry_job = s_job;
            refresh_shelf();
            return APP_REDRAW_PAGE;
        }
        if (strcmp(qr,s_qr)) {
            snprintf(s_qr,sizeof(s_qr),"%s",qr);
            if (*qr) (void)ui_wifi_qr_prepare_text(qr);
            return APP_REDRAW_PAGE;
        }
        if (weread_progress_should_redraw(s_done, s_total, done, total, s_job == WR_CACHE)) {
            s_done = done;
            s_total = total;
            ui_text_set_system_scale(true);
            draw_work_progress_area(ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    if (status == WR_EXPIRED && s_logged) {
        session_verification_result(status);
        refresh_shelf();
        s_message = "登录已失效，请重新扫码";
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t activate(app_ctx_t *ctx,int index) {
    if (s_view == ACTIONS) {
        if (!action_enabled(index)) return APP_REDRAW_NONE;
        if (index == 4) { s_view = SHELF; return APP_REDRAW_PAGE; }
        if (index != 0) return start(ctx,index==1?WR_CACHE:index==2?WR_UPLOAD:WR_DOWNLOAD);
        if (!wr_book_path(&s_selected,s_open)) { refresh_shelf(); s_view=SHELF; return APP_REDRAW_PAGE; }
        s_from_home=false;
        s_back_to_weread=true;
        if (s_logged && !wr_online()) { s_view=ASK_WIFI; return APP_REDRAW_PAGE; }
        open_book(ctx,false);
        return APP_REDRAW_NONE;
    }
    if (s_view == ASK_WIFI) {
        if (index == 0) { s_wifi_open=true; connect_wifi(ctx); return APP_REDRAW_NONE; }
        s_wifi_open=false;
        if (index == 1) { open_book(ctx,true); return APP_REDRAW_NONE; }
        s_open[0]=0;
        s_view=SHELF;
        if (!s_back_to_weread) { ctx->request_return=true; return APP_REDRAW_NONE; }
        return APP_REDRAW_PAGE;
    }
    if (s_view == LOGOUT) {
        if (index == 2) { s_view=ACCOUNT; return APP_REDRAW_PAGE; }
        if (!wr_logout(index==1)) { s_message="退出失败，请重试"; return APP_REDRAW_PAGE; }
        s_session_verified_us = 0;
        if (index == 1) book_store_notify_changed();
        s_message=NULL;
        s_view=ACCOUNT;
        refresh_shelf();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t gesture(app_ctx_t *ctx,const ui_gesture_event_t *event) {
    const int x=event->x0,y=event->y0;
    if (s_view == SHELF && (event->type == UI_GESTURE_SWIPE_U || event->type == UI_GESTURE_SWIPE_D)) {
        int page = s_page + (event->type==UI_GESTURE_SWIPE_U ? 1 : -1);
        if (page>=0 && page<pages()) { s_page=page; refresh_shelf(); return APP_REDRAW_PAGE; }
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_message && s_view != WORK) {
        const bool retry = s_has_retry && ui_rect_hit(retry_rect(),x,y);
        const wr_job_t job = s_retry_job;
        s_message = NULL;
        s_has_retry = false;
        if (retry) return start(ctx,job);
        return APP_REDRAW_PAGE;
    }
    if (s_view == WORK) {
        if (ui_rect_hit(action_rect(4),x,y)) { wr_cancel(); s_message="正在取消"; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_view == ACTIONS || s_view == ASK_WIFI || s_view == LOGOUT) {
        for (int i=0; i<(s_view==ACTIONS?5:3); ++i) if (ui_rect_hit(action_rect(i),x,y)) return activate(ctx,i);
        return APP_REDRAW_NONE;
    }
    int tab=ui_nav_hit(x,y);
    if (tab>=0) { ui_nav_request(ctx,tab); return APP_REDRAW_NONE; }
    if (s_view == ACCOUNT) {
        if (ui_rect_hit(action_rect(2),x,y)) {
            if (!s_logged) return start(ctx,WR_LOGIN);
            s_view=LOGOUT;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(action_rect(3),x,y)) { s_view=SHELF; s_message=NULL; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(account_rect(),x,y)) {
        s_pressed = 1; s_pressed_until = ctx->now_ms+140; s_pending_action = 1;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(sync_rect(),x,y)) {
        s_pressed = 0; s_pressed_until = ctx->now_ms+140; s_pending_action = 0;
        return APP_REDRAW_PAGE;
    }
    int pager=ui_pager_hit(x,y);
    if (pager==UI_PAGER_PREVIOUS || pager==UI_PAGER_NEXT) {
        int next=s_page+(pager==UI_PAGER_PREVIOUS?-1:1);
        if (next>=0 && next<pages()) { s_page=next; refresh_shelf(); return APP_REDRAW_PAGE; }
    }
    int slots = app_book_shelf_slots();
    if (slots > WR_SLOTS) slots = WR_SLOTS;
    for (int i=0; i<slots && s_books[i].id[0]; ++i) if (ui_rect_hit(app_book_shelf_slot(i),x,y)) {
        s_selected=s_books[i];
        s_selected_index=s_page*slots+i;
        s_selected_cached=s_cached[i];
        s_view=ACTIONS;
        s_message=NULL;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static bool root(app_ctx_t *ctx) { (void)ctx; return s_view==SHELF; }
static bool navigation(app_ctx_t *ctx) { (void)ctx; return s_view==SHELF || s_view==ACCOUNT; }
static app_redraw_t back(app_ctx_t *ctx) {
    if (s_view==ASK_WIFI || s_view==LOGOUT) return activate(ctx,2);
    if (s_view==WORK) { wr_cancel(); return APP_REDRAW_NONE; }
    if (s_view==SHELF) { ui_nav_request(ctx,UI_TAB_HOME); return APP_REDRAW_NONE; }
    s_view=SHELF;
    return APP_REDRAW_PAGE;
}
static app_redraw_t key(app_ctx_t *ctx,int key) { return key==1?back(ctx):APP_REDRAW_NONE; }
static void media(app_ctx_t *ctx) { (void)ctx; refresh_shelf(); }
static void lost(app_ctx_t *ctx) { wr_cancel(); media(ctx); }
static void locking(app_ctx_t *ctx) { (void)ctx; if (s_view == WORK) wr_cancel(); }

const app_desc_t app_weread = {
    .title="微信读书", .detail="微信读书书架", .enter_full=true,
    .persistent_navigation=true, .navigation_visible=navigation, .navigation_enabled=navigation,
    .main_page_visible=root,
    .on_enter=enter, .on_exit=leave, .on_tick=tick, .render=render,
    .on_nav_reselect=nav_reselect,
    .area_hint=work_progress_area_hint,
    .on_gesture=gesture, .on_key=key, .on_media_lost=lost, .on_media_ready=media,
    .on_before_lock=locking,
};
