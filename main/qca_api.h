// main/qca_api.h —— QCA Forward 层 REST 接口：列 Template / Identity、创建 Realtime 会话。
//
// 所有函数都是阻塞的（内部 HTTPS），只能在应用任务中调用，不要在 LVGL 任务里调用。
// 响应体用 qjson 流式解析，只保留 id/name 等少量字段，避免在无 PSRAM 的 C3 上
// 整包缓存数十 KB 的 Template 列表（每条都带完整 system 提示词）。
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "qca_list_parse.h"   // qca_list_t / qca_item_t / QCA_*_MAX（名称在此处再按字库净化）

// 最近一次失败的简短原因（中文，可直接显示）。
const char *qca_api_last_error(void);
int qca_api_last_status(void);

// 检查当前 PAT 是否能读取模板接口；不保留响应数据，仍执行正常 TLS 证书验证。
esp_err_t qca_check_auth(void);

// GET /forward/templates?status=active —— 平台 Forward 层全部启用的 Template。
esp_err_t qca_list_templates(qca_list_t *out);

// GET /forward/identities —— 用户身份（Forward Identity）列表，跳过 enabled=false 的条目。
esp_err_t qca_list_identities(qca_list_t *out);

// POST /forward/realtime/conversations —— 用 Identity + Template 创建实时语音会话。
esp_err_t qca_create_conversation(const char *identity_id, const char *template_id,
                                  char *conv_id, int conv_id_cap);
