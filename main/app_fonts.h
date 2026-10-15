// main/app_fonts.h —— 中文字库与显示文本净化。
//
// app_font_cjk_16：思源黑体 SC（Noto Sans SC）Medium 16px，ASCII + GB2312 全部汉字/符号，
//                  回退到 Montserrat 14 以显示 LV_SYMBOL_* 图标。
// app_font_status_28：28px 大字，仅含状态词（聆听中/思考中/…）与数字，回退到 16px。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

void app_fonts_init(void);
const lv_font_t *app_font_text(void);
const lv_font_t *app_font_big(void);

// 16px 字库是否含该码点（不含回退、不含占位符）。
bool app_font_has(uint32_t cp, void *user);

// 截断到 max_bytes（码点边界）并把字库外字符替换为 '?'。返回新长度。
size_t app_text_sanitize(char *s, size_t max_bytes);
