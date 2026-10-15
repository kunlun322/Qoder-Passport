// main/b64_stream.h —— 流式 Base64 编解码（纯逻辑，可 host 测试）。
//
// 解码：Realtime 下行 audio.delta 的 base64 文本会被 qjson 按分片交出，分片边界
// 可能落在 4 字符组中间，因此需要跨分片保存 0~3 个待凑整的字符。
// 编码：上行 audio.append 每 100 ms 一包，一次性编码到调用方提供的缓冲。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t acc;   // 已累积的 6-bit 组
    uint8_t n;      // acc 中的字符数（0..3）
    uint8_t pad;    // 已遇到的 '=' 个数；>0 后不再接受数据字符
    bool error;     // 遇到非法字符
} b64_decoder_t;

void b64_decoder_init(b64_decoder_t *d);

// 解码一段 base64 文本，输出写入 out（容量 cap）。返回写入字节数。
// 空白字符被忽略；非法字符置 error 并跳过。out 容量至少为 (len/4+1)*3 才能保证不丢数据，
// 不足时多出的字节被丢弃并置 error。
size_t b64_decode_chunk(b64_decoder_t *d, const char *in, size_t len, uint8_t *out, size_t cap);

// 编码 len 字节到 out（含 '=' 填充，不写 NUL）。返回写入字符数；cap 不足返回 0。
size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);

// 编码 len 字节所需的字符数。
static inline size_t b64_encoded_len(size_t len) { return (len + 2) / 3 * 4; }
