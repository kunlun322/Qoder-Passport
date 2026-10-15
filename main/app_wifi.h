// main/app_wifi.h —— Wi-Fi STA 连接与“下次开机进入配网”标记。
//
// Wi-Fi 凭据由 esp_wifi 存在 NVS（WIFI_STORAGE_FLASH），并在应用命名空间 qca_app
// 保留一份镜像作为兜底（IDF 持久化在本设备实测会丢失）；镜像里还记着上次连通时 AP
// 的主信道，下次连接作为扫描起点提示以加快联网。本模块绝不打印 SSID/密码内容，
// 日志只出现长度、信道与返回码。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// 初始化 NVS / netif / 事件循环 / Wi-Fi STA 并启动驱动。
// auto_reconnect=true：连上后掉线自动重连（正常模式）；false：热点配置模式单次尝试。
esp_err_t app_wifi_init(bool auto_reconnect);
void app_wifi_set_auto_reconnect(bool enabled);

// NVS 里是否已有 STA SSID。
bool app_wifi_has_config(void);

// 复制已保存的 SSID（不含密码）。
void app_wifi_get_ssid(char *out, size_t cap);

// 连接已保存的网络，阻塞直到拿到 IP 或超时。
esp_err_t app_wifi_connect(int timeout_ms);

bool app_wifi_is_connected(void);

// 应用 NVS（qca_app）里的 Wi-Fi 凭据镜像：IDF 驱动的 WIFI_STORAGE_FLASH 持久化在
// 本设备实测中重启后会丢失 STA 配置，用与 PAT 相同的可靠命名空间兜底。密码明文存
// 于 NVS（与既有 PAT 强度一致），任何日志只记录长度，绝不打印 SSID/密码内容。
// 配网成功后保存当前生效凭据（含“沿用旧密码”情形，故由调用方传入实际连接用配置），
// 同一次提交里带上已学到的信道提示。
esp_err_t app_wifi_save_creds(const char *ssid, const char *pass);
// 开机在启动门户/云端请求前调用：驱动无 STA 配置但镜像存在时回填驱动，
// 返回是否回填成功。回填时一并套用连接策略：信道已知就从该信道快扫（开机提速），
// 未知则全频段扫描后按信号挑最好的同名 AP 并允许驱动原地重试一次。
// 回填后 app_wifi_has_config() 即为真。
bool app_wifi_restore_creds(void);
// 清除镜像（含信道提示）；恢复出厂时调用。键不存在视为成功（幂等）。
esp_err_t app_wifi_clear_creds(void);

// 置“下次开机进入配网”标记并重启；失败返回错误，不重启。
esp_err_t app_wifi_request_provision(void);

// 读取并清除配网标记。
bool app_wifi_take_provision_flag(void);

// 二次确认后写入恢复出厂意图并重启；失败不重启、不删除任何配置。
esp_err_t app_wifi_request_factory_reset(void);
// 开机在启动配置服务/云端请求之前检查；读取失败必须停止正常流程。
esp_err_t app_wifi_factory_reset_pending(bool *pending);
// 仅在专用重置开机、语音/配置 worker 均未启动时调用。停 Wi-Fi 后删除
// Wi-Fi 持久配置、PAT、音量、配网标记，最后清除重置意图。失败保留意图可重试。
// 不擦除整个 NVS/Flash，不触碰 PHY 或其他命名空间。成功后调用方重启。
esp_err_t app_wifi_finish_factory_reset(void);
