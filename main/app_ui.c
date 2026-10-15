// main/app_ui.c —— 见 app_ui.h。
//
// 视觉语言：深海军蓝底 + 单一状态色。语音页上部是一行紧凑的状态条：左侧小“呼吸环”
// (颜色表示状态，粗细随电平跳动)，右侧大字状态词；其下一行告诉你“现在该怎么做”。
// 页面下半部留给对话卡片，多行显示最近一句转写。
#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include "app_fonts.h"
#include "app_portal.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"
#include "qca_voice.h"
#include "voice_logic.h"

#define SCR_W 240
#define SCR_H 320
#define PAD   16

#define C_BG        0x0B1020
#define C_CARD      0x161D33
#define C_CARD_SEL  0x123B3F
#define C_TEXT      0xE2E8F0
#define C_DIM       0x94A3B8
#define C_FAINT     0x475569
#define C_TEAL      0x2DD4BF
#define C_VIOLET    0xA78BFA
#define C_SKY       0x38BDF8
#define C_AMBER     0xF59E0B
#define C_RED       0xF87171

static lv_obj_t *s_scr;
static lv_obj_t *s_batt;
static lv_timer_t *s_batt_timer;
static lv_timer_t *s_page_timer;   // 当前页的轮询定时器（语音 / 配网）

// 列表页
static lv_obj_t *s_list_counter;
static lv_obj_t *s_rows[APP_UI_LIST_ROWS];
static lv_obj_t *s_row_idx[APP_UI_LIST_ROWS];
static lv_obj_t *s_row_name[APP_UI_LIST_ROWS];

// 语音页
static lv_obj_t *s_halo;
static lv_obj_t *s_ring;
static lv_obj_t *s_disc;
static int s_cx, s_cy;          // 呼吸圈圆心
static lv_obj_t *s_word;
static lv_obj_t *s_guide;
static lv_obj_t *s_line;
static vs_state_t s_last_state;
static bool s_last_finished;
static uint32_t s_last_line_seq;
static uint32_t s_toast_until;
static int s_backlight = -1;
static int s_level_smooth;

// 配网页
static lv_obj_t *s_prov_status;
static portal_stage_t s_prov_last;

static uint32_t state_color(vs_state_t st) {
    switch (st) {
    case VS_LISTENING: return C_TEAL;
    case VS_THINKING:  return C_VIOLET;
    case VS_SPEAKING:  return C_SKY;
    case VS_PAUSED:    return C_AMBER;
    case VS_ERROR:     return C_RED;
    case VS_ENDED:     return C_FAINT;
    default:           return C_DIM;
    }
}

static uint32_t tone_color(ui_tone_t t) {
    switch (t) {
    case UI_TONE_OK:    return C_TEAL;
    case UI_TONE_WARN:  return C_AMBER;
    case UI_TONE_ERROR: return C_RED;
    default:            return C_SKY;
    }
}

static void set_backlight(int pct) {
    if (pct == s_backlight) return;
    s_backlight = pct;
    bsp_display_backlight((uint8_t)pct);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// ---------- 电池 ----------
static void battery_refresh(void) {
    if (!s_batt) return;
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *sym = soc >= 85 ? LV_SYMBOL_BATTERY_FULL
                    : soc >= 60 ? LV_SYMBOL_BATTERY_3
                    : soc >= 35 ? LV_SYMBOL_BATTERY_2
                    : soc >= 12 ? LV_SYMBOL_BATTERY_1
                                : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(s_batt, "%d%% %s", soc, sym);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(soc < 15 ? C_RED : C_DIM), 0);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
}

static void battery_timer_cb(lv_timer_t *t) {
    (void)t;
    battery_refresh();
}

// ---------- 页面框架 ----------
static void stop_page_timer(void) {
    if (s_page_timer) {
        lv_timer_delete(s_page_timer);
        s_page_timer = NULL;
    }
}

static lv_obj_t *page_begin(void) {
    stop_page_timer();
    s_halo = s_ring = s_disc = s_word = s_guide = s_line = NULL;
    s_prov_status = NULL;
    s_list_counter = NULL;
    memset(s_rows, 0, sizeof(s_rows));

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, app_font_text(), 0);

    // 顶栏：品牌色小方块 + 标题 + 电量。左右各留约 20px，避开 30px 圆角
    // （顶栏位于 y=8..28，圆角在该高度只吃掉左右约 4~10px）。
    // 标题“Qoder Passport”约 119px：x=34..153，电量 “100% + 电池图标” 约 55px 靠右到 x=220。
    lv_obj_t *mark = mk_box(scr, 8, 8, C_TEAL, 2);
    lv_obj_set_pos(mark, PAD + 4, 15);
    lv_obj_t *title = mk_label(scr, app_font_text(), C_TEXT, "Qoder Passport");
    lv_obj_set_pos(title, PAD + 18, 8);

    s_batt = mk_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -(PAD + 4), 11);
    battery_refresh();

    lv_obj_t *rule = mk_box(scr, SCR_W - 2 * PAD, 1, C_CARD, 0);
    lv_obj_set_pos(rule, PAD, 34);
    return scr;
}

static void page_commit(lv_obj_t *scr) {
    lv_obj_t *old = s_scr;
    lv_screen_load(scr);
    s_scr = scr;
    if (old) lv_obj_delete(old);
}

void app_ui_init(void) {
    app_fonts_init();
    if (!bsp_lvgl_lock(1000)) return;
    s_batt_timer = lv_timer_create(battery_timer_cb, 10000, NULL);
    set_backlight(100);
    bsp_lvgl_unlock();
}

// ---------- 提示页 ----------
void app_ui_show_message(ui_tone_t tone, const char *title, const char *body,
                         const char *hint, bool spinner) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    const uint32_t c = tone_color(tone);

    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(scr);
        lv_spinner_set_anim_params(sp, 1000, 270);
        lv_obj_set_size(sp, 64, 64);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, lv_color_hex(c), LV_PART_INDICATOR);
        lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 72);
    } else {
        lv_obj_t *halo = mk_box(scr, 64, 64, c, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(halo, LV_OPA_20, 0);
        lv_obj_align(halo, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_t *dot = mk_box(halo, 24, 24, c, LV_RADIUS_CIRCLE);
        lv_obj_center(dot);
    }

    lv_obj_t *t = mk_label(scr, app_font_text(), C_TEXT, title);
    lv_obj_set_width(t, SCR_W - 2 * PAD - 8);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 152);

    lv_obj_t *b = mk_label(scr, app_font_text(), C_DIM, body);
    lv_obj_set_width(b, SCR_W - 2 * PAD - 8);
    lv_obj_set_height(b, 100);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(b, 2, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 182);

    if (hint && hint[0]) {
        lv_obj_t *h = mk_label(scr, app_font_text(), C_TEAL, hint);
        lv_obj_set_width(h, SCR_W - 2 * PAD - 16);
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
    page_commit(scr);
    bsp_lvgl_unlock();
}

// ---------- 选择页 ----------
#define ROW_Y0  70
#define ROW_H   38
#define ROW_GAP 6

static void list_render(const qca_list_t *list, const app_list_t *cur) {
    if (!s_rows[0]) return;
    lv_label_set_text_fmt(s_list_counter, "%d/%d", list->count ? cur->cursor + 1 : 0, list->count);
    for (int r = 0; r < APP_UI_LIST_ROWS; r++) {
        const int i = cur->top + r;
        if (i >= list->count) {
            lv_obj_add_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
        const bool sel = (i == cur->cursor);
        lv_obj_set_style_bg_color(s_rows[r], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_rows[r], sel ? 2 : 0, 0);
        lv_label_set_text_fmt(s_row_idx[r], "%02d", i + 1);
        lv_obj_set_style_text_color(s_row_idx[r], lv_color_hex(sel ? C_TEAL : C_FAINT), 0);
        lv_obj_set_style_text_color(s_row_name[r], lv_color_hex(sel ? C_TEXT : C_DIM), 0);
        // 选中行循环滚动显示全名，其余行省略号。
        lv_label_set_long_mode(s_row_name[r], sel ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                                  : LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(s_row_name[r], list->items[i].name);
    }
}

void app_ui_show_list(const char *title, const char *hint,
                      const qca_list_t *list, const app_list_t *cursor) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();

    lv_obj_t *t = mk_label(scr, app_font_text(), C_TEAL, title);
    lv_obj_set_width(t, 160);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(t, PAD + 4, 42);
    s_list_counter = mk_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_list_counter, LV_ALIGN_TOP_RIGHT, -(PAD + 4), 45);

    for (int r = 0; r < APP_UI_LIST_ROWS; r++) {
        lv_obj_t *row = mk_box(scr, SCR_W - 2 * PAD, ROW_H, C_CARD, 10);
        lv_obj_set_style_border_color(row, lv_color_hex(C_TEAL), 0);
        lv_obj_set_pos(row, PAD, ROW_Y0 + r * (ROW_H + ROW_GAP));
        s_rows[r] = row;
        s_row_idx[r] = mk_label(row, &lv_font_montserrat_14, C_FAINT, "");
        lv_obj_align(s_row_idx[r], LV_ALIGN_LEFT_MID, 10, 0);
        s_row_name[r] = mk_label(row, app_font_text(), C_DIM, "");
        lv_obj_set_width(s_row_name[r], SCR_W - 2 * PAD - 48);
        lv_obj_align(s_row_name[r], LV_ALIGN_LEFT_MID, 38, 0);
    }

    char h[96];
    snprintf(h, sizeof(h), LV_SYMBOL_UP LV_SYMBOL_DOWN " %s", hint ? hint : "");
    lv_obj_t *hl = mk_label(scr, app_font_text(), C_DIM, h);
    lv_obj_set_width(hl, SCR_W - 2 * PAD - 8);
    lv_label_set_long_mode(hl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(hl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hl, LV_ALIGN_BOTTOM_MID, 0, -10);

    list_render(list, cursor);
    page_commit(scr);
    bsp_lvgl_unlock();
}

void app_ui_list_update(const qca_list_t *list, const app_list_t *cursor) {
    if (!bsp_lvgl_lock(1000)) return;
    list_render(list, cursor);
    bsp_lvgl_unlock();
}

// ---------- 语音页 ----------
// 纵向布局（240x320，正文字体行高 20）：
//   0-34 顶栏 | 38 模板名 | 58 身份 | 82-146 状态条(呼吸圈 + 状态词) | 150 操作提示
//   | 174-286 对话卡片(5 行) | 290-310 底部按键提示
// 呼吸圈由三层同心圆组成，圆心固定，按状态改变直径与透明度：
//   光晕 halo（半透明实心，最外层）→ 环 ring（描边）→ 核心 disc（实心）。
#define HALO_MAX    64     // 状态区高度，光晕最大直径
#define RING_MIN    38
#define RING_MAX    56
#define DISC_D      26
#define STATUS_Y    82
#define WORD_W      84     // 28px 状态词最多 3 个字
#define STATUS_GAP  12
#define CARD_Y      174
#define CARD_H      112
#define CARD_PAD_X  8
#define CARD_PAD_Y  6
#define VOICE_TICK_MS 50

static int tri_wave(uint32_t ms, uint32_t period) {     // 0..100..0
    uint32_t p = ms % period;
    uint32_t half = period / 2;
    return (int)(p < half ? p * 100 / half : (period - p) * 100 / half);
}

// 平滑呼吸曲线 0..100..0：对三角波做 smoothstep，两端停顿、中段加速，比线性更像呼吸。
static int breath(uint32_t ms, uint32_t period) {
    const int t = tri_wave(ms, period);
    return t * t * (300 - 2 * t) / 10000;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 以 (s_cx, s_cy) 为圆心放置直径 d 的圆；直径取偶数避免圆心半像素抖动。
static void place_circle(lv_obj_t *o, int d) {
    d &= ~1;
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, s_cx - d / 2, s_cy - d / 2);
}

// 单行标签：固定为一行高，超宽末尾省略，绝不换行。
static void make_single_line(lv_obj_t *l, const lv_font_t *font, int w) {
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void voice_timer_cb(lv_timer_t *tm) {
    (void)tm;
    if (!s_ring) return;
    qca_voice_view_t v;
    qca_voice_get_view(&v);
    const uint32_t now = lv_tick_get();

    // 电平平滑：上升快、下降慢，环的跳动更自然。
    if (v.level > s_level_smooth) s_level_smooth = (s_level_smooth + v.level * 3) / 4;
    else s_level_smooth = (s_level_smooth * 7 + v.level) / 8;

    if (v.state != s_last_state || v.finished != s_last_finished) {
        s_last_state = v.state;
        s_last_finished = v.finished;
        const uint32_t c = state_color(v.state);
        lv_obj_set_style_border_color(s_ring, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(s_halo, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(s_disc, lv_color_hex(c), 0);
        lv_label_set_text(s_word, voice_state_word(v.state));
        lv_obj_set_style_text_color(s_word, lv_color_hex(c), 0);
        lv_label_set_text(s_guide, voice_state_guide(v.state));
        if (v.state == VS_ERROR && v.error[0]) {
            lv_label_set_text(s_line, v.error);
            lv_obj_set_style_text_color(s_line, lv_color_hex(C_RED), 0);
        }
        set_backlight(v.state == VS_PAUSED ? 30 : 100);
    }

    // 呼吸圈：b 为呼吸相位（0..100），lvl 为平滑后的电平（0..100）。
    //   聆听：1.8 s 深呼吸，环与光晕一起胀缩，说话时随音量再外扩；
    //   回答：1.1 s 较快节律，环随喇叭电平跳动，光晕随之亮起；
    //   思考：0.8 s 快速脉动；暂停/连接：3.6 s 浅而慢的呼吸；结束/出错：静止。
    const int lvl = s_level_smooth;
    int b, ring_d, ring_w, ring_opa, halo_d, halo_opa, disc_d, disc_opa;
    switch (v.state) {
    case VS_LISTENING:
        b = breath(now, 1800);
        ring_d = 40 + b * 10 / 100 + lvl * 6 / 100;
        ring_w = 2 + lvl * 4 / 100;
        ring_opa = 150 + b * 105 / 100;
        halo_d = ring_d + 4 + b * 8 / 100 + lvl * 4 / 100;
        halo_opa = 20 + b * 60 / 100 + lvl * 30 / 100;
        disc_d = DISC_D - 2 + b * 6 / 100;
        disc_opa = 60 + b * 110 / 100 + lvl * 60 / 100;
        break;
    case VS_SPEAKING:
        b = breath(now, 1100);
        ring_d = 42 + b * 6 / 100 + lvl * 10 / 100;
        ring_w = 3 + lvl * 3 / 100;
        ring_opa = 255;
        halo_d = ring_d + 6 + lvl * 6 / 100;
        halo_opa = 30 + b * 30 / 100 + lvl * 50 / 100;
        disc_d = DISC_D - 2 + b * 4 / 100 + lvl * 4 / 100;
        disc_opa = 110 + b * 60 / 100 + lvl * 60 / 100;
        break;
    case VS_THINKING:
        b = breath(now, 800);
        ring_d = 40 + b * 8 / 100;
        ring_w = 3;
        ring_opa = 120 + b * 135 / 100;
        halo_d = ring_d + 2 + b * 10 / 100;
        halo_opa = b * 70 / 100;
        disc_d = DISC_D - 2 + b * 4 / 100;
        disc_opa = 60 + b * 120 / 100;
        break;
    case VS_PAUSED:
    case VS_CONNECTING:
        b = breath(now, 3600);
        ring_d = 40 + b * 4 / 100;
        ring_w = 2;
        ring_opa = 90 + b * 120 / 100;
        halo_d = ring_d + 2;
        halo_opa = b * 25 / 100;
        disc_d = DISC_D - 2;
        disc_opa = 40 + b * 60 / 100;
        break;
    default:
        ring_d = 44;
        ring_w = 2;
        ring_opa = 120;
        halo_d = ring_d;
        halo_opa = 0;
        disc_d = DISC_D - 2;
        disc_opa = 50;
        break;
    }
    ring_d = clampi(ring_d, RING_MIN, RING_MAX);
    halo_d = clampi(halo_d, ring_d, HALO_MAX);
    disc_d = clampi(disc_d, 20, ring_d - 2 * ring_w - 4);
    place_circle(s_halo, halo_d);
    place_circle(s_ring, ring_d);
    place_circle(s_disc, disc_d);
    lv_obj_set_style_bg_opa(s_halo, (lv_opa_t)clampi(halo_opa, 0, 110), 0);
    lv_obj_set_style_border_width(s_ring, clampi(ring_w, 1, 6), 0);
    lv_obj_set_style_border_opa(s_ring, (lv_opa_t)clampi(ring_opa, 0, 255), 0);
    lv_obj_set_style_bg_opa(s_disc, (lv_opa_t)clampi(disc_opa, 0, 230), 0);

    // 转写行：toast 优先；之后显示最近一句。
    if (s_toast_until && (int32_t)(now - s_toast_until) >= 0) {
        s_toast_until = 0;
        s_last_line_seq = v.line_seq - 1;   // 强制恢复显示最近一句
    }
    if (!s_toast_until && v.line_seq != s_last_line_seq && v.state != VS_ERROR) {
        s_last_line_seq = v.line_seq;
        if (v.line[0]) {
            lv_label_set_text_fmt(s_line, "%s%s", v.line_is_user ? "你：" : "助手：", v.line);
        } else {
            lv_label_set_text(s_line, "");
        }
        lv_obj_set_style_text_color(s_line, lv_color_hex(v.line_is_user ? C_TEAL : C_DIM), 0);
    }
}

void app_ui_show_voice(const char *template_name, const char *identity_name) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();

    lv_obj_t *who = mk_label(scr, app_font_text(), C_TEXT, template_name);
    lv_obj_set_size(who, SCR_W - 2 * PAD - 8, lv_font_get_line_height(app_font_text()));
    lv_label_set_long_mode(who, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 38);
    char ident[QCA_NAME_MAX + 16];
    snprintf(ident, sizeof(ident), "身份：%s", identity_name);
    lv_obj_t *tpl = mk_label(scr, app_font_text(), C_FAINT, ident);
    make_single_line(tpl, app_font_text(), SCR_W - 2 * PAD - 8);
    lv_obj_align(tpl, LV_ALIGN_TOP_MID, 0, 58);

    // 状态条：呼吸圈（HALO_MAX 见方）与状态词作为一组水平居中；三层圆共用圆心。
    const int group_x = (SCR_W - (HALO_MAX + STATUS_GAP + WORD_W)) / 2;
    s_cx = group_x + HALO_MAX / 2;
    s_cy = STATUS_Y + HALO_MAX / 2;

    s_halo = mk_box(scr, RING_MIN, RING_MIN, C_DIM, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_halo, LV_OPA_TRANSP, 0);
    place_circle(s_halo, RING_MIN);

    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    place_circle(s_ring, RING_MIN + 2);

    s_disc = mk_box(scr, DISC_D, DISC_D, C_DIM, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_disc, LV_OPA_20, 0);
    place_circle(s_disc, DISC_D - 2);

    s_word = mk_label(scr, app_font_big(), C_DIM, voice_state_word(VS_CONNECTING));
    lv_obj_set_width(s_word, WORD_W);
    lv_obj_set_pos(s_word, group_x + HALO_MAX + STATUS_GAP, s_cy - 14);

    s_guide = mk_label(scr, app_font_text(), C_TEXT, voice_state_guide(VS_CONNECTING));
    make_single_line(s_guide, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(s_guide, LV_ALIGN_TOP_MID, 0, STATUS_Y + HALO_MAX + 4);

    // 对话卡片：多行显示最近一句转写/提示，超出 5 行末尾省略。
    lv_obj_t *card = mk_box(scr, SCR_W - 2 * PAD, CARD_H, C_CARD, 12);
    lv_obj_set_pos(card, PAD, CARD_Y);
    s_line = mk_label(card, app_font_text(), C_DIM, "");
    lv_obj_set_size(s_line, SCR_W - 2 * PAD - 2 * CARD_PAD_X, CARD_H - 2 * CARD_PAD_Y);
    lv_label_set_long_mode(s_line, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_line, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_line, CARD_PAD_X, CARD_PAD_Y);

    // 底部提示约 198px，给满 208px 宽并固定为单行，避免折行。
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_FAINT, "OK 暂停/继续  长按 OK 结束");
    make_single_line(hint, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_last_state = VS_CONNECTING;
    s_last_finished = false;
    s_last_line_seq = 0;
    s_toast_until = 0;
    s_level_smooth = 0;
    page_commit(scr);
    s_page_timer = lv_timer_create(voice_timer_cb, VOICE_TICK_MS, NULL);
    bsp_lvgl_unlock();
}

void app_ui_voice_toast(const char *text) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_line) {
        lv_label_set_text(s_line, text);
        lv_obj_set_style_text_color(s_line, lv_color_hex(C_AMBER), 0);
        s_toast_until = lv_tick_get() + 1500;
        if (!s_toast_until) s_toast_until = 1;
    }
    bsp_lvgl_unlock();
}

// ---------- 热点配置页 ----------
static void prov_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_prov_status) return;
    const portal_stage_t st = app_portal_stage();
    if (st == s_prov_last) return;
    s_prov_last = st;
    lv_label_set_text(s_prov_status, app_portal_text(st));
    const bool failed = (st >= PORTAL_WIFI_FAILED && st <= PORTAL_SAVE_FAILED) || st == PORTAL_ERROR;
    lv_obj_set_style_text_color(s_prov_status,
        lv_color_hex(st == PORTAL_DONE ? C_TEAL : failed ? C_RED : C_AMBER), 0);
}

void app_ui_show_provision(void) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *t = mk_label(scr, app_font_text(), C_TEAL, "配置网络与 PAT");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 45);

    if (app_portal_stage() != PORTAL_ERROR) {
        // 只包含临时热点连接信息，不包含路由器密码或 PAT；禁止记录二维码内容。
        char data[96];
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:%s;P:%s;;",
                 app_portal_ssid(), app_portal_password());
        lv_obj_t *qr = lv_qrcode_create(scr);
        lv_qrcode_set_size(qr, 112);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_update(qr, data, strlen(data));
        memset(data, 0, sizeof(data));
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_t *ssid = mk_label(scr, &lv_font_montserrat_14, C_TEXT, app_portal_ssid());
        lv_obj_align(ssid, LV_ALIGN_TOP_MID, 0, 194);
        char password[64];
        snprintf(password, sizeof(password), "热点密码：%s", app_portal_password());
        lv_obj_t *pw = mk_label(scr, app_font_text(), C_DIM, password);
        lv_obj_align(pw, LV_ALIGN_TOP_MID, 0, 215);
        memset(password, 0, sizeof(password));
        lv_obj_t *url = mk_label(scr, app_font_text(), C_SKY, "浏览器：192.168.4.1");
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 239);
    }
    s_prov_status = mk_label(scr, app_font_text(), C_AMBER, app_portal_text(app_portal_stage()));
    make_single_line(s_prov_status, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_set_style_text_align(s_prov_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prov_status, LV_ALIGN_TOP_MID, 0, 265);
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_FAINT, "长按 OK 进入设备设置");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_prov_last = (portal_stage_t)-1;
    page_commit(scr);
    s_page_timer = lv_timer_create(prov_timer_cb, 200, NULL);
    bsp_lvgl_unlock();
}

// ---------- 设备设置 ----------
static void settings_render(int selected) {
    for (int i = 0; i < 4; ++i) {
        if (!s_rows[i]) continue;
        const bool sel = i == selected;
        lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_rows[i], sel ? 2 : 0, 0);
    }
}

void app_ui_show_settings(bool provisioning, int selected) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *title = mk_label(scr, app_font_text(), C_TEAL, "设备设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_t *guide = mk_label(scr, app_font_text(), C_DIM, "修改配置或恢复出厂");
    lv_obj_align(guide, LV_ALIGN_TOP_MID, 0, 73);
    const char *items[] = { provisioning ? "取消配置并重启" : "刷新 Agent 列表",
        "重新配置 Wi-Fi / PAT", "恢复出厂设置", "返回上一页" };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *row = mk_box(scr, SCR_W - 2 * PAD, 36, C_CARD, 10);
        lv_obj_set_style_border_color(row, lv_color_hex(C_TEAL), 0);
        lv_obj_set_pos(row, PAD, 104 + i * 43);
        s_rows[i] = row;
        lv_obj_t *label = mk_label(row, app_font_text(), i == 2 ? C_RED : C_TEXT, items[i]);
        lv_obj_center(label);
    }
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_DIM,
        LV_SYMBOL_UP LV_SYMBOL_DOWN " 选择  OK 确认  长按返回");
    make_single_line(hint, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    settings_render(selected);
    page_commit(scr);
    bsp_lvgl_unlock();
}

void app_ui_settings_update(int selected) {
    if (!bsp_lvgl_lock(1000)) return;
    settings_render(selected);
    bsp_lvgl_unlock();
}

void app_ui_show_reset_confirm(bool confirm_selected) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *title = mk_label(scr, app_font_text(), C_RED, "恢复出厂设置？");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_t *body = mk_label(scr, app_font_text(), C_TEXT,
        "删除 Wi-Fi 和 PAT\n恢复默认音量\n不删除云端会话\n重启后重新配置");
    lv_obj_set_width(body, SCR_W - 2 * PAD);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(body, 4, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 83);
    const char *items[] = { "取消，保留设置", "确认清除并重启" };
    for (int i = 0; i < 2; ++i) {
        const bool sel = (i == 1) == confirm_selected;
        lv_obj_t *row = mk_box(scr, SCR_W - 2 * PAD, 36, sel ? C_CARD_SEL : C_CARD, 10);
        lv_obj_set_style_border_color(row, lv_color_hex(i ? C_RED : C_TEAL), 0);
        lv_obj_set_style_border_width(row, sel ? 2 : 0, 0);
        lv_obj_set_pos(row, PAD, 196 + i * 46);
        lv_obj_t *label = mk_label(row, app_font_text(), i ? C_RED : C_TEXT, items[i]);
        lv_obj_center(label);
    }
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_DIM,
        confirm_selected ? "长按 OK 清除  " LV_SYMBOL_UP " 取消" : "OK 取消  " LV_SYMBOL_DOWN " 选择清除");
    make_single_line(hint, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    page_commit(scr);
    bsp_lvgl_unlock();
}
