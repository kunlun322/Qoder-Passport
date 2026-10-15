// main/utf8_util.c —— 见 utf8_util.h。
#include "utf8_util.h"

#include <string.h>

uint32_t utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    const unsigned char c = p[0];
    if (c == 0) return 0;
    int n;
    uint32_t cp;
    if (c < 0x80) { *pp += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *pp += 1; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return cp;
}

size_t utf8_truncate(char *s, size_t max_bytes) {
    size_t len = strlen(s);
    if (len <= max_bytes) return len;
    size_t cut = max_bytes;
    // 回退到码点起始字节（非 10xxxxxx）。
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    s[cut] = '\0';
    return cut;
}

static size_t put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_sanitize(char *s, utf8_has_glyph_fn has, void *user) {
    const char *r = s;
    char *w = s;
    for (;;) {
        const char *start = r;
        const uint32_t cp = utf8_next(&r);
        if (cp == 0) break;
        if (cp < 0x20 || cp == 0x7F) {
            *w++ = ' ';
        } else if (cp == 0xFFFD && (r - start) == 1) {
            *w++ = '?';
        } else if (has && !has(cp, user)) {
            *w++ = '?';
        } else {
            // 写回的字节数不会超过读出的字节数（同码点同编码），原地安全。
            w += put_utf8(w, cp);
        }
    }
    *w = '\0';
    return (size_t)(w - s);
}

size_t utf8_count(const char *s) {
    size_t n = 0;
    while (utf8_next(&s) != 0) n++;
    return n;
}
