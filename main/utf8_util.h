// main/utf8_util.h —— UTF-8 截断与字形净化（纯逻辑，可 host 测试）。
//
// Template / 用户身份名称和转写文本来自服务端，可能含字库外的字符（生僻字、emoji）。
// LVGL 遇到缺字会画占位框或空白，因此显示前把字库外字符替换成 '?'，并在码点
// 边界上截断，避免半个汉字导致乱码。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 解码 *p 处的一个码点并前移；非法序列返回 0xFFFD 并前移 1 字节。到达 NUL 返回 0。
uint32_t utf8_next(const char **p);

// 把 s 截断到最多 max_bytes 字节（不含 NUL），只在完整码点处截断。返回新长度。
size_t utf8_truncate(char *s, size_t max_bytes);

// 原地替换：控制字符（含换行）→ ' '；has(cp) 为 false 的码点 → '?'。返回新长度。
typedef bool (*utf8_has_glyph_fn)(uint32_t cp, void *user);
size_t utf8_sanitize(char *s, utf8_has_glyph_fn has, void *user);

// 统计码点数。
size_t utf8_count(const char *s);
