// main/app_fonts.c —— 见 app_fonts.h。
#include "app_fonts.h"

#include "utf8_util.h"

LV_FONT_DECLARE(app_font_cjk_16);
LV_FONT_DECLARE(app_font_status_28);

// 生成的字库是 const，fallback 需在运行时副本上设置。
static lv_font_t s_text;
static lv_font_t s_big;
static bool s_ready;

void app_fonts_init(void) {
    if (s_ready) return;
    s_text = app_font_cjk_16;
    s_text.fallback = &lv_font_montserrat_14;
    s_big = app_font_status_28;
    s_big.fallback = &s_text;
    s_ready = true;
}

const lv_font_t *app_font_text(void) { return &s_text; }
const lv_font_t *app_font_big(void) { return &s_big; }

bool app_font_has(uint32_t cp, void *user) {
    (void)user;
    if (cp == ' ' ) return true;
    lv_font_glyph_dsc_t dsc;
    // 直接调用字库自身的查找函数：不走 fallback，也不会返回占位符。
    return app_font_cjk_16.get_glyph_dsc(&app_font_cjk_16, &dsc, cp, 0);
}

size_t app_text_sanitize(char *s, size_t max_bytes) {
    utf8_truncate(s, max_bytes);
    return utf8_sanitize(s, app_font_has, NULL);
}
