// main/app_wifi.c —— 见 app_wifi.h。
#include "app_wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "qca_pat.h"
#include "qca_voice.h"

static const char *TAG = "app_wifi";

#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1
#define CONNECT_RETRY_MAX 5
// 家用 AP 在 APSTA 信道切换后常以 status=30（Refused Temporarily）拒绝首次关联，
// 并附 Comeback Time（参考实现 78/esp-wifi-connect 实测约 1.1 s）；而驱动的
// failure_retry_cnt 是背靠背重试、不遵守该定时器，过早重试必然再被拒。退避下限
// 取 1200 ms 越过这个窗口，上限 3000 ms 保证 5 次重试的总等待（≤12.6 s）仍落在
// app_wifi_connect 的超时预算之内。
#define RETRY_BACKOFF_MIN_MS 1200
#define RETRY_BACKOFF_MAX_MS 3000

#define NVS_NS        "qca_app"
#define NVS_KEY_PROV  "prov"
#define NVS_KEY_RESET "reset"
// 应用命名空间里的 Wi-Fi 凭据镜像。IDF 驱动的 WIFI_STORAGE_FLASH 持久化在本设备
// 实测中重启后会丢失 STA 配置（保存返回 ESP_OK，下次开机 ssid_len=0），而同一
// qca_app 命名空间的 PAT 却能可靠保留，故用镜像兜底。键名 ≤15 字符（NVS 限制）。
#define NVS_KEY_SSID  "wifi_ssid"
#define NVS_KEY_PASS  "wifi_pass"
// 上次连通时 AP 的主信道（2.4G 1~13）。按 IDF 定义它只是「扫描起点」提示
// （Channel hint ... scan starting from the specified channel），提示过期不会
// 导致连不上，因此不需要额外的全频段回退分支。0 = 未知。
#define NVS_KEY_CH    "wifi_ch"

static EventGroupHandle_t s_ev;
static bool s_auto_reconnect;
static volatile bool s_want;       // 正在请求连接 / 已连接时需要保持
static volatile bool s_connected;
static int s_retry;
static bool s_reset_in_progress; // 最后撤销意图失败时，页内重试仍不可退出恢复流程
static uint8_t s_ch_hint;          // 镜像里记住的信道提示，0 = 未知
static esp_timer_handle_t s_retry_timer;

static esp_err_t nvs_ready(void);
static void remember_channel(uint8_t ch);

// 退避重试交给 esp_timer 任务，而不是在事件回调里 vTaskDelay —— 后者会阻塞默认
// 事件循环最长 1.5 s，拖延 softAP/DHCP 等其它事件的分发。
static void retry_timer_cb(void *arg) {
    (void)arg;
    if (s_want && !s_connected) esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        const bool was_connected = s_connected;
        s_connected = false;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        if (!s_want) return;
        if (!s_auto_reconnect) {
            xEventGroupSetBits(s_ev, BIT_FAILED);
            return;
        }
        if (was_connected) {
            ESP_LOGW(TAG, "Wi-Fi 掉线(reason=%d)，重连", d ? d->reason : -1);
            s_retry = 0;
            esp_wifi_connect();
        } else if (++s_retry <= CONNECT_RETRY_MAX) {
            unsigned ms = RETRY_BACKOFF_MIN_MS * (unsigned)s_retry;
            if (ms > RETRY_BACKOFF_MAX_MS) ms = RETRY_BACKOFF_MAX_MS;
            ESP_LOGW(TAG, "连接失败(reason=%d)，%u ms 后第 %d/%d 次重试",
                     d ? d->reason : -1, ms, s_retry, CONNECT_RETRY_MAX);
            esp_timer_start_once(s_retry_timer, ms * 1000ULL);
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // 事件本身就带 AP 主信道，不必再调 esp_wifi_sta_get_ap_info()。
        const wifi_event_sta_connected_t *c = data;
        if (c) remember_channel(c->channel);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "已联网 IP=" IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        // 实时语音需要低延迟：关闭 Wi-Fi 省电，避免 DTIM 睡眠带来的数百毫秒抖动。
        esp_wifi_set_ps(WIFI_PS_NONE);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static esp_err_t nvs_ready(void) {
    static bool ok;
    if (ok) return ESP_OK;
    esp_err_t e = nvs_flash_init();
    // 初始化失败不是整区擦除授权；保留其他应用/BSP 数据，显式报错。
    if (e != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败，未擦除数据: %s", esp_err_to_name(e));
    ok = (e == ESP_OK);
    return e;
}

// 记住 AP 主信道，作为下次连接时的扫描起点提示；只在信道变化时落盘，正常运行
// 几乎不会触发写入。即使写失败也更新内存里的提示值，避免同一次会话反复重试。
static void remember_channel(uint8_t ch) {
    if (ch == 0 || ch == s_ch_hint) return;
    s_ch_hint = ch;
    if (nvs_ready() != ESP_OK) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, NVS_KEY_CH, ch);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "信道提示保存失败: %s", esp_err_to_name(e));
}

esp_err_t app_wifi_init(bool auto_reconnect) {
    s_auto_reconnect = auto_reconnect;
    esp_err_t e = nvs_ready();
    if (e != ESP_OK) return e;

    s_ev = xEventGroupCreate();
    if (!s_ev) return ESP_ERR_NO_MEM;

    // 单次定时器承载退避重试，回调只调非阻塞的 esp_wifi_connect()。
    const esp_timer_create_args_t ta = {
        .callback = retry_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    e = esp_timer_create(&ta, &s_retry_timer);
    if (e != ESP_OK) return e;

    e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) return e;
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return e;
    return esp_wifi_start();
}

void app_wifi_set_auto_reconnect(bool enabled) {
    s_auto_reconnect = enabled;
    if (!enabled) s_want = false;
}

bool app_wifi_has_config(void) {
    wifi_config_t c = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) {
        memset(&c, 0, sizeof(c));
        return false;
    }
    const bool present = c.sta.ssid[0] != 0;
    memset(&c, 0, sizeof(c));   // 栈上的密码副本立即清零
    return present;
}

void app_wifi_get_ssid(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    wifi_config_t c = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return;
    size_t n = strnlen((const char *)c.sta.ssid, sizeof(c.sta.ssid));
    if (n >= cap) n = cap - 1;
    memcpy(out, c.sta.ssid, n);
    out[n] = 0;
    memset(&c, 0, sizeof(c));   // 栈上的密码副本立即清零
}

esp_err_t app_wifi_connect(int timeout_ms) {
    if (s_connected) return ESP_OK;
    if (!app_wifi_has_config()) return ESP_ERR_NOT_FOUND;
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    s_want = true;
    s_retry = 0;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) return e;
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (b & BIT_GOT_IP) return ESP_OK;
    s_want = false;
    // 调用者已放弃本轮连接：取消尚未到点的退避重试，避免它在 s_want=false 之后再
    // 拉起一次无主的 esp_wifi_connect()（回调里也有同样的判据，这里只是不留悬挂）。
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    return (b & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

bool app_wifi_is_connected(void) {
    return s_connected;
}

// ---------------- 应用 NVS 的 Wi-Fi 凭据镜像 ----------------
// 见 NVS_KEY_SSID 注释：IDF 驱动持久化不可靠时用 qca_app 兜底。密码以明文存于
// NVS，与既有 PAT 及 IDF wifi NVS 的存储强度一致（本机未启用 Flash/NVS 加密）；
// 所有日志只记录长度与返回码，绝不记录 SSID/密码内容。栈上副本用完立即清零。
esp_err_t app_wifi_save_creds(const char *ssid, const char *pass) {
    if (!ssid) return ESP_ERR_INVALID_ARG;
    char s[33] = {0}, p[65] = {0};
    size_t sn = strnlen(ssid, sizeof(s) - 1);
    if (sn == 0) return ESP_ERR_INVALID_ARG;
    memcpy(s, ssid, sn);
    if (pass) memcpy(p, pass, strnlen(pass, sizeof(p) - 1));
    esp_err_t e = nvs_ready();
    nvs_handle_t h;
    if (e == ESP_OK) e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, NVS_KEY_SSID, s);
        if (e == ESP_OK) e = nvs_set_str(h, NVS_KEY_PASS, p);
        // 同一次提交里带上刚学到的信道提示（0 = 未知），省一次 NVS 事务。
        if (e == ESP_OK) e = nvs_set_u8(h, NVS_KEY_CH, s_ch_hint);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    memset(s, 0, sizeof(s));
    memset(p, 0, sizeof(p));
    if (e != ESP_OK) ESP_LOGE(TAG, "Wi-Fi 凭据镜像保存失败: %s", esp_err_to_name(e));
    return e;
}

// 从镜像读取凭据；ssid 必填，pass 可空（开放网络），ch 可选（缺失即 0）。
// 仅供本文件回填使用。
static esp_err_t load_creds(char *ssid, size_t scap, char *pass, size_t pcap, uint8_t *ch) {
    if (!ssid || scap == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = 0;
    if (pass && pcap) pass[0] = 0;
    if (ch) *ch = 0;
    esp_err_t e = nvs_ready();
    if (e != ESP_OK) return e;
    nvs_handle_t h;
    e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t n = scap;
    e = nvs_get_str(h, NVS_KEY_SSID, ssid, &n);
    if (e == ESP_OK && pass && pcap) {
        size_t m = pcap;
        if (nvs_get_str(h, NVS_KEY_PASS, pass, &m) != ESP_OK) pass[0] = 0;
    }
    // 旧镜像没有这个键，读取失败保持 0（无提示），不算错误。
    if (e == ESP_OK && ch) (void)nvs_get_u8(h, NVS_KEY_CH, ch);
    nvs_close(h);
    return e;
}

bool app_wifi_restore_creds(void) {
    if (app_wifi_has_config()) return false;   // 驱动已有配置，无需回填
    char ssid[33] = {0}, pass[65] = {0};
    uint8_t ch = 0;
    esp_err_t e = load_creds(ssid, sizeof(ssid), pass, sizeof(pass), &ch);
    if (e != ESP_OK || !ssid[0]) {
        memset(pass, 0, sizeof(pass));
        return false;
    }
    s_ch_hint = ch;
    wifi_config_t c = {0};
    size_t sn = strnlen(ssid, sizeof(c.sta.ssid));
    memcpy(c.sta.ssid, ssid, sn);
    size_t pn = strnlen(pass, sizeof(c.sta.password));
    memcpy(c.sta.password, pass, pn);
    c.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 连接策略（对照 78/esp-wifi-connect 3.3.1 的 StartConnect）：按信道是否已知二选一。
    // 已知 → 从该信道起快扫，命中同名 AP 即连，省掉全频段遍历（这是开机联网提速的
    //   来源）；提示万一过期，快扫仍会从该信道继续扫完其余信道直到命中，只是不做
    //   信号排序，连通后 remember_channel 会把提示刷新。
    // 未知 → 扫完全部信道再按信号强度挑同名 AP 中最好的一个（多 AP/mesh 家庭网络
    //   很常见），并允许驱动在换下一个 AP 前原地重试一次。IDF 文档明确要求
    //   failure_retry_cnt 必须配 WIFI_ALL_CHANNEL_SCAN，故只在未知分支设置。
    // listen_interval 不设：它只在 WIFI_PS_MAX_MODEM 下生效，而本机会话期用 PS_NONE。
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (s_ch_hint) {
        c.sta.channel = s_ch_hint;
        c.sta.scan_method = WIFI_FAST_SCAN;
    } else {
        c.sta.channel = 0;
        c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        c.sta.failure_retry_cnt = 1;
    }
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);   // 尽力让 IDF 也持久化；镜像才是真相源
    e = esp_wifi_set_config(WIFI_IF_STA, &c);
    // 只记录长度、信道与返回码，绝不记录 SSID/密码内容。
    ESP_LOGI(TAG, "从应用 NVS 回填 Wi-Fi 凭据 ssid_len=%u pass_len=%u ch=%u set=%s",
             (unsigned)sn, (unsigned)pn, (unsigned)s_ch_hint, esp_err_to_name(e));
    memset(&c, 0, sizeof(c));
    memset(pass, 0, sizeof(pass));
    return e == ESP_OK;
}

esp_err_t app_wifi_clear_creds(void) {
    esp_err_t e = nvs_ready();
    if (e != ESP_OK) return e;
    nvs_handle_t h;
    e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t e1 = nvs_erase_key(h, NVS_KEY_SSID);
    if (e1 == ESP_ERR_NVS_NOT_FOUND) e1 = ESP_OK;
    esp_err_t e2 = nvs_erase_key(h, NVS_KEY_PASS);
    if (e2 == ESP_ERR_NVS_NOT_FOUND) e2 = ESP_OK;
    esp_err_t e3 = nvs_erase_key(h, NVS_KEY_CH);
    if (e3 == ESP_ERR_NVS_NOT_FOUND) e3 = ESP_OK;
    e = (e1 != ESP_OK) ? e1 : ((e2 != ESP_OK) ? e2 : e3);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    s_ch_hint = 0;   // 恢复出厂后不留旧信道提示
    return e;
}

esp_err_t app_wifi_request_provision(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, NVS_KEY_PROV, 1);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e; // 不能在未落盘时重启，否则重新配置入口会失效
    ESP_LOGI(TAG, "下次开机进入热点配置，重启");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

bool app_wifi_take_provision_flag(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_ready() != ESP_OK) return false;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, NVS_KEY_PROV, &v) == ESP_OK && v) {
        nvs_erase_key(h, NVS_KEY_PROV);
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

esp_err_t app_wifi_request_factory_reset(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, NVS_KEY_RESET, 1);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    // 重启隔离正在运行的门户/HTTP/语音任务。这里只存意图，不并发删除凭据。
    ESP_LOGI(TAG, "已确认恢复出厂，重启后清除应用设置");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t app_wifi_factory_reset_pending(bool *pending) {
    if (!pending) return ESP_ERR_INVALID_ARG;
    *pending = s_reset_in_progress;
    esp_err_t e = nvs_ready();
    if (e != ESP_OK) return e;
    nvs_handle_t h;
    e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    uint8_t v = 0;
    e = nvs_get_u8(h, NVS_KEY_RESET, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e == ESP_OK) *pending = s_reset_in_progress || v != 0;
    return e;
}

esp_err_t app_wifi_finish_factory_reset(void) {
    bool pending = false;
    esp_err_t e = app_wifi_factory_reset_pending(&pending);
    if (e != ESP_OK) return e;
    if (!pending) return ESP_ERR_INVALID_STATE;
    s_reset_in_progress = true;
    app_wifi_set_auto_reconnect(false);
    // 必须在本轮开机启动任何门户/云端 worker 前执行，不依赖异步断开延时。
    e = esp_wifi_stop();
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (e == ESP_OK) e = esp_wifi_restore(); // IDF 官方 API：只重置 Wi-Fi 持久设置
    if (e == ESP_OK) e = app_wifi_clear_creds(); // 同时清除应用 NVS 的 Wi-Fi 凭据镜像
    if (e == ESP_OK) e = qca_pat_use("", true);
    if (e == ESP_OK) e = qca_voice_reset_volume();
    if (e != ESP_OK) return e; // 意图尚在，下次开机/本页重试继续，不能回普通流程
    nvs_handle_t h;
    e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_key(h, NVS_KEY_PROV);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_erase_key(h, NVS_KEY_RESET);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK; // 最后 commit 失败后的页内重试
    if (e == ESP_OK) e = nvs_commit(h); // Wi-Fi、PAT、音量全部成功后，最后撤销意图
    if (e != ESP_OK) {
        // 尽力重新落盘；即使重写也失败，RAM 闭锁仍保留，不在本次开机恢复正常联网。
        if (nvs_set_u8(h, NVS_KEY_RESET, 1) == ESP_OK) (void)nvs_commit(h);
    }
    nvs_close(h);
    if (e == ESP_OK) s_reset_in_progress = false;
    return e;
}
