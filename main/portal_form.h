// main/portal_form.h —— 热点配置页用到的纯逻辑：表单解码、输入校验、JSON 转义。
//
// 不依赖 ESP-IDF，tests/test_qca_logic.c 在主机上测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 从 application/x-www-form-urlencoded 正文中取 key 的值并做 URL 解码（'+'→空格，%XX）。
// 返回解码后的字节数；键不存在、编码非法、含 NUL 或超出 cap-1 时返回 -1。
int form_get(const char *body, size_t body_len, const char *key, char *out, size_t cap);

// 去掉首尾 ASCII 空白（空格、\t、\r、\n），原地修改。
void form_trim(char *s);

// SSID：1..32 字节。
bool form_ssid_valid(const char *ssid);

// Wi-Fi 密码：空（开放网络）、8..63 字节口令，或 64 位十六进制 PSK。
bool form_wifi_pass_valid(const char *pass);

// PAT：以 pt- 开头，8..max_len 个可见 ASCII 字符（0x21..0x7E），不含空白与控制字符，
// 避免拼进 HTTP 头时被注入换行。
bool form_pat_valid(const char *pat, size_t max_len);

// 把 in[0..in_len) 转义为 JSON 字符串内容（不含两侧引号）：" \ 与控制字符被转义，
// 其余字节原样输出。保证不截断半个转义序列；返回写入的字节数（不含 '\0'）。
size_t form_json_escape(char *out, size_t cap, const char *in, size_t in_len);
