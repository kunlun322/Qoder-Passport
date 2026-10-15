// main/qjson.h —— 流式（增量）JSON 词法/语法扫描器（纯逻辑，可 host 测试）。
//
// 为什么需要它：ESP32-C3 没有 PSRAM，而 QCA 的两类数据都“大”：
//   * GET /forward/templates 的每个 Template 带完整 system 提示词，单条 0.7~3.2 KB，
//     一页 100 条可达数十 KB；
//   * Realtime 下行 audio.delta 单条 WebSocket 消息约 25.7 KB（base64 PCM）。
// 把整条报文拼进连续内存再交给 cJSON 解析，需要 2~3 倍报文大小的连续堆，在 C3 上
// 与 Wi-Fi/TLS 同时运行时不可行。本扫描器按任意大小的分片喂入，O(1) 内存
// （固定 ~400 字节状态），把“键 / 字符串值分片 / 原始值 / 容器开闭”逐一回调给使用方，
// 使用方只截取自己关心的字段（id、name、type、audio…），其余内容直接丢弃。
//
// 约束与语义：
//   * 只接受 UTF-8 文本；字符串中的转义（\" \\ \/ \b \f \n \r \t \uXXXX，含代理对）
//     会被解码成 UTF-8 后再回调，因此使用方拿到的已是“显示用”字节。
//   * 嵌套深度上限 QJ_MAX_DEPTH；超出即报错（QCA 报文实际深度 ≤ 6）。
//   * 键名最长 QJ_KEY_MAX-1 字节，超长部分截断（只影响匹配，不影响后续解析）。
//   * 原始值（数字/true/false/null）最长 QJ_PRIM_MAX-1 字节，超长截断。
//   * 线程模型：一个 qj_parser_t 只能由一个任务使用；不分配堆内存。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QJ_MAX_DEPTH 8   // 最大容器嵌套层数（含根对象）
#define QJ_KEY_MAX   24  // 每层“当前键名”缓冲（含 NUL）
#define QJ_PRIM_MAX  24  // 原始值缓冲（含 NUL）
#define QJ_CHUNK_MAX 96  // 字符串值分片的解码输出缓冲；满了就先回调一次

// 回调事件类型。
typedef enum {
    QJ_EV_OBJ_BEGIN = 0,  // 进入对象 '{'；回调时 depth 已 +1
    QJ_EV_OBJ_END,        // 离开对象 '}'；回调时 depth 仍为该对象内部深度
    QJ_EV_ARR_BEGIN,      // 进入数组 '['
    QJ_EV_ARR_END,        // 离开数组 ']'
    QJ_EV_STR_CHUNK,      // 字符串值的一段已解码字节（data/len）；一个值可能分多段
    QJ_EV_STR_END,        // 字符串值结束（data=NULL,len=0）
    QJ_EV_PRIM,           // 原始值（数字/true/false/null）完整文本（data/len）
} qj_event_t;

struct qj_parser;

// 回调签名。p 可用 qj_depth()/qj_key()/qj_in_array() 查询当前位置；
// data 只在回调期间有效。回调内不得再调用 qj_feed() 重入同一解析器。
typedef void (*qj_cb_t)(void *user, struct qj_parser *p, qj_event_t ev,
                        const char *data, size_t len);

// 解析器内部状态（调用方只需分配，不要直接读写字段）。
typedef struct qj_parser {
    qj_cb_t cb;                         // 事件回调
    void *user;                         // 回调透传指针
    uint8_t state;                      // 词法状态（见 qjson.c 的 enum）
    uint8_t depth;                      // 当前容器深度：0=在根值之外
    bool is_array[QJ_MAX_DEPTH];        // 每层容器类型：true=数组 false=对象
    char key[QJ_MAX_DEPTH][QJ_KEY_MAX]; // 每层对象“当前成员”的键名（数组层为空串）
    uint8_t key_len;                    // 正在读取的键名长度
    char prim[QJ_PRIM_MAX];             // 正在读取的原始值
    uint8_t prim_len;
    char out[QJ_CHUNK_MAX];             // 字符串值解码输出缓冲
    uint8_t out_len;
    uint8_t esc_hex_n;                  // \uXXXX 已读到的十六进制位数
    uint32_t esc_cp;                    // \uXXXX 累积的码元
    uint32_t hi_surrogate;              // 待配对的高代理（0=无）
    bool in_key;                        // 当前字符串是键名（true）还是值（false）
    bool error;                         // 一旦置位，后续 qj_feed 全部返回 false
    bool done;                          // 根值已完整结束
} qj_parser_t;

// 初始化 / 复位解析器，可重复调用以开始新文档。
void qj_init(qj_parser_t *p, qj_cb_t cb, void *user);

// 喂入任意长度的分片。返回 false 表示语法错误或超出深度（此后需 qj_init 复位）。
// 不阻塞、不分配内存；所有回调在本函数内同步发生。
bool qj_feed(qj_parser_t *p, const char *data, size_t len);

// 根值是否已完整结束（用于判断一条消息是否收全）。
bool qj_done(const qj_parser_t *p);

// 当前容器深度（根对象内部为 1）。
static inline int qj_depth(const qj_parser_t *p) { return p->depth; }

// 第 level 层（1..depth）对象的当前键名；数组层或越界返回空串。
const char *qj_key(const qj_parser_t *p, int level);

// 第 level 层（1..depth）是否为数组。
bool qj_in_array(const qj_parser_t *p, int level);

// 便捷判断：当前所在层（depth）的当前键是否等于 name。
bool qj_key_is(const qj_parser_t *p, const char *name);
