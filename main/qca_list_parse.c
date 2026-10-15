// main/qca_list_parse.c —— 见 qca_list_parse.h。
#include "qca_list_parse.h"

#include <stdio.h>
#include <string.h>

// qjson 的深度把每层容器都计入：根对象内部=1，data 数组内部=2，单个条目对象内部=3
// （OBJ_BEGIN 回调时深度已 +1）。条目自身的字段就在第 3 层；model.id 等嵌套字段在
// 第 4 层及更深，自然被忽略。
#define LIST_ITEM_DEPTH 3

void qca_list_parse_init(qca_list_ctx_t *c, qca_list_kind_t kind, qca_list_t *out) {
    memset(c, 0, sizeof(*c));
    memset(out, 0, sizeof(*out));
    c->kind = kind;
    c->out = out;
}

static void begin_str(qca_list_ctx_t *c, char *dst, size_t cap) {
    c->dst = dst;
    c->dst_cap = cap;
    c->dst_len = 0;
    dst[0] = '\0';
}

static void append_str(qca_list_ctx_t *c, const char *data, size_t len) {
    if (!c->dst) return;
    const size_t room = c->dst_cap - 1 - c->dst_len;
    if (len > room) len = room;
    memcpy(c->dst + c->dst_len, data, len);
    c->dst_len += len;
    c->dst[c->dst_len] = '\0';
}

static void finish_item(qca_list_ctx_t *c) {
    qca_list_t *o = c->out;
    if (c->id[0] == '\0' || c->disabled) return;
    // status 非 active 的 Template 跳过（查询已带过滤，这里再兜底一次）。
    // 必须在条目结束时判断：值可能被网络分片切成多段回调。
    if (c->kind == QCA_LIST_TEMPLATES && c->status[0] && strcmp(c->status, "active") != 0) return;
    if (o->count >= QCA_LIST_MAX) { o->truncated = true; return; }
    qca_item_t *it = &o->items[o->count++];
    snprintf(it->id, sizeof(it->id), "%s", c->id);
    const char *nm = c->name[0] ? c->name : (c->ext[0] ? c->ext : c->id);
    snprintf(it->name, sizeof(it->name), "%s", nm);
}

void qca_list_parse_cb(void *user, qj_parser_t *p, qj_event_t ev, const char *data, size_t len) {
    qca_list_ctx_t *c = user;
    const int d = qj_depth(p);
    const bool item_level = d == LIST_ITEM_DEPTH && strcmp(qj_key(p, 1), "data") == 0 &&
                            qj_in_array(p, 2) && !qj_in_array(p, 3);

    if (ev == QJ_EV_OBJ_BEGIN && item_level) {
        c->in_item = true;
        c->id[0] = c->name[0] = c->ext[0] = c->status[0] = '\0';
        c->disabled = false;
        c->dst = NULL;
        return;
    }
    if (ev == QJ_EV_OBJ_END && item_level && c->in_item) {
        c->in_item = false;
        c->dst = NULL;
        finish_item(c);
        return;
    }
    if (ev != QJ_EV_STR_CHUNK && ev != QJ_EV_STR_END && ev != QJ_EV_PRIM) return;

    const char *k = qj_key(p, d);
    if (c->in_item && item_level) {
        if (ev == QJ_EV_PRIM) {
            if (strcmp(k, "enabled") == 0 && len == 5 && memcmp(data, "false", 5) == 0) {
                c->disabled = true;
            }
            return;
        }
        if (ev == QJ_EV_STR_END) { c->dst = NULL; return; }
        if (!c->dst) {
            if (strcmp(k, "id") == 0) begin_str(c, c->id, sizeof(c->id));
            else if (strcmp(k, "name") == 0) begin_str(c, c->name, sizeof(c->name));
            else if (strcmp(k, "status") == 0) begin_str(c, c->status, sizeof(c->status));
            else if (c->kind == QCA_LIST_IDENTITIES && strcmp(k, "external_id") == 0)
                begin_str(c, c->ext, sizeof(c->ext));
            else return;
        }
        append_str(c, data, len);
        return;
    }
    if (d == 1 && ev == QJ_EV_PRIM && strcmp(k, "has_more") == 0 &&
        len == 4 && memcmp(data, "true", 4) == 0) {
        c->out->truncated = true;
        return;
    }
    // 错误报文：{"error":{"message":...}} 或 {"message":...}
    if (!c->in_item && strcmp(k, "message") == 0) {
        if (ev == QJ_EV_STR_END) { c->dst = NULL; return; }
        if (ev == QJ_EV_PRIM) return;
        if (!c->dst) begin_str(c, c->msg, sizeof(c->msg));
        append_str(c, data, len);
    }
}
