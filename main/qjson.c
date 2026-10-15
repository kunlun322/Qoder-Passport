// main/qjson.c —— 流式 JSON 扫描器实现。说明见 qjson.h。
#include "qjson.h"

#include <string.h>

// 词法状态。每个状态说明“下一个有意义的字符应该是什么”。
enum {
    ST_VALUE = 0,     // 期待一个值（根值、键后、数组元素）
    ST_OBJ_FIRST,     // 刚进入对象：期待键名 '"' 或 '}'
    ST_OBJ_KEY,       // 对象中逗号之后：只能是键名 '"'
    ST_COLON,         // 键名之后：期待 ':'
    ST_AFTER_VALUE,   // 值之后：期待 ',' 或容器结束符
    ST_ARR_FIRST,     // 刚进入数组：期待值或 ']'
    ST_STRING,        // 字符串内部（键或值）
    ST_ESCAPE,        // 字符串内反斜杠之后
    ST_UNICODE,       // \u 之后读取 4 位十六进制
    ST_PRIM,          // 原始值（数字/true/false/null）内部
    ST_DONE,          // 根值结束后：只允许空白
};

void qj_init(qj_parser_t *p, qj_cb_t cb, void *user) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->user = user;
    p->state = ST_VALUE;
}

bool qj_done(const qj_parser_t *p) { return p->done && !p->error; }

const char *qj_key(const qj_parser_t *p, int level) {
    if (level < 1 || level > p->depth || p->is_array[level - 1]) return "";
    return p->key[level - 1];
}

bool qj_in_array(const qj_parser_t *p, int level) {
    if (level < 1 || level > p->depth) return false;
    return p->is_array[level - 1];
}

bool qj_key_is(const qj_parser_t *p, const char *name) {
    return strcmp(qj_key(p, p->depth), name) == 0;
}

static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// 把已解码的字符串值字节交给回调（键名不走这里，键名单独缓存）。
static void flush_out(qj_parser_t *p) {
    if (p->out_len > 0) {
        p->cb(p->user, p, QJ_EV_STR_CHUNK, p->out, p->out_len);
        p->out_len = 0;
    }
}

// 追加一个已解码字节：键名进键缓冲（超长截断），值进输出缓冲（满即回调）。
static void put_byte(qj_parser_t *p, char c) {
    if (p->in_key) {
        if (p->key_len + 1 < QJ_KEY_MAX) {
            p->key[p->depth - 1][p->key_len++] = c;
            p->key[p->depth - 1][p->key_len] = '\0';
        }
        return;
    }
    if (p->out_len >= QJ_CHUNK_MAX) flush_out(p);
    p->out[p->out_len++] = c;
}

// 把一个 Unicode 码点编码成 UTF-8 追加。非法码点替换为 U+FFFD。
static void put_codepoint(qj_parser_t *p, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        put_byte(p, (char)cp);
    } else if (cp < 0x800) {
        put_byte(p, (char)(0xC0 | (cp >> 6)));
        put_byte(p, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put_byte(p, (char)(0xE0 | (cp >> 12)));
        put_byte(p, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_byte(p, (char)(0x80 | (cp & 0x3F)));
    } else {
        put_byte(p, (char)(0xF0 | (cp >> 18)));
        put_byte(p, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put_byte(p, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_byte(p, (char)(0x80 | (cp & 0x3F)));
    }
}

// 一个值（字符串/原始值/容器）结束后的状态推进。
static void value_finished(qj_parser_t *p) {
    if (p->depth == 0) {
        p->state = ST_DONE;
        p->done = true;
    } else {
        p->state = ST_AFTER_VALUE;
    }
}

static bool push(qj_parser_t *p, bool is_array) {
    if (p->depth >= QJ_MAX_DEPTH) return false;
    p->is_array[p->depth] = is_array;
    p->key[p->depth][0] = '\0';
    p->depth++;
    p->cb(p->user, p, is_array ? QJ_EV_ARR_BEGIN : QJ_EV_OBJ_BEGIN, NULL, 0);
    return true;
}

static bool pop(qj_parser_t *p, bool is_array) {
    if (p->depth == 0 || p->is_array[p->depth - 1] != is_array) return false;
    p->cb(p->user, p, is_array ? QJ_EV_ARR_END : QJ_EV_OBJ_END, NULL, 0);
    p->depth--;
    value_finished(p);
    return true;
}

static void prim_end(qj_parser_t *p) {
    p->prim[p->prim_len] = '\0';
    p->cb(p->user, p, QJ_EV_PRIM, p->prim, p->prim_len);
    p->prim_len = 0;
    value_finished(p);
}

// 处理“期待值”位置上的一个字符。返回 false 表示语法错误。
static bool begin_value(qj_parser_t *p, char c) {
    if (c == '{') {
        if (!push(p, false)) return false;
        p->state = ST_OBJ_FIRST;
    } else if (c == '[') {
        if (!push(p, true)) return false;
        p->state = ST_ARR_FIRST;
    } else if (c == '"') {
        p->in_key = false;
        p->out_len = 0;
        p->state = ST_STRING;
    } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
        p->prim_len = 0;
        p->prim[p->prim_len++] = c;
        p->state = ST_PRIM;
    } else {
        return false;
    }
    return true;
}

bool qj_feed(qj_parser_t *p, const char *data, size_t len) {
    if (p->error) return false;
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        switch (p->state) {
        case ST_VALUE:
            if (is_ws(c)) break;
            if (!begin_value(p, c)) goto fail;
            break;
        case ST_ARR_FIRST:
            if (is_ws(c)) break;
            if (c == ']') {
                if (!pop(p, true)) goto fail;
                break;
            }
            if (!begin_value(p, c)) goto fail;
            break;
        case ST_OBJ_FIRST:
        case ST_OBJ_KEY:
            if (is_ws(c)) break;
            if (c == '}' && p->state == ST_OBJ_FIRST) {
                if (!pop(p, false)) goto fail;
                break;
            }
            if (c != '"') goto fail;
            p->in_key = true;
            p->key_len = 0;
            p->key[p->depth - 1][0] = '\0';
            p->state = ST_STRING;
            break;
        case ST_COLON:
            if (is_ws(c)) break;
            if (c != ':') goto fail;
            p->state = ST_VALUE;
            break;
        case ST_AFTER_VALUE:
            if (is_ws(c)) break;
            if (c == ',') {
                p->state = p->is_array[p->depth - 1] ? ST_VALUE : ST_OBJ_KEY;
            } else if (c == '}') {
                if (!pop(p, false)) goto fail;
            } else if (c == ']') {
                if (!pop(p, true)) goto fail;
            } else {
                goto fail;
            }
            break;
        case ST_STRING:
            if (c == '"') {
                if (p->hi_surrogate) {  // 孤立高代理：按替换字符输出
                    put_codepoint(p, 0xFFFD);
                    p->hi_surrogate = 0;
                }
                if (p->in_key) {
                    p->state = ST_COLON;
                } else {
                    flush_out(p);
                    p->cb(p->user, p, QJ_EV_STR_END, NULL, 0);
                    value_finished(p);
                }
            } else if (c == '\\') {
                p->state = ST_ESCAPE;
            } else if ((unsigned char)c < 0x20) {
                goto fail;  // JSON 不允许未转义的控制字符
            } else {
                if (p->hi_surrogate) {
                    put_codepoint(p, 0xFFFD);
                    p->hi_surrogate = 0;
                }
                put_byte(p, c);
            }
            break;
        case ST_ESCAPE: {
            char d = 0;
            switch (c) {
            case '"': d = '"'; break;
            case '\\': d = '\\'; break;
            case '/': d = '/'; break;
            case 'b': d = '\b'; break;
            case 'f': d = '\f'; break;
            case 'n': d = '\n'; break;
            case 'r': d = '\r'; break;
            case 't': d = '\t'; break;
            case 'u':
                p->esc_hex_n = 0;
                p->esc_cp = 0;
                p->state = ST_UNICODE;
                continue;
            default: goto fail;
            }
            if (p->hi_surrogate) {
                put_codepoint(p, 0xFFFD);
                p->hi_surrogate = 0;
            }
            put_byte(p, d);
            p->state = ST_STRING;
            break;
        }
        case ST_UNICODE: {
            uint32_t v;
            if (c >= '0' && c <= '9') v = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (uint32_t)(c - 'A' + 10);
            else goto fail;
            p->esc_cp = (p->esc_cp << 4) | v;
            if (++p->esc_hex_n < 4) break;
            const uint32_t u = p->esc_cp;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (p->hi_surrogate) put_codepoint(p, 0xFFFD);
                p->hi_surrogate = u;  // 等待低代理
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                if (p->hi_surrogate) {
                    put_codepoint(p, 0x10000 + ((p->hi_surrogate - 0xD800) << 10) + (u - 0xDC00));
                    p->hi_surrogate = 0;
                } else {
                    put_codepoint(p, 0xFFFD);
                }
            } else {
                if (p->hi_surrogate) {
                    put_codepoint(p, 0xFFFD);
                    p->hi_surrogate = 0;
                }
                put_codepoint(p, u);
            }
            p->state = ST_STRING;
            break;
        }
        case ST_PRIM:
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' ||
                c == '+' || c == 'E') {
                if (p->prim_len + 1 < QJ_PRIM_MAX) p->prim[p->prim_len++] = c;
                break;
            }
            prim_end(p);
            // 结束原始值的那个字符本身还需按新状态处理一次
            if (is_ws(c)) break;
            if (p->state == ST_DONE) goto fail;
            if (c == ',') {
                p->state = p->is_array[p->depth - 1] ? ST_VALUE : ST_OBJ_KEY;
            } else if (c == '}') {
                if (!pop(p, false)) goto fail;
            } else if (c == ']') {
                if (!pop(p, true)) goto fail;
            } else {
                goto fail;
            }
            break;
        case ST_DONE:
            if (!is_ws(c)) goto fail;
            break;
        default:
            goto fail;
        }
    }
    // 分片末尾若正处于字符串值内部，把已解码部分先交出去，保证回调延迟有界。
    if (p->state == ST_STRING && !p->in_key) flush_out(p);
    return true;
fail:
    p->error = true;
    return false;
}
