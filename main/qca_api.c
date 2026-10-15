// main/qca_api.c —— 见 qca_api.h。
#include "qca_api.h"

#include <stdio.h>
#include <string.h>

#include "app_fonts.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "qca_config.h"
#include "qca_pat.h"
#include "qjson.h"
#include "utf8_util.h"

static const char *TAG = "qca_api";

static char s_err[64] = "";
static int s_status;

const char *qca_api_last_error(void) { return s_err; }
int qca_api_last_status(void) { return s_status; }

static void set_err(const char *msg) {
    snprintf(s_err, sizeof(s_err), "%s", msg);
}

// 列表解析见 qca_list_parse.c（纯逻辑，有 host 测试）；这里只负责 HTTP 与名称净化。

// ---------------- HTTP ----------------
static esp_err_t http_json(esp_http_client_method_t method, const char *path, const char *body,
                           qj_cb_t cb, void *user, int *status_out) {
    s_status = 0;
    s_err[0] = 0;
    if (!qca_pat_present()) { set_err("请先配置 PAT"); return ESP_ERR_INVALID_STATE; }
    char url[192];
    snprintf(url, sizeof(url), QCA_API_BASE "%s", path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { set_err("内存不足"); return ESP_ERR_NO_MEM; }
    char auth[QCA_PAT_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", qca_pat());
    esp_err_t header_err = esp_http_client_set_header(cl, "Authorization", auth);
    memset(auth, 0, sizeof(auth));
    if (header_err != ESP_OK) {
        set_err("认证头设置失败");
        esp_http_client_cleanup(cl);
        return header_err;
    }
    esp_http_client_set_header(cl, "Accept", "application/json");
    char idem[40];
    const int body_len = body ? (int)strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(cl, "Content-Type", "application/json");
        snprintf(idem, sizeof(idem), "passport-%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random());
        esp_http_client_set_header(cl, "Idempotency-Key", idem);
    }

    esp_err_t err = esp_http_client_open(cl, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接 %s 失败: %s", path, esp_err_to_name(err));
        set_err("无法连接 QCA 服务器");
        esp_http_client_cleanup(cl);
        return err;
    }
    if (body_len > 0 && esp_http_client_write(cl, body, body_len) != body_len) {
        set_err("请求发送失败");
        esp_http_client_cleanup(cl);
        return ESP_FAIL;
    }
    (void)esp_http_client_fetch_headers(cl);
    const int status = esp_http_client_get_status_code(cl);
    s_status = status;
    if (status_out) *status_out = status;

    static qj_parser_t s_parser;   // 仅应用任务使用
    qj_init(&s_parser, cb, user);
    static char buf[1024];
    int total = 0;
    for (;;) {
        const int n = esp_http_client_read(cl, buf, sizeof(buf));
        if (n < 0) { err = ESP_FAIL; set_err("读取响应失败"); break; }
        if (n == 0) break;
        total += n;
        if (cb && !qj_feed(&s_parser, buf, (size_t)n)) {
            ESP_LOGW(TAG, "%s 响应 JSON 解析失败（已读 %d 字节）", path, total);
            err = ESP_FAIL;
            set_err("响应格式无法解析");
            break;
        }
    }
    ESP_LOGI(TAG, "%s → HTTP %d，%d 字节", path, status, total);
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return err;
}

esp_err_t qca_check_auth(void) {
    int status = 0;
    esp_err_t e = http_json(HTTP_METHOD_GET, "/forward/templates?limit=1&status=active",
                            NULL, NULL, NULL, &status);
    if (e != ESP_OK) return e;
    if (status == 200) return ESP_OK;
    set_err(status == 401 || status == 403 ? "PAT 无效或无权限" : "QCA 服务暂不可用");
    return ESP_FAIL;
}

static esp_err_t fetch_list(qca_list_kind_t kind, const char *path, qca_list_t *out) {
    static qca_list_ctx_t ctx;
    qca_list_parse_init(&ctx, kind, out);
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_GET, path, NULL, qca_list_parse_cb, &ctx, &status);
    if (err != ESP_OK) return err;
    if (status == 401 || status == 403) { set_err("PAT 无效或无权限"); return ESP_FAIL; }
    if (status != 200) {
        char m[64];
        snprintf(m, sizeof(m), "服务返回 %d", status);
        set_err(m);
        if (ctx.msg[0]) ESP_LOGW(TAG, "服务端说明: %s", ctx.msg);
        return ESP_FAIL;
    }
    for (int i = 0; i < out->count; i++) {
        app_text_sanitize(out->items[i].name, sizeof(out->items[i].name) - 1);   // 字库外字符 → '?'
    }
    if (out->truncated) ESP_LOGW(TAG, "%s 超过 %d 条，仅显示前 %d 条", path, QCA_LIST_MAX, out->count);
    return ESP_OK;
}

esp_err_t qca_list_templates(qca_list_t *out) {
    return fetch_list(QCA_LIST_TEMPLATES, "/forward/templates?limit=100&status=active", out);
}

esp_err_t qca_list_identities(qca_list_t *out) {
    return fetch_list(QCA_LIST_IDENTITIES, "/forward/identities?limit=100", out);
}

// ---------------- 创建会话 ----------------
typedef struct {
    char id[64];
    char msg[48];
    char *dst;
    size_t cap, len;
} conv_ctx_t;

static void conv_cb(void *user, qj_parser_t *p, qj_event_t ev, const char *data, size_t len) {
    conv_ctx_t *c = user;
    const int d = qj_depth(p);
    if (ev != QJ_EV_STR_CHUNK && ev != QJ_EV_STR_END) return;
    if (ev == QJ_EV_STR_END) { c->dst = NULL; return; }
    if (!c->dst) {
        const char *k = qj_key(p, d);
        if (d == 1 && strcmp(k, "id") == 0) { c->dst = c->id; c->cap = sizeof(c->id); }
        else if (strcmp(k, "message") == 0) { c->dst = c->msg; c->cap = sizeof(c->msg); }
        else return;
        c->len = 0;
        c->dst[0] = '\0';
    }
    size_t room = c->cap - 1 - c->len;
    if (len > room) len = room;
    memcpy(c->dst + c->len, data, len);
    c->len += len;
    c->dst[c->len] = '\0';
}

esp_err_t qca_create_conversation(const char *identity_id, const char *template_id,
                                  char *conv_id, int conv_id_cap) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"identity_id\":\"%s\",\"template_id\":\"%s\",\"title\":\"" QCA_CONV_TITLE "\"}",
             identity_id, template_id);
    static conv_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, "/forward/realtime/conversations", body,
                              conv_cb, &ctx, &status);
    if (err != ESP_OK) return err;
    if (status != 200 && status != 201) {
        char m[64];
        snprintf(m, sizeof(m), "创建会话失败（%d）", status);
        set_err(m);
        if (ctx.msg[0]) ESP_LOGW(TAG, "服务端说明: %s", ctx.msg);
        return ESP_FAIL;
    }
    if (strncmp(ctx.id, "conv_", 5) != 0) {
        set_err("会话 ID 缺失");
        return ESP_FAIL;
    }
    snprintf(conv_id, (size_t)conv_id_cap, "%s", ctx.id);
    ESP_LOGI(TAG, "已创建会话 %s", conv_id);
    return ESP_OK;
}
