// main/portal_form.c —— 见 portal_form.h。
#include "portal_form.h"

#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int form_get(const char *body, size_t body_len, const char *key, char *out, size_t cap) {
    if (!body || !key || !out || cap == 0) return -1;
    const size_t klen = strlen(key);
    size_t i = 0;
    while (i < body_len) {
        size_t end = i;
        while (end < body_len && body[end] != '&') end++;
        const char *eq = memchr(body + i, '=', end - i);
        const size_t name_len = eq ? (size_t)(eq - (body + i)) : end - i;
        if (name_len == klen && memcmp(body + i, key, klen) == 0) {
            size_t n = 0;
            for (size_t j = eq ? (size_t)(eq - body) + 1 : end; j < end; j++) {
                char c = body[j];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (j + 2 >= end) return -1;   // 截断的 %X
                    const int hi = hexval(body[j + 1]);
                    const int lo = hexval(body[j + 2]);
                    if (hi < 0 || lo < 0) return -1;
                    c = (char)(hi * 16 + lo);
                    j += 2;
                }
                if (c == '\0' || n + 1 >= cap) return -1;
                out[n++] = c;
            }
            out[n] = '\0';
            return (int)n;
        }
        i = end + 1;
    }
    return -1;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void form_trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && is_space(s[len - 1])) s[--len] = '\0';
    size_t start = 0;
    while (start < len && is_space(s[start])) start++;
    if (start) memmove(s, s + start, len - start + 1);
}

bool form_ssid_valid(const char *ssid) {
    if (!ssid) return false;
    const size_t n = strlen(ssid);
    return n >= 1 && n <= 32;
}

bool form_wifi_pass_valid(const char *pass) {
    if (!pass) return false;
    const size_t n = strlen(pass);
    if (n == 0) return true;
    if (n >= 8 && n <= 63) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; i++) {
        if (hexval(pass[i]) < 0) return false;
    }
    return true;
}

bool form_pat_valid(const char *pat, size_t max_len) {
    if (!pat) return false;
    const size_t n = strlen(pat);
    if (n < 8 || n > max_len || strncmp(pat, "pt-", 3) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)pat[i];
        if (c < 0x21 || c > 0x7E) return false;
    }
    return true;
}

size_t form_json_escape(char *out, size_t cap, const char *in, size_t in_len) {
    static const char hex[] = "0123456789abcdef";
    if (!out || cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        const unsigned char c = (unsigned char)in[i];
        char esc[7];
        size_t k = 0;
        if (c == '"' || c == '\\') {
            esc[k++] = '\\';
            esc[k++] = (char)c;
        } else if (c < 0x20) {
            esc[k++] = '\\';
            esc[k++] = 'u';
            esc[k++] = '0';
            esc[k++] = '0';
            esc[k++] = hex[c >> 4];
            esc[k++] = hex[c & 0xF];
        } else {
            esc[k++] = (char)c;
        }
        if (n + k >= cap) break;
        memcpy(out + n, esc, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}
