/*
 * page_pcmon.c — 副屏页
 *
 * 一屏放下上位机的系统状态和 API 用量，**左右分栏**：左边系统（CPU / 内存 /
 * GPU），右边 API 用量（Tokens / 已消费 / 余额）；**网络上下行单独放最底下**，
 * 一个贴左、一个贴右，对称。
 *
 * 为什么分栏：CPU/内存 和 API 用量这两组数据没有关系，上下排会让人顺着往下读、
 * 以为是一组。为什么网速要单独提出来：它带单位、字符串最长，夹在栏里会把
 * 栏宽撑到别处放不下；提到整幅宽的底行之后，两栏都松快了。
 *
 * 屏幕只有 320x240，去掉标题栏和外壳 padding 之后内容区是 **304 x 190**，
 * 所以这一页不用 ui_list_row_create（那个行高 30 多），自己排紧凑行。
 *
 * **下面每个数字都是拿 tools/measure_font.py 量出来的，不是估的。** 改字号或
 * 改文案之前先量一遍——这块屏上"差 1.5px 就挤到标签上"是常态（见下面 Tokens
 * 那条注释）。超了不会报错，只会静静地多出个滚动条或者两个标签糊在一起。
 *
 *   竖：状态行 20 + 小节标题 22 + 3 行 × 30 + 网速行 30 = 162 ≤ 190
 *   横：左栏 130 + 间距 8 + 右栏 166 = 304
 *        左栏要塞 "RAM"(16) + "45.6%"(20) = 97.2
 *        右栏要塞 "Tokens"(16) + "99999999"(20) = 157.4  ← 全场最长的一行
 *        底行一侧要塞 "12345.6 KB/s"(20) + 箭头 ≈ 144 ≤ 152（半幅）
 *
 * 字号是 16（标签）/ 20（值）。**先按 24 做过一版，用户反馈"大了些"**，
 * 所以降回来一档——降完之后右栏还多出 30px 余量，Tokens 的压缩阈值也跟着
 * 从七位放宽到九位（见 set_tokens）。
 */
#include "page_pcmon.h"

#include <stdarg.h>
#include <stdio.h>


#include "esp_log.h"

#include "app_pcmon.h"
#include "ui.h"
#include "ui_theme.h"

static const char *TAG = "page_pcmon";

#define STATUS_H    20
#define SECTION_H   22
#define ROW_H       30
#define NET_H       30
#define COL_L_W     130
#define COL_R_W     166
#define COL_GAP     8
#define REFRESH_MS  500

static lv_obj_t *s_status;
static lv_obj_t *s_row_cpu, *s_row_ram, *s_row_gpu;
static lv_obj_t *s_row_tokens, *s_row_cost, *s_row_balance;
static lv_obj_t *s_net_down, *s_net_up;
static lv_timer_t *s_timer;

/*---------------------------------------------------------------------------
 * 紧凑行：左边标签（16 号），右边值（20 号，右对齐）
 *-------------------------------------------------------------------------*/
static lv_obj_t *add_row(lv_obj_t *parent, const char *label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT_SUB, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "--");
    /* 值比标签大一号：这一页要看的是数字，标签只是索引 */
    lv_obj_set_style_text_font(v, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    /* 和 ui_list_row_create 一个套路：值文本挂在行的 user_data 上，好找 */
    lv_obj_set_user_data(row, v);

    return row;
}

static void row_set(lv_obj_t *row, lv_color_t color, const char *fmt, ...)
{
    lv_obj_t *v = (lv_obj_t *)lv_obj_get_user_data(row);
    if (v == NULL) {
        return;
    }
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(v, buf);
    lv_obj_set_style_text_color(v, color, 0);
}

/* 负载高时变色，和上位机那个进度条一套（80 红 / 60 橙） */
static lv_color_t load_color(float pct)
{
    if (pct >= 80.0f) {
        return UI_COLOR_DANGER;
    }
    if (pct >= 60.0f) {
        return lv_color_hex(0xFF9500);
    }
    return UI_COLOR_TEXT;
}

/*
 * Tokens 那一行只有 8 个字符位，超了才压成 3 位有效数字 + 10 的次方。
 *
 * 8 是量出来的：字号从 24 降回 20 之后，166px 的右栏能塞下八位
 * （"99999999" 约 99px + 标签 "Tokens"(16 号) 58px = 157px）；九位
 * （约 111px + 58px = 169px）就顶到标签上了。**8 位以内一律照原样打整数**
 * ——用户要的是看见用量，只在真放不下时才换记法。
 *
 * ⚠️ **用 C 库的 `%.2e`，不要自己算位数。** 自己归一化最容易在"尾数进位"
 * 上翻车：999999999 先按 9 位数取指数 8，尾数 9.99999999 用 `%.2f` 一舍入
 * 就成了 10.00，拼出来 `10.00e+08`——既不是规范写法（尾数该在 [1,10)），
 * 又正好 9 个字符、把 8 个字符位的上限撑破。根因是"先定指数、再舍入尾数"，
 * 而舍入结果可能又跨一个数量级。C 库的 `%.2e` 是先规范化、再舍入、发现
 * 进位就重新规范化，直接给 `1.00e+09`，并且稳定 8 个字符。
 *
 * **余额和已消费从来不压缩**，那两个是要精确看的。
 */
static void set_tokens(lv_obj_t *row, long long tokens)
{
    if (tokens < 100000000LL) {         /* 8 位以内，原样打 */
        row_set(row, UI_COLOR_TEXT, "%lld", tokens);
    } else {
        /* row_set 走的是 C 库的 vsnprintf，%e 没问题。
         * （别换成 lv_label_set_text_fmt：那个不支持浮点，会打出字面的 "f"） */
        row_set(row, UI_COLOR_TEXT, "%.2e", (double)tokens);
    }
}

/*
 * 底行的网速。**符号和数字一起写**——底行这两个是普通的 label，不是
 * add_row 那种"标签 + 值"的两段式（那样两侧对不齐）。
 *
 * 1000 KB/s 以上换算成 MB/s：一是好读，二是 "12345.6 KB/s" 这种四位数加
 * 小数点会把底行两侧顶到一起（两侧合起来只有 304px，各占一半）。
 *
 * ⚠️ **这里必须用 C 库的 snprintf，不能用 lv_label_set_text_fmt()。**
 * LVGL 自带的那套 printf 默认**不支持 `%f`**（`CONFIG_LV_SPRINTF_USE_FLOAT`
 * 没开，见 `lv_printf.c` 里那个被 `#if` 包住的 `case 'f'`），遇到 `%.1f` 会把
 * 转换符本身当普通字符打出来——屏幕上就是**一行字面上写着 "f KB/s" 的网速**，
 * 数值整个不见。看着像数据没收到，其实数都在，只是格式化的那一步被吃掉了。
 *
 * 同一个坑在本文件里不会踩到别的行：`row_set()` 走的就是 vsnprintf。
 * LVGL 那套支持的是 `%d %i %u %x %X %o %b %c %s %p %%`——不带浮点。
 */
static void set_net(lv_obj_t *lbl, const char *sym, bool have, float kbps)
{
    char buf[32];
    if (!have) {
        snprintf(buf, sizeof(buf), "%s --", sym);
    } else if (kbps >= 1000.0f) {
        snprintf(buf, sizeof(buf), "%s %.1f MB/s", sym, (double)kbps / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%s %.1f KB/s", sym, (double)kbps);
    }
    lv_label_set_text(lbl, buf);
}

/*---------------------------------------------------------------------------
 * 刷新
 *-------------------------------------------------------------------------*/
static void clear_all(void)
{
    row_set(s_row_cpu, UI_COLOR_TEXT, "--");
    row_set(s_row_ram, UI_COLOR_TEXT, "--");
    row_set(s_row_gpu, UI_COLOR_TEXT, "--");
    row_set(s_row_tokens, UI_COLOR_TEXT, "--");
    row_set(s_row_cost, UI_COLOR_TEXT, "--");
    row_set(s_row_balance, UI_COLOR_TEXT, "--");
    set_net(s_net_down, LV_SYMBOL_DOWN, false, 0.0f);
    set_net(s_net_up, LV_SYMBOL_UP, false, 0.0f);
}

static void refresh(void)
{
    if (s_status == NULL) {
        return;
    }

    pcmon_state_t st;
    const bool any = app_pcmon_get(&st);

    if (!any) {
        /* 一帧都没收到过。区分"开关没开"和"上位机没在发"——两种都是这个
         * 画面，但提示语要能指路 */
        lv_label_set_text(s_status, app_pcmon_is_running()
                          ? "Waiting for PC..."
                          : "Status display is off");
        lv_obj_set_style_text_color(s_status, UI_COLOR_TEXT_SUB, 0);
        clear_all();
        return;
    }

    /*
     * 断线（age 超时）时**保留最后一帧的数值**，只把状态行变橙。
     * 最后一帧常常正是用户想看的（上位机关机前那一刻的用量），清成 "--"
     * 等于把唯一有用的数据扔了；而状态行已经明说了这组数不新鲜。
     */
    if (st.age_ms > APP_PCMON_STALE_MS) {
        lv_label_set_text_fmt(s_status, "Lost contact (%us)",
                              (unsigned)(st.age_ms / 1000));
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xFF9500), 0);
    } else {
        /* 上位机没发 host 时 st.host 是空的，直接拼会显示成"  live" */
        lv_label_set_text_fmt(s_status, "%s  %s",
                              st.host[0] ? st.host : "PC", "live");
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x34C759), 0);
    }

    row_set(s_row_cpu, load_color(st.cpu), "%.1f%%", (double)st.cpu);
    row_set(s_row_ram, load_color(st.ram), "%.1f%%", (double)st.ram);

    if (st.gpu < 0) {
        row_set(s_row_gpu, UI_COLOR_TEXT_SUB, "N/A");
    } else {
        row_set(s_row_gpu, load_color((float)st.gpu), "%d%%", st.gpu);
    }

    /* 箭头用 LV_SYMBOL_DOWN / LV_SYMBOL_UP：LVGL 内置的 montserrat 只覆盖
     * ASCII 加 FontAwesome 那一段，直接写 "↓↑"（U+2193/U+2191）是方框 */
    set_net(s_net_down, LV_SYMBOL_DOWN, true, st.net_down);
    set_net(s_net_up, LV_SYMBOL_UP, true, st.net_up);

    set_tokens(s_row_tokens, st.tokens);
    row_set(s_row_cost, UI_COLOR_TEXT, "%.2f", (double)st.cost);
    row_set(s_row_balance, UI_COLOR_TEXT, "%.2f", (double)st.balance);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

/*---------------------------------------------------------------------------
 * 页面
 *-------------------------------------------------------------------------*/
static lv_obj_t *make_column(lv_obj_t *parent, int w)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    return col;
}

static lv_obj_t *add_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_size(lbl, LV_PCT(100), SECTION_H);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_SUB, 0);
    return lbl;
}

lv_obj_t *page_pcmon_create(lv_obj_t *parent)
{
    lv_obj_t *shell = ui_page_shell_create(parent, "PC Status");
    lv_obj_t *content = ui_page_shell_content(shell);

    /* 这一页要按行精确排，不要外壳默认的 6px 行间距 */
    lv_obj_set_style_pad_row(content, 0, 0);

    s_status = lv_label_create(content);
    lv_obj_set_size(s_status, LV_PCT(100), STATUS_H);
    lv_label_set_text(s_status, "Waiting for PC...");
    lv_obj_set_style_text_font(s_status, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status, UI_COLOR_TEXT_SUB, 0);

    /* 两栏并排。用固定宽度而不是 flex_grow：宽度是量出来的（130 + 8 + 166 = 304），
     * flex_grow 会和 LV_SIZE_CONTENT 的高度相互依赖，排出来的东西不好预测 */
    lv_obj_t *cols = lv_obj_create(content);
    lv_obj_set_size(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_set_style_radius(cols, 0, 0);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_clear_flag(cols, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, COL_GAP, 0);

    /* 左栏：系统。电池那行按需求删掉了（台式机上它一直是 N/A，白占一行），
     * 上位机还在发这个字段，固件刻意不解析，见 app_pcmon.h */
    lv_obj_t *left = make_column(cols, COL_L_W);
    add_section(left, "SYSTEM");
    s_row_cpu = add_row(left, "CPU");
    s_row_ram = add_row(left, "RAM");
    s_row_gpu = add_row(left, "GPU");

    /* 右栏：API 用量 */
    lv_obj_t *right = make_column(cols, COL_R_W);
    add_section(right, "API (CNY)");
    s_row_tokens = add_row(right, "Tokens");
    s_row_cost = add_row(right, "Cost");
    s_row_balance = add_row(right, "Balance");

    /* 底行：网络上下行，一个贴左一个贴右，对称 */
    lv_obj_t *net = lv_obj_create(content);
    lv_obj_set_size(net, LV_PCT(100), NET_H);
    lv_obj_set_style_bg_opa(net, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(net, 0, 0);
    lv_obj_set_style_radius(net, 0, 0);
    lv_obj_set_style_pad_all(net, 0, 0);
    lv_obj_clear_flag(net, LV_OBJ_FLAG_SCROLLABLE);

    s_net_down = lv_label_create(net);
    lv_obj_set_style_text_font(s_net_down, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_net_down, UI_COLOR_TEXT, 0);
    lv_obj_align(s_net_down, LV_ALIGN_LEFT_MID, 0, 0);

    s_net_up = lv_label_create(net);
    lv_obj_set_style_text_font(s_net_up, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_net_up, UI_COLOR_TEXT, 0);
    lv_obj_align(s_net_up, LV_ALIGN_RIGHT_MID, 0, 0);

    s_timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
    refresh();

    ESP_LOGI(TAG, "pc status page created");
    return shell;
}

void page_pcmon_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_status = NULL;
    s_row_cpu = s_row_ram = s_row_gpu = NULL;
    s_row_tokens = s_row_cost = s_row_balance = NULL;
    s_net_down = s_net_up = NULL;
}
