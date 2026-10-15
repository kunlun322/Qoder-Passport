// main/qca_pat.h —— Qoder Cloud Agents 个人访问令牌（PAT）的设备端存储。
//
// PAT 由热点配置页（app_portal）录入，保存在 NVS（命名空间 "qca_app"，键 "pat"），
// 固件镜像里不再包含任何凭证。运行时 qca_api / qca_voice 通过 qca_pat() 取当前值
// 组装 Authorization 头。本模块不打印、不记录 PAT 内容。
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define QCA_PAT_MAX 256   // 含结尾 '\0'；平台未公布长度上限，按 255 字符预留

// 从 NVS 读入内存缓存（需先初始化 NVS，app_wifi_init 已完成）。
// 返回 ESP_OK / ESP_ERR_NOT_FOUND（尚未配置）/ 其他 NVS 错误。
esp_err_t qca_pat_load(void);

// 内存中是否有可用的 PAT。
bool qca_pat_present(void);

// 当前 PAT（未配置时为空串）。仅应用任务 / 语音任务读取，返回的指针在下一次
// qca_pat_use() 前有效。
const char *qca_pat(void);

// 使用新的 PAT；persist=true 时先保存 NVS，成功后更新缓存。
// 空串用于失败回滚到“未配置”，persist=true 时删除保存的令牌。
esp_err_t qca_pat_use(const char *pat, bool persist);
