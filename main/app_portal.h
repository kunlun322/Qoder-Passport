// main/app_portal.h —— WPA2 热点 + 本地网页配网（含 QCA PAT）。
// 每次进入通过重启隔离语音与配置任务；退出也重启，不让门户常驻语音会话。
#pragma once
#include "esp_err.h"

typedef enum {
    PORTAL_IDLE, PORTAL_READY, PORTAL_CONNECTING, PORTAL_CHECKING,
    PORTAL_WIFI_FAILED, PORTAL_PAT_FAILED, PORTAL_SERVICE_FAILED,
    PORTAL_SAVE_FAILED, PORTAL_DONE, PORTAL_ERROR
} portal_stage_t;

// app_wifi_init 后调用。创建 WPA2 热点（随机密码）、DNS 和 HTTP，地址 192.168.4.1。
esp_err_t app_portal_start(void);
portal_stage_t app_portal_stage(void);
const char *app_portal_ssid(void);
const char *app_portal_password(void);  // 仅屏幕/二维码使用，禁止记录日志
const char *app_portal_text(portal_stage_t stage);
