// main/b64_stream.c —— 流式 Base64 实现。说明见 b64_stream.h。
#include "b64_stream.h"

static const char ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_decoder_init(b64_decoder_t *d) {
    d->acc = 0;
    d->n = 0;
    d->pad = 0;
    d->error = false;
}

// 单字符查表：返回 0..63；'=' 返回 64；空白返回 65；非法返回 -1。
static int dec_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;  // 兼容 URL-safe 变体
    if (c == '/' || c == '_') return 63;
    if (c == '=') return 64;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return 65;
    return -1;
}

size_t b64_decode_chunk(b64_decoder_t *d, const char *in, size_t len, uint8_t *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        const int v = dec_char(in[i]);
        if (v == 65) continue;
        if (v < 0) {
            d->error = true;
            continue;
        }
        if (v == 64) {
            // 填充：xx== 输出 1 字节，xxx= 输出 2 字节
            d->pad++;
            if (d->n == 2 && d->pad == 1) {
                if (w < cap) out[w++] = (uint8_t)(d->acc >> 4); else d->error = true;
            } else if (d->n == 3 && d->pad == 1) {
                if (w + 1 < cap + 0 && w + 2 <= cap) {
                    out[w++] = (uint8_t)(d->acc >> 10);
                    out[w++] = (uint8_t)(d->acc >> 2);
                } else {
                    d->error = true;
                }
            }
            d->n = 0;
            d->acc = 0;
            continue;
        }
        if (d->pad) {  // 填充之后又出现数据：视为新一段（容错，不报错）
            d->pad = 0;
        }
        d->acc = (d->acc << 6) | (uint32_t)v;
        if (++d->n == 4) {
            if (w + 3 <= cap) {
                out[w++] = (uint8_t)(d->acc >> 16);
                out[w++] = (uint8_t)(d->acc >> 8);
                out[w++] = (uint8_t)d->acc;
            } else {
                d->error = true;
            }
            d->n = 0;
            d->acc = 0;
        }
    }
    return w;
}

size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    const size_t need = b64_encoded_len(len);
    if (cap < need) return 0;
    size_t w = 0, i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[w++] = ENC[(v >> 18) & 63];
        out[w++] = ENC[(v >> 12) & 63];
        out[w++] = ENC[(v >> 6) & 63];
        out[w++] = ENC[v & 63];
    }
    if (len - i == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[w++] = ENC[(v >> 18) & 63];
        out[w++] = ENC[(v >> 12) & 63];
        out[w++] = '=';
        out[w++] = '=';
    } else if (len - i == 2) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[w++] = ENC[(v >> 18) & 63];
        out[w++] = ENC[(v >> 12) & 63];
        out[w++] = ENC[(v >> 6) & 63];
        out[w++] = '=';
    }
    return w;
}
