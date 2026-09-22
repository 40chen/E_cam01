/*
 * page_music.c — 音乐页
 *
 * 两个视图：
 *   1. 列表视图（页面本体）：/sdcard/MUSIC 下的曲子，点一首就开始放并进入
 *      大图模式；底部常驻一条"正在播放"，点它也能进大图模式。
 *      标题栏上的垃圾桶进批量删除（和相册页同一套做法）。
 *   2. 大图模式（盖在 lv_layer_top() 上的另一个页面外壳）：左边旋转黑胶，
 *      右边一根竖排音量条，下面是进度条 + 播放控制。
 *
 * 播放本身在 app_audio 的解码任务里，这里只做两件事：发命令、按 200ms 把
 * 播放位置刷到界面上。**不要把这个刷新塞进解码任务**——触摸是 30ms 轮询、
 * 没有中断，LVGL 任务被长活儿占住就会连触摸都没反应（相册页的缩略图解码
 * 定时器踩过这个坑，见 page_photos.c 里的注释）。
 *
 * 歌词功能做过一版又移除了，代码和结论都在 _removed/ 里。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_mail.h"
#include "app_storage.h"
#include "lckfb_bsp.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_music";

#define UI_REFRESH_MS   200         /* 进度/歌词刷新周期 */
#define DISC_STEP_MS    60          /* 黑胶旋转周期 */
#define DISC_STEP_DEG   4           /* 每拍转多少度：60ms × 4° ≈ 5.4 秒一圈 */

#define NOW_BAR_H       46          /* 底部"正在播放"条 */
#define BIG_CTRL_H      44          /* 大图模式的控制行 */
#define BIG_PROG_H      24          /* 进度行（音量行共用这个高度） */
/* 黑胶直径。大图模式的视觉区高 130（206 减去进度 24 和控制 44 和两处间距），
 * 歌名在标题栏上，所以唱片可以几乎占满 */
#define DISC_D          124

/* 竖排音量条。宽度就是"粗细"：320x240 上细滑条根本按不准，给到 24px；
 * 高度受视觉区（130）限制，留出上面的喇叭图标 */
#define VOL_SLIDER_W    24
#define VOL_SLIDER_H    96

/*---------------------------------------------------------------------------
 * 状态
 *-------------------------------------------------------------------------*/
static lv_obj_t *s_root;            /* 页面本体（ui.c 销毁的就是它） */
static lv_obj_t *s_list;            /* 曲目列表容器 */
static lv_obj_t *s_now_name;        /* 底部条的曲名 */
static lv_obj_t *s_btn_now_play;    /* 底部条的播放/暂停 */
static int       s_shown_index = -2;/* 界面当前高亮的是第几首；-2 = 还没同步过 */
static lv_timer_t *s_ui_timer;

/* 删除用的批量选择模式（和相册页一套做法） */
static lv_obj_t    *s_title_lbl;
static lv_obj_t    *s_del_btn;
static lv_obj_t    *s_mail_btn;
static lv_obj_t    *s_all_btn;
static bool         s_select_mode;
static bool         s_selected[APP_AUDIO_MAX_TRACKS];
static int          s_selected_count;

/* 大图模式 */
static lv_obj_t *s_big;             /* 覆盖层外壳；NULL = 没开 */
static lv_obj_t *s_visual;          /* 视觉区（黑胶 + 竖排音量条） */
static lv_obj_t *s_vinyl;           /* 黑胶容器 */
static lv_obj_t *s_disc_arc;        /* 会转的高光弧 */
static lv_obj_t *s_big_title;       /* 大图模式标题栏上的歌名 */
static lv_timer_t *s_disc_timer;
static int       s_disc_deg;

static lv_obj_t *s_prog_row;
static lv_obj_t *s_prog_slider;
static lv_obj_t *s_lbl_pos;
static lv_obj_t *s_lbl_dur;
static lv_obj_t *s_btn_big_play;

static void refresh_ui(void);
static void open_big_mode(void);
static void big_close(void);
static void build_visual(void);
static void prev_clicked_cb(lv_event_t *e);
static void next_clicked_cb(lv_event_t *e);
static void ui_timer_cb(lv_timer_t *t);
static void big_back_cb(lv_event_t *e);
static void set_selected(int idx, bool on);
static void exit_select_mode(void);
static void rebuild_list(void);

/*---------------------------------------------------------------------------
 * 小工具
 *-------------------------------------------------------------------------*/
/* 显示用的曲名：去掉 .mp3 后缀 */
static void display_name(char *dst, size_t cap, const char *name)
{
    strlcpy(dst, name, cap);
    const size_t n = strlen(dst);
    if (n > 4 && strcasecmp(dst + n - 4, ".mp3") == 0) {
        dst[n - 4] = '\0';
    }
}

static void fmt_time(char *buf, size_t cap, uint32_t ms)
{
    const uint32_t sec = ms / 1000;
    snprintf(buf, cap, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

static const char *play_symbol(void)
{
    return (app_audio_state() == APP_AUDIO_PLAYING) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
}

/* 图标按钮。项目里没有现成的 helper，相册页/相机页都是现写的，这里沿用
 * 它们的做法：透明底 + 强调色图标 + ext_click_area 把触点放大到 44 上下 */
static lv_obj_t *icon_button_create(lv_obj_t *parent, const char *symbol, int size,
                                    int big_icon, lv_color_t color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, big_icon ? UI_FONT_ICON : UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_center(lbl);
    /* 图标要能换（播放/暂停），把 label 存起来方便找回 */
    lv_obj_set_user_data(btn, lbl);
    return btn;
}

static void icon_button_set(lv_obj_t *btn, const char *symbol)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (lbl != NULL) {
        lv_label_set_text(lbl, symbol);
    }
}

/* 列表 / 控制按钮共用的两个动作 */
static void prev_clicked_cb(lv_event_t *e)
{
    (void)e;
    app_audio_play_prev();
}

static void next_clicked_cb(lv_event_t *e)
{
    (void)e;
    app_audio_play_next();
}

/*---------------------------------------------------------------------------
 * 列表视图
 *-------------------------------------------------------------------------*/
static void track_clicked_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_select_mode) {
        set_selected(index, !s_selected[index]);
        return;
    }
    if (app_audio_play_index(index) != ESP_OK) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    /* 点歌即进大图模式（用户要的交互） */
    open_big_mode();
}

static void build_list(lv_obj_t *parent)
{
    const int count = app_audio_track_count();

    if (!app_storage_ready()) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, "No TF card");
        lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
        lv_obj_center(lbl);
        return;
    }
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, "No music\n/sdcard/MUSIC\n" LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
        lv_obj_center(lbl);
        return;
    }

    for (int i = 0; i < count; i++) {
        char name[APP_AUDIO_NAME_LEN];
        display_name(name, sizeof(name), app_audio_track_name(i));

        lv_obj_t *row = ui_list_row_create(parent, name, NULL, false);
        /*
         * 歌名换成带汉字的字体。ui_list_row_create 默认给标题用 UI_FONT_BODY
         * （Montserrat），它没有任何汉字——中文文件名会整行显示成方框。
         * 行里的第 0 个子对象就是标题 label（见 ui_theme.c 里那个函数）。
         */
        lv_obj_set_style_text_font(lv_obj_get_child(row, 0), UI_FONT_CJK, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, track_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_shown_index = -2;                 /* 让 refresh_ui() 重刷一次高亮 */
}

/*---------------------------------------------------------------------------
 * 删除：批量选择模式（和相册页同一套做法，只是对象从照片换成了曲目）
 *
 * 入口是标题栏上常驻的 Delete 按钮：点一下进选择模式，再点曲目切换选中，
 * 选好之后再点 Delete 弹二次确认。同一个按钮承担"进入"和"确认"两个角色，
 * 靠 s_select_mode 区分。
 *-------------------------------------------------------------------------*/
static void update_title(void)
{
    if (s_title_lbl == NULL) {
        return;
    }
    if (!s_select_mode) {
        lv_label_set_text(s_title_lbl, "Music");
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d selected", s_selected_count);
    lv_label_set_text(s_title_lbl, buf);
}

static void update_buttons(void)
{
    if (s_del_btn == NULL || s_all_btn == NULL || s_mail_btn == NULL) {
        return;
    }
    const int count = app_audio_track_count();

    /* 列表模式：点哪个都进选择模式；选择模式：没选中就没得发/没得删 */
    const bool usable = s_select_mode ? (s_selected_count > 0) : (count > 0);
    if (usable) {
        lv_obj_clear_state(s_del_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(s_mail_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_del_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_mail_btn, LV_STATE_DISABLED);
    }

    /* "全选"只在选择模式露面，平时不占标题栏的地方（相册页那版就是挤没了） */
    if (!s_select_mode) {
        lv_obj_add_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(s_all_btn, 0),
                      (s_selected_count < count) ? "All" : "None");
    if (count > 0) {
        lv_obj_clear_state(s_all_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_all_btn, LV_STATE_DISABLED);
    }
}

static void set_selected(int idx, bool on)
{
    const int count = app_audio_track_count();
    if (idx < 0 || idx >= count || idx >= APP_AUDIO_MAX_TRACKS) {
        return;
    }
    if (s_selected[idx] == on) {
        return;
    }
    s_selected[idx] = on;
    s_selected_count += on ? 1 : -1;

    /* 选中态用整行底色表示，和相册页一致 */
    if (s_list != NULL) {
        lv_obj_t *row = lv_obj_get_child(s_list, idx);
        if (row != NULL) {
            lv_obj_set_style_bg_color(row, on ? UI_COLOR_ACCENT : UI_COLOR_CARD, 0);
            lv_obj_t *lbl = lv_obj_get_child(row, 0);
            if (lbl != NULL) {
                lv_obj_set_style_text_color(lbl, on ? lv_color_white() : UI_COLOR_TEXT, 0);
            }
        }
    }
    update_title();
    update_buttons();
}

/* 全选 / 全不选。已经全选中了再点一次就是全部取消，所以不需要两个按钮 */
static void all_clicked_cb(lv_event_t *e)
{
    (void)e;
    const bool select_all = (s_selected_count < app_audio_track_count());
    for (int i = 0; i < app_audio_track_count(); i++) {
        set_selected(i, select_all);
    }
}

static void enter_select_mode(void)
{
    if (s_select_mode) {
        return;
    }
    s_select_mode = true;
    s_selected_count = 0;
    memset(s_selected, 0, sizeof(s_selected));
    update_title();
    update_buttons();
}

static void exit_select_mode(void)
{
    if (!s_select_mode) {
        return;
    }
    const int count = app_audio_track_count();
    for (int i = 0; i < count && i < APP_AUDIO_MAX_TRACKS; i++) {
        s_selected[i] = false;
        if (s_list != NULL) {
            lv_obj_t *row = lv_obj_get_child(s_list, i);
            if (row != NULL) {
                lv_obj_set_style_bg_color(row, UI_COLOR_CARD, 0);
                lv_obj_t *lbl = lv_obj_get_child(row, 0);
                if (lbl != NULL) {
                    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
                }
            }
        }
    }
    s_select_mode = false;
    s_selected_count = 0;
    update_title();
    update_buttons();
    s_shown_index = -2;                 /* 让 refresh_ui 把"正在播放"的高亮补回来 */
    refresh_ui();
}

static void rebuild_list(void)
{
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);
    app_audio_scan(APP_MUSIC_DIR);
    build_list(s_list);

    memset(s_selected, 0, sizeof(s_selected));
    s_selected_count = 0;
    s_select_mode = false;
    update_title();
    update_buttons();
    refresh_ui();
}

static void delete_async(void *arg)
{
    (void)arg;
    if (s_list == NULL) {
        return;                         /* 页面已经销毁 */
    }

    /*
     * 先把播放停掉再删。三条理由，每条都能单独咬人：
     *   1) FATFS 上删一个正被 fopen 打开的文件（就是正在播的那首）行为不可靠；
     *   2) 重扫会重建曲目数组，而解码任务正拿它拼路径；
     *   3) 列表一变，s_index 指向的就是另一首歌了，播放状态必然错位。
     * 停播比"想办法保住播放"简单得多，也不会留下自相矛盾的界面状态。
     */
    app_audio_stop();

    const int count = app_audio_track_count();
    int wanted = 0, removed = 0;
    for (int i = 0; i < count && i < APP_AUDIO_MAX_TRACKS; i++) {
        if (!s_selected[i]) {
            continue;
        }
        wanted++;
        char path[sizeof(APP_MUSIC_DIR) + APP_AUDIO_NAME_LEN + 2];
        snprintf(path, sizeof(path), "%s/%s", APP_MUSIC_DIR, app_audio_track_name(i));
        if (remove(path) == 0) {
            removed++;
        } else {
            ESP_LOGW(TAG, "remove failed: %s", path);
        }
    }
    ESP_LOGI(TAG, "删除了 %d / %d 首", removed, wanted);

    rebuild_list();                     /* 重扫，把删失败的那几首也重新对齐 */

    char msg[48];
    if (removed == wanted) {
        snprintf(msg, sizeof(msg), "Deleted %d track(s)", removed);
    } else {
        snprintf(msg, sizeof(msg), "Deleted %d of %d", removed, wanted);
    }
    ui_toast_show(msg, removed > 0 ? lv_color_white() : UI_COLOR_DANGER);
}

static void del_confirm_cb(lv_event_t *e)
{
    /*
     * 必须用 get_current_target，不能用 get_target：msgbox 的按钮是 btnmatrix，
     * 它发的 VALUE_CHANGED 会冒泡到 msgbox 才轮到我们；冒泡时 e->target 仍然
     * 是最初发事件的 btnmatrix，拿它当 lv_msgbox_t 读成员会得到野指针、
     * 解引用直接 LoadProhibited 复位（相册页和设置页都踩过，见 README）。
     */
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        /* 真正动手放到下一次 LVGL 处理：现在还在 msgbox 的回调里，而删除会重建列表 */
        lv_async_call(delete_async, NULL);
    }
    lv_msgbox_close_async(mbox);
}

static void del_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!s_select_mode) {
        enter_select_mode();
        return;
    }
    if (s_selected_count <= 0) {
        return;
    }
    char text[48];
    snprintf(text, sizeof(text), "Delete %d selected track(s)?", s_selected_count);
    static const char *btns[] = { "Cancel", "Delete", "" };
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Delete", text, btns, false);
    lv_obj_set_style_text_font(lv_msgbox_get_text(mbox), UI_FONT_SMALL, 0);
    lv_obj_add_event_cb(mbox, del_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/*---------------------------------------------------------------------------
 * 批量发邮件：把选中的曲目作为多个附件放进同一封邮件
 *
 * 和相册页那套是同一套流程（拷快照 → 后台任务 → lv_async_call 回 UI），
 * 差别只有两点：
 *   - 附件标成 audio/mpeg（走 app_mail_send_files；标成 image/jpeg 虽然多数
 *     客户端会自己嗅探，但那是碰运气）；
 *   - **MP3 比照片大得多**（手机里导出来的动辄十几 MB），而单封邮件的附件总量
 *     上限是 APP_MAIL_MAX_ATTACH（18MB，见 app_mail.h 里的推导）。所以这里先
 *     算总大小拦一道，报出实际数字（"Too big: 21.0 MB"），而不是笼统一个
 *     Attachment too big——用户至少知道差多少。
 *-------------------------------------------------------------------------*/
#define MAIL_TASK_STACK 8192

typedef struct {
    char **paths;
    int    count;
} mail_job_t;

static void mail_job_free(mail_job_t *job)
{
    if (job == NULL) {
        return;
    }
    for (int i = 0; i < job->count; i++) {
        free(job->paths[i]);
    }
    free(job->paths);
    free(job);
}

static void mail_done_async(void *arg)
{
    const app_mail_result_t r = (app_mail_result_t)(intptr_t)arg;
    if (s_mail_btn != NULL) {
        lv_obj_clear_state(s_mail_btn, LV_STATE_DISABLED);
    }
    ui_toast_show(app_mail_result_str(r),
                  r == APP_MAIL_OK ? lv_color_white() : UI_COLOR_DANGER);
}

static void mail_task(void *arg)
{
    mail_job_t *job = (mail_job_t *)arg;
    const app_mail_result_t r = app_mail_send_files((const char *const *)job->paths,
                                                    job->count, "audio/mpeg", "music");
    mail_job_free(job);
    lv_async_call(mail_done_async, (void *)(intptr_t)r);
    vTaskDelete(NULL);
}

static void mail_clicked_cb(lv_event_t *e)
{
    (void)e;

    /* 列表模式下这一下是"进入选择模式"（和 Delete 按钮一样） */
    if (!s_select_mode) {
        enter_select_mode();
        return;
    }
    if (s_selected_count == 0 || s_list == NULL) {
        return;
    }

    const char *missing = app_mail_missing_field();
    if (missing != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Set %s first", missing);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }
    if (s_selected_count > APP_MAIL_MAX_ATTACHMENTS) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Max %d files per mail", APP_MAIL_MAX_ATTACHMENTS);
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /* 先算总的文件大小：超了就直接说清楚，不要连上服务器、认证完、
     * 正文发了一半才发现附件塞不下 */
    long total = 0;
    const int count = app_audio_track_count();
    for (int i = 0; i < count && i < APP_AUDIO_MAX_TRACKS; i++) {
        if (!s_selected[i]) {
            continue;
        }
        char path[sizeof(APP_MUSIC_DIR) + APP_AUDIO_NAME_LEN + 2];
        snprintf(path, sizeof(path), "%s/%s", APP_MUSIC_DIR, app_audio_track_name(i));
        struct stat st;
        if (stat(path, &st) == 0) {
            total += (long)st.st_size;
        }
    }
    if (total > APP_MAIL_MAX_ATTACH) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Too big: %.1f MB", (double)total / (1024 * 1024));
        ui_toast_show(buf, UI_COLOR_DANGER);
        return;
    }

    /*
     * 拷一份路径快照交给任务：任务要跑好几秒到几十秒，这期间用户可能已经
     * 退出选择模式、删了歌、甚至离开页面——让任务直接读 s_selected/s_names
     * 就是踩已释放内存。
     */
    mail_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    job->paths = calloc(s_selected_count, sizeof(char *));
    if (job->paths == NULL) {
        free(job);
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
        return;
    }
    for (int i = 0; i < count && job->count < s_selected_count && i < APP_AUDIO_MAX_TRACKS; i++) {
        if (!s_selected[i]) {
            continue;
        }
        char path[sizeof(APP_MUSIC_DIR) + APP_AUDIO_NAME_LEN + 2];
        snprintf(path, sizeof(path), "%s/%s", APP_MUSIC_DIR, app_audio_track_name(i));
        job->paths[job->count] = strdup(path);
        if (job->paths[job->count] == NULL) {
            break;
        }
        job->count++;
    }
    if (job->count == 0) {
        mail_job_free(job);
        return;
    }

    if (s_mail_btn != NULL) {
        lv_obj_add_state(s_mail_btn, LV_STATE_DISABLED);   /* 防连点 */
    }
    ui_toast_show("Sending...", lv_color_white());

    if (xTaskCreate(mail_task, "mail", MAIL_TASK_STACK, job, 5, NULL) != pdPASS) {
        mail_job_free(job);
        if (s_mail_btn != NULL) {
            lv_obj_clear_state(s_mail_btn, LV_STATE_DISABLED);
        }
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
}

/* "正在播放"那一行换成强调色底、白字。行是按顺序建的，用子对象下标定位，
 * 不额外存指针表（列表建好就不再变） */
static void highlight_row(int index)
{
    if (s_list == NULL) {
        return;
    }
    const uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        const bool cur = ((int)i == index);
        lv_obj_set_style_bg_color(row, cur ? UI_COLOR_ACCENT : UI_COLOR_CARD, 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl, cur ? lv_color_white() : UI_COLOR_TEXT, 0);
        }
    }
}

static void now_bar_cb(lv_event_t *e)
{
    /*
     * 这行是防御性的，当前**不会**生效：LVGL 8.3 里只有带
     * LV_OBJ_FLAG_EVENT_BUBBLE 的子对象才会把事件交给父对象
     * （lv_event.c 的 event_is_bubbled），而条里的按钮没有这个标志，
     * 所以按按钮根本走不到这里。
     *
     * 留着它是因为万一以后给那些按钮加上了冒泡标志，没有这行就会变成
     * "按一下播放键,顺带把大图模式也打开了"。判断方式：冒泡时 target 是最初
     * 发事件的那个对象、current_target 才是本回调所属的对象。
     */
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    open_big_mode();
}

static void now_play_cb(lv_event_t *e)
{
    (void)e;
    if (app_audio_toggle() != ESP_OK) {
        ui_toast_show("Out of memory", UI_COLOR_DANGER);
    }
    refresh_ui();
}

static lv_obj_t *build_now_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = ui_card_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), NOW_BAR_H);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_pad_left(bar, 10, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, now_bar_cb, LV_EVENT_CLICKED, NULL);

    s_now_name = lv_label_create(bar);
    lv_label_set_text(s_now_name, "Tap a song to play");
    /* 带汉字的字体：这里显示的是文件名，中文歌名在 Montserrat 下是方框 */
    lv_obj_set_style_text_font(s_now_name, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(s_now_name, UI_COLOR_TEXT_SUB, 0);
    lv_label_set_long_mode(s_now_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_now_name, 166);
    lv_obj_align(s_now_name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *next = icon_button_create(bar, LV_SYMBOL_NEXT, 34, 0, UI_COLOR_ACCENT);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(next, next_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_btn_now_play = icon_button_create(bar, LV_SYMBOL_PLAY, 34, 0, UI_COLOR_ACCENT);
    lv_obj_align(s_btn_now_play, LV_ALIGN_RIGHT_MID, -36, 0);
    lv_obj_add_event_cb(s_btn_now_play, now_play_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *prev = icon_button_create(bar, LV_SYMBOL_PREV, 34, 0, UI_COLOR_ACCENT);
    lv_obj_align(prev, LV_ALIGN_RIGHT_MID, -72, 0);
    lv_obj_add_event_cb(prev, prev_clicked_cb, LV_EVENT_CLICKED, NULL);

    return bar;
}

/*---------------------------------------------------------------------------
 * 大图模式：黑胶
 *-------------------------------------------------------------------------*/
/*
 * 盘面上的装饰件：透明底、无边框、**不可点**。
 *
 * 不可点是必须的：lv_obj_create 默认带 LV_OBJ_FLAG_CLICKABLE，而 LVGL 8.3 的
 * 命中测试取的是最内层的可点对象，所以盘子、纹路、纸标这些如果可点，点在盘子
 * 上事件就归它们，冒不到视觉区去（见 visual_clicked_cb 和 lv_event.c 的
 * event_is_bubbled）——切换歌词/黑胶就永远点不出来。
 */
static lv_obj_t *deco_create(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

/*
 * 黑胶的配色。
 *
 * 原来盘面是黑的、中心纸标用的是主题的 iOS 蓝，整体是"蓝黑"。这里整套换成
 * 红黑，几个决定：
 *
 *  - **盘面不用纯黑**（0x000000）。屏幕底色是 iOS 灰、视觉区是浅色卡片，
 *    纯黑倒也不算撞色，但盘面纹理要在盘上分层，纯黑没有"底色再往下压"的
 *    余量。这里用近黑，并且**带一点红调**（R 通道比 G/B 高），和红纹路是
 *    同一族色，整体才不会像"黑盘上贴了几道红胶带"。
 *  - **外圈加一道深红描边**：盘面近黑、卡片是白的，边界本来靠明度差就够；
 *    但往下滚动时视觉区会贴着深色进度条，加这一圈让盘的轮廓在任何背景上
 *    都成立。
 *  - **中心孔用盘面色而不是屏幕底色**。原来开的是浅灰的洞，那是"红色圆环
 *    中间有个洞"；换成盘面色才是唱片"中间那个孔"。纸标因此变成一个红环。
 *  - 高光弧沿用主题里的 DANGER 红——调色板里只有这一个亮红，另起一个色值
 *    不如跟着走；它在近黑盘面上对比足够。
 */
#define VINYL_DISC     lv_color_hex(0x140A0B)   /* 盘面：近黑带红调 */
#define VINYL_RIM      lv_color_hex(0x8E1418)   /* 外圈深红描边 */
#define VINYL_GROOVE   lv_color_hex(0x3A1518)   /* 同心纹路 */
#define VINYL_LABEL    lv_color_hex(0xC01A1F)   /* 中心纸标 */
#define VINYL_HILIGHT  UI_COLOR_DANGER          /* 高光弧（转起来的那道） */

/* 全用图元画，不需要素材。旋转只改 lv_arc 自带的 rotation（转一段高光弧的
 * 起始角），不走 LVGL 的 transform_angle 图层变换——那条路每帧要把整块
 * 区域重新渲染一遍，而这里每拍只重绘一条细弧。 */
static void build_vinyl(lv_obj_t *parent)
{
    s_vinyl = lv_obj_create(parent);
    lv_obj_set_size(s_vinyl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_vinyl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_vinyl, 0, 0);
    lv_obj_set_style_pad_all(s_vinyl, 0, 0);
    lv_obj_clear_flag(s_vinyl, LV_OBJ_FLAG_SCROLLABLE);
    /* 盘子自己可点（这样点击落在这里），但要让事件继续冒到视觉区 */
    lv_obj_add_flag(s_vinyl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *disc = deco_create(s_vinyl);
    lv_obj_set_size(disc, DISC_D, DISC_D);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, VINYL_DISC, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disc, 2, 0);
    lv_obj_set_style_border_color(disc, VINYL_RIM, 0);
    /* 左边（给右边的竖排音量条让出位置）：视觉区 320 宽，音量条占最右约 36px，
     * 所以把盘子居中在剩下的 284px 里 → 中心 142，盘子 124 宽 → 左边缘 80 */
    lv_obj_align(disc, LV_ALIGN_LEFT_MID, 80, 0);

    /* 几圈更暗的同心纹路。它们本身是圆的，转不转看不出来，所以只建一次；
     * 真正提供"在转"这个信息的是下面那段高光弧 */
    static const int groove_pct[] = { 92, 80, 68, 56 };
    for (size_t i = 0; i < sizeof(groove_pct) / sizeof(groove_pct[0]); i++) {
        lv_obj_t *g = deco_create(disc);
        const int d = DISC_D * groove_pct[i] / 100;
        lv_obj_set_size(g, d, d);
        lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(g, 1, 0);
        lv_obj_set_style_border_color(g, VINYL_GROOVE, 0);
        lv_obj_center(g);
    }

    /* 纸标外面一圈亮红细环。纹路到纸标之间原来是硬切过去的，垫这一圈之后
     * 红纸标像是"嵌在盘里"，而不是飘在黑底上 */
    lv_obj_t *halo = deco_create(disc);
    lv_obj_set_size(halo, DISC_D * 42 / 100, DISC_D * 42 / 100);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(halo, 1, 0);
    lv_obj_set_style_border_color(halo, VINYL_HILIGHT, 0);
    lv_obj_center(halo);

    /* 中央的纸标 + 中心孔 */
    lv_obj_t *label = deco_create(disc);
    lv_obj_set_size(label, DISC_D * 34 / 100, DISC_D * 34 / 100);
    lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(label, VINYL_LABEL, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_center(label);

    lv_obj_t *hole = deco_create(label);
    lv_obj_set_size(hole, 8, 8);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hole, VINYL_DISC, 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_center(hole);

    s_disc_arc = lv_arc_create(disc);
    lv_obj_set_size(s_disc_arc, DISC_D - 12, DISC_D - 12);
    lv_obj_center(s_disc_arc);
    lv_obj_remove_style(s_disc_arc, NULL, LV_PART_KNOB);   /* 去掉可拖的圆点 */
    lv_obj_clear_flag(s_disc_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(s_disc_arc, 0, 360);
    lv_obj_set_style_arc_opa(s_disc_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_disc_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_disc_arc, VINYL_HILIGHT, LV_PART_INDICATOR);
    lv_arc_set_angles(s_disc_arc, 0, 40);
    /* 歌名放在标题栏上（见 open_big_mode），视觉区整块留给唱片/歌词，
     * 所以这里不再另外挂一个歌名 label——它会压在盘面下缘上 */
}

static void disc_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_disc_arc == NULL) {
        return;
    }
    s_disc_deg = (s_disc_deg + DISC_STEP_DEG) % 360;
    lv_arc_set_rotation(s_disc_arc, s_disc_deg);
}

/*---------------------------------------------------------------------------
 * 大图模式：视觉区（黑胶 + 竖排音量）
 *-------------------------------------------------------------------------*/
static void volume_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    app_audio_set_volume((int)lv_slider_get_value(sl));
}

/*
 * 竖排音量条，放在黑胶右边。
 *
 * LVGL 8.3 的滑条没有"方向"参数，方向是按宽高自己判的（lv_bar.c 里
 * `bool hor = barw >= barh`），所以**竖着放就是把高度设得比宽度大**——
 * 连拖动方向和端点都跟着对，不用自己做旋转。
 *
 * 做粗是刻意的：320x240 上用默认细滑条根本按不准，宽度给到 24px，
 * 滑块做成 24x14 的横条。
 */
static void build_volume_slider(lv_obj_t *parent)
{
    /*
     * 图标和滑条串在一个竖直的列容器里，滑条用 flex_grow 吃掉图标以外的
     * 全部高度——这样两者不可能重叠。
     *
     * 上一版是分别用 RIGHT_MID + 固定偏移定位的，算出来图标跑到了视觉区上
     * 边之外（视觉区高 130，96 高的滑条居中后上方只剩 17px，而图标按 -58
     * 偏移会被顶到 y≈0 再被裁掉），看起来就和滑条叠在一起了。
     */
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, VOL_SLIDER_W + 12, LV_PCT(100));
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 2, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(col);
    lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(icon, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(icon, UI_COLOR_TEXT_SUB, 0);

    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_width(sl, VOL_SLIDER_W);
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_set_ext_click_area(sl, 10);

    int vol = 0;
    app_audio_get_volume(&vol);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, vol, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(sl, UI_COLOR_SEP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 6, LV_PART_KNOB);
    lv_obj_set_style_width(sl, VOL_SLIDER_W, LV_PART_KNOB);
    lv_obj_set_style_height(sl, 14, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* 视觉区：黑胶居中偏左，音量条在最右边 */
static void build_visual(void)
{
    if (s_visual == NULL) {
        return;
    }
    if (s_vinyl != NULL) {
        lv_obj_del(s_vinyl);
        s_vinyl = NULL;
        s_disc_arc = NULL;
    }
    if (s_disc_timer != NULL) {
        lv_timer_del(s_disc_timer);
        s_disc_timer = NULL;
    }
    build_vinyl(s_visual);
    build_volume_slider(s_visual);
    s_disc_timer = lv_timer_create(disc_timer_cb, DISC_STEP_MS, NULL);
}

/*---------------------------------------------------------------------------
 * 大图模式：进度 / 音量 / 控制
 *-------------------------------------------------------------------------*/
static void prog_released_cb(lv_event_t *e)
{
    (void)e;
    if (s_prog_slider == NULL) {
        return;
    }
    const uint32_t dur = app_audio_duration_ms();
    if (dur > 0) {
        /* 用 64 位中间量：滑块是 0~1000，乘上超过 71.6 分钟的时长就会把
         * uint32 撑溢出（长播客/整轨混音会撞上），算出来是个回绕后的垃圾值 */
        app_audio_seek_ms((uint32_t)((uint64_t)lv_slider_get_value(s_prog_slider) * dur / 1000u));
    }
}

static void big_play_cb(lv_event_t *e)
{
    (void)e;
    app_audio_toggle();
    refresh_ui();
}

/* 进度条在 320x240 上很细，够不着，用 ext_click_area 把上下各放大 10px */
static void style_slider(lv_obj_t *sl)
{
    lv_obj_set_height(sl, 6);
    lv_obj_set_ext_click_area(sl, 10);
    lv_obj_set_style_bg_color(sl, UI_COLOR_SEP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
}

static lv_obj_t *row_container_create(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), BIG_PROG_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return row;
}

static void build_progress_row(lv_obj_t *parent)
{
    s_prog_row = row_container_create(parent);

    s_lbl_pos = lv_label_create(s_prog_row);
    lv_label_set_text(s_lbl_pos, "0:00");
    lv_obj_set_style_text_font(s_lbl_pos, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_lbl_pos, UI_COLOR_TEXT_SUB, 0);

    s_prog_slider = lv_slider_create(s_prog_row);
    lv_slider_set_range(s_prog_slider, 0, 1000);        /* 千分比，够细了 */
    style_slider(s_prog_slider);
    lv_obj_set_flex_grow(s_prog_slider, 1);
    lv_obj_add_event_cb(s_prog_slider, prog_released_cb, LV_EVENT_RELEASED, NULL);

    s_lbl_dur = lv_label_create(s_prog_row);
    lv_label_set_text(s_lbl_dur, "0:00");
    lv_obj_set_style_text_font(s_lbl_dur, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_lbl_dur, UI_COLOR_TEXT_SUB, 0);
}

static void build_controls(lv_obj_t *parent)
{
    lv_obj_t *ctrl = lv_obj_create(parent);
    lv_obj_set_size(ctrl, LV_PCT(100), BIG_CTRL_H);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 0, 0);
    lv_obj_set_style_pad_column(ctrl, 16, 0);
    lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *prev = icon_button_create(ctrl, LV_SYMBOL_PREV, 38, 0, UI_COLOR_TEXT);
    lv_obj_add_event_cb(prev, prev_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_btn_big_play = ui_round_button_create(ctrl, 44, UI_COLOR_ACCENT);
    lv_obj_set_ext_click_area(s_btn_big_play, 6);
    lv_obj_t *pl = lv_label_create(s_btn_big_play);
    lv_label_set_text(pl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(pl, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(pl, lv_color_white(), 0);
    lv_obj_center(pl);
    lv_obj_set_user_data(s_btn_big_play, pl);
    lv_obj_add_event_cb(s_btn_big_play, big_play_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = icon_button_create(ctrl, LV_SYMBOL_NEXT, 38, 0, UI_COLOR_TEXT);
    lv_obj_add_event_cb(next, next_clicked_cb, LV_EVENT_CLICKED, NULL);
}

static void open_big_mode(void)
{
    if (s_big != NULL || s_select_mode || app_audio_track_count() <= 0) {
        return;
    }

    /* 盖在 lv_layer_top() 上，不塞进页面外壳的内容区：那是个 flex 容器，
     * 会把全屏子对象重新摆位。相册页的大图也是这么做的 */
    s_big = ui_page_shell_create_ex(lv_layer_top(), "Playing", big_back_cb);
    lv_obj_t *content = ui_page_shell_content(s_big);

    /* 歌名写进标题栏：标题栏是外壳的第 0 个子对象，里面第 0 个是返回键、
     * 第 1 个是标题文本（相册页也是这么取标题 label 的）。
     * 必须限宽 + 省略号，否则长文件名会一路压到屏幕右边缘 */
    lv_obj_t *bar = lv_obj_get_child(s_big, 0);
    if (bar != NULL) {
        s_big_title = lv_obj_get_child(bar, 1);
        if (s_big_title != NULL) {
            /* 标题栏默认字体（Montserrat 20）没有汉字，歌名是中文的话全是方框。
             * 换成带汉字的字体，字号从 20 变成 16，宽度也要跟着留够 */
            lv_obj_set_style_text_font(s_big_title, UI_FONT_CJK, 0);
            lv_label_set_long_mode(s_big_title, LV_LABEL_LONG_DOT);
            lv_obj_set_width(s_big_title, 236);
        }
    }
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 4, 0);

    /* 视觉区吃掉剩余高度：黑胶在左、竖排音量条在右。
     * 内容区高 206，减去 24(进度) + 44(控制) + 2×4(间距) = 130 留给视觉区 */
    s_visual = lv_obj_create(content);
    lv_obj_set_width(s_visual, LV_PCT(100));
    lv_obj_set_flex_grow(s_visual, 1);
    lv_obj_set_style_bg_opa(s_visual, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_visual, 0, 0);
    lv_obj_set_style_pad_all(s_visual, 0, 0);
    lv_obj_clear_flag(s_visual, LV_OBJ_FLAG_SCROLLABLE);

    build_progress_row(content);
    build_controls(content);

    build_visual();
    refresh_ui();
}

static void big_back_cb(lv_event_t *e)
{
    (void)e;
    /* 大图模式的返回键是"关掉这一层"，不是回主菜单：音乐还在放，
     * 用户只是退回列表视图。关自己必须异步（回调执行期间删掉自己
     * 所属的对象在 LVGL 里不安全） */
    big_close();
}

static void big_close(void)
{
    if (s_big == NULL) {
        return;
    }
    if (s_disc_timer != NULL) {
        lv_timer_del(s_disc_timer);
        s_disc_timer = NULL;
    }
    lv_obj_t *obj = s_big;
    /* 先清指针再删：删除是异步的，删完到真正回收之间还可能有回调进来 */
    s_big = NULL;
    s_visual = NULL;
    s_vinyl = NULL;
    s_disc_arc = NULL;
    s_big_title = NULL;
    s_prog_row = NULL;
    s_prog_slider = NULL;
    s_lbl_pos = NULL;
    s_lbl_dur = NULL;
    s_btn_big_play = NULL;
    lv_obj_del_async(obj);
}

/*---------------------------------------------------------------------------
 * 定时刷新（跑在 LVGL 任务里，200ms 一拍）
 *-------------------------------------------------------------------------*/
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_ui();
}

static void refresh_ui(void)
{
    if (s_root == NULL) {
        return;
    }
    const int index = app_audio_current_index();

    /* 换歌：列表高亮 + 底部条曲名。只在换歌时做，不必每拍都刷 */
    if (index != s_shown_index) {
        s_shown_index = index;
        if (!s_select_mode) {
            /* 选择模式下行的底色归选中态用，不能让"正在播放"的高亮去抢 */
            highlight_row(index);
        }
        char name[APP_AUDIO_NAME_LEN] = "";
        if (index >= 0 && index < app_audio_track_count()) {
            display_name(name, sizeof(name), app_audio_track_name(index));
            lv_label_set_text(s_now_name, name);
        } else {
            lv_label_set_text(s_now_name, "Tap a song to play");
        }
        if (s_big_title != NULL) {
            lv_label_set_text(s_big_title, name[0] ? name : "Playing");
        }
    }

    icon_button_set(s_btn_now_play, play_symbol());
    icon_button_set(s_btn_big_play, play_symbol());

    if (s_big == NULL) {
        return;
    }

    /* 进度条。用户正在拖的时候不要抢——那会让滑块跟手指较劲 */
    if (s_prog_slider != NULL && !lv_obj_has_state(s_prog_slider, LV_STATE_PRESSED)) {
        const uint32_t dur = app_audio_duration_ms();
        const uint32_t pos = app_audio_position_ms();
        lv_slider_set_value(s_prog_slider,
                            (dur > 0) ? (int)((uint64_t)pos * 1000u / dur) : 0,
                            LV_ANIM_OFF);
        char buf[16];
        fmt_time(buf, sizeof(buf), pos);
        lv_label_set_text(s_lbl_pos, buf);
        fmt_time(buf, sizeof(buf), dur);
        lv_label_set_text(s_lbl_dur, buf);
    }

}

/*---------------------------------------------------------------------------
 * 播放状态回调：在解码任务里被调用，不能直接碰 LVGL，转回 LVGL 上下文
 * （和相册页的 mail_task / 设置页的 tone_task 是同一套写法）
 *-------------------------------------------------------------------------*/
static void audio_event_async(void *arg)
{
    (void)arg;
    if (s_root == NULL) {
        return;                         /* 页面已经销毁 */
    }
    /* 换歌只要刷一下标题/高亮就行，大图模式里没有随歌变化的东西了
     * （黑胶是恒定图形，音量条跟的是全局音量） */
    refresh_ui();
}

static void on_audio_event(app_audio_state_t state, int index, void *user)
{
    (void)state;
    (void)index;
    (void)user;
    lv_async_call(audio_event_async, NULL);
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
static void page_back_cb(lv_event_t *e)
{
    (void)e;
    /* 选择模式下返回键是"取消选择"，不是退出页面（和相册页一致） */
    if (s_select_mode) {
        exit_select_mode();
        return;
    }
    /* 返回键就是离开音乐页：先停播（关功放、释放解码器），再异步切页。
     * 切页必须走异步——回调执行期间把整个页面（含这个按钮）删掉在 LVGL
     * 里是未定义行为 */
    app_audio_stop();
    ui_go_home_async();
}

lv_obj_t *page_music_create(lv_obj_t *parent)
{
    /*
     * 相机和音频互斥（工程约定，也是硬件限制：两者抢内部 DMA RAM）。
     * 正常情况下进这一页时相机页已经销毁、相机也停了，这里是道保险——
     * 万一哪天多出别的路径能带着相机进来，至少不会两边一起跑。
     */
    if (app_camera_is_running()) {
        ESP_LOGW(TAG, "相机还在跑，先停掉（相机与 MP3 不能同时工作）");
        app_camera_stop();
    }

    if (app_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频服务初始化失败");
    }
    app_audio_set_event_cb(on_audio_event, NULL);

    s_root = ui_page_shell_create_ex(parent, "Music", page_back_cb);
    lv_obj_t *content = ui_page_shell_content(s_root);

    /*
     * 标题栏右侧挂删除按钮：外壳的第 0 个子对象是标题栏，里面第 0 个是返回键、
     * 第 1 个是标题文本（和相册页取标题的方式一样）。
     * 全选按钮一起建好但默认隐藏，只在选择模式露面——平时两个按钮并排会把
     * 标题挤没，相册页那版就是这么被删掉的。
     */
    lv_obj_t *bar = lv_obj_get_child(s_root, 0);
    if (bar != NULL) {
        s_title_lbl = lv_obj_get_child(bar, 1);

        s_del_btn = lv_btn_create(bar);
        lv_obj_set_size(s_del_btn, 40, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_del_btn, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_bg_color(s_del_btn, UI_COLOR_DANGER, 0);
        lv_obj_set_style_radius(s_del_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_del_btn, 0, 0);
        lv_obj_add_event_cb(s_del_btn, del_clicked_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *dbl = lv_label_create(s_del_btn);
        lv_label_set_text(dbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(dbl, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(dbl, lv_color_white(), 0);
        lv_obj_center(dbl);

        /* Email 在 Delete 左边，和相册页一致 */
        s_mail_btn = lv_btn_create(bar);
        lv_obj_set_size(s_mail_btn, 40, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_mail_btn, LV_ALIGN_RIGHT_MID, -50, 0);
        lv_obj_set_style_bg_color(s_mail_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(s_mail_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_mail_btn, 0, 0);
        lv_obj_add_event_cb(s_mail_btn, mail_clicked_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ml = lv_label_create(s_mail_btn);
        lv_label_set_text(ml, LV_SYMBOL_ENVELOPE);
        lv_obj_set_style_text_font(ml, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(ml, lv_color_white(), 0);
        lv_obj_center(ml);

        s_all_btn = lv_btn_create(bar);
        lv_obj_set_size(s_all_btn, 44, UI_TITLE_BAR_H - 8);
        lv_obj_align(s_all_btn, LV_ALIGN_RIGHT_MID, -94, 0);
        lv_obj_set_style_bg_color(s_all_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_radius(s_all_btn, UI_RADIUS_BTN, 0);
        lv_obj_set_style_shadow_width(s_all_btn, 0, 0);
        lv_obj_add_flag(s_all_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_all_btn, all_clicked_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *al = lv_label_create(s_all_btn);
        lv_label_set_text(al, "All");
        lv_obj_set_style_text_font(al, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(al, lv_color_white(), 0);
        lv_obj_center(al);
    }

    /* 曲目列表：扫描是一次 readdir，几百毫秒内的事，和相册页一样同步做 */
    app_audio_scan(APP_MUSIC_DIR);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    s_list = list;
    build_list(list);

    build_now_bar(content);

    /* 列表建好才知道有没有曲目，删除按钮的可用性据此定 */
    update_buttons();
    s_shown_index = -2;
    refresh_ui();
    s_ui_timer = lv_timer_create(ui_timer_cb, UI_REFRESH_MS, NULL);
    return s_root;
}

void page_music_destroy(void)
{
    /* 顺序照 page_camera.c：先注销回调（解码任务随时可能回调进来），
     * 再停播放，最后清指针 */
    app_audio_set_event_cb(NULL, NULL);
    app_audio_stop();
    big_close();

    if (s_ui_timer != NULL) {
        lv_timer_del(s_ui_timer);
        s_ui_timer = NULL;
    }
    if (s_disc_timer != NULL) {
        lv_timer_del(s_disc_timer);
        s_disc_timer = NULL;
    }

    /* 到这里界面上的控件马上就要被 ui.c 删掉了，别再碰它们 */
    s_root = NULL;
    s_list = NULL;
    s_now_name = NULL;
    s_btn_now_play = NULL;
    /* 选择模式的指针也都指向刚被销毁的控件，一起清掉 */
    s_title_lbl = NULL;
    s_del_btn = NULL;
    s_mail_btn = NULL;
    s_all_btn = NULL;
    s_select_mode = false;
    s_selected_count = 0;
    s_shown_index = -2;
}
