// main/qca_list_parse.h —— QCA Forward 列表报文（Template / Identity）的流式解析（纯逻辑，可 host 测试）。
//
// 报文形如 {"data":[{"id":"...","name":"...",...}, ...], "has_more":false}。
// 每个 Template 带完整 system 提示词和嵌套的 model/tools/vaults 对象，单页可达数十 KB，
// 因此只从 qjson 事件里截取条目第一层的 id / name / external_id / status / enabled。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "qjson.h"

#define QCA_LIST_MAX   40   // 每类最多保留条数（实测 Template 17 条、Identity 20 条）
#define QCA_ID_MAX     48
#define QCA_NAME_MAX   92   // UTF-8 字节；约 30 个汉字，显示时再按宽度省略

typedef struct {
    char id[QCA_ID_MAX];
    char name[QCA_NAME_MAX];
} qca_item_t;

typedef struct {
    qca_item_t items[QCA_LIST_MAX];
    int count;
    bool truncated;            // 服务端条目多于 QCA_LIST_MAX 或 has_more=true
} qca_list_t;

typedef enum { QCA_LIST_TEMPLATES, QCA_LIST_IDENTITIES } qca_list_kind_t;

// 解析上下文（调用方只需分配，不要直接读写字段）。
typedef struct {
    qca_list_kind_t kind;
    qca_list_t *out;
    bool in_item;
    char id[QCA_ID_MAX];
    char name[QCA_NAME_MAX];
    char ext[QCA_NAME_MAX];
    char status[16];
    bool disabled;
    char *dst;                 // 当前字符串值的写入目标
    size_t dst_cap, dst_len;
    char msg[48];              // 错误报文里的 message
} qca_list_ctx_t;

// 清空 out 并准备解析。
void qca_list_parse_init(qca_list_ctx_t *c, qca_list_kind_t kind, qca_list_t *out);

// 作为 qj_init 的回调使用，user 传 qca_list_ctx_t*。
void qca_list_parse_cb(void *user, qj_parser_t *p, qj_event_t ev, const char *data, size_t len);
