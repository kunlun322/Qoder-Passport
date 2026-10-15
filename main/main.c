// main/main.c —— 「Qoder Passport」：选 Agent 模板 → 选用户身份 → 全双工实时语音对话。
//
// 按键（全局统一）：
//   ▲/▼ 单击      列表中移动选中项（双击 ±2，长按 ±5）；语音页调音量 ±10
//   OK  单击      列表中确认；语音页暂停/继续；出错页重试
//   OK  长按      Agent 模板页/热点页/Wi-Fi、PAT失败页=设备设置；
//                 身份页=返回模板；语音页=结束对话；设置内可刷新、配网或恢复出厂
//
// 线程：app_task 串行执行整个流程（阻塞 HTTPS 只在这里做）；按键回调只入队；
// 界面由 app_ui 内部持 LVGL 锁绘制；语音由 qca_voice 自己的任务驱动。
#include <stdio.h>
#include <string.h>
#include "app_fonts.h"
#include "app_list.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "app_portal.h"
#include "qca_pat.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "qca_api.h"
#include "qca_voice.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8
#define WIFI_TIMEOUT_MS   20000
#define LIST_PAGE_STEP    5

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static bool s_audio_ok;

static qca_list_t s_templates;
static qca_list_t s_idents;
static app_list_t s_tpl_cur;
static app_list_t s_ident_cur;
static char s_conv_id[64];

// ---------- 按键 ----------
// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold (500 ms here), so an unintended action fires almost immediately. Drop
// key actions until that press comes up; the deadline is only a safety net so a
// stuck key cannot leave the pad dead. The state machine lives in key_guard.h so it
// can be unit-tested without the rest of the app.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    // 只关心语义事件；PRESS/RELEASE 不入队，避免挤占队列。
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static bool wait_key(input_event_t *out, int timeout_ms) {
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_input_queue, out, t) == pdTRUE;
}

// 阻塞操作（联网、HTTPS）期间按下的键不应在操作结束后“补触发”。
static void flush_keys(void) {
    xQueueReset(s_input_queue);
}

static bool is_ok(const input_event_t *k) {
    return k->btn == BSP_BTN_OK && (k->event == BSP_BTN_CLICK || k->event == BSP_BTN_DOUBLE);
}

static bool is_ok_long(const input_event_t *k) {
    return k->btn == BSP_BTN_OK && k->event == BSP_BTN_LONG;
}

// ▲/▼ 的移动量：单击 1，双击 2，长按一页。OK 返回 0。
static int move_delta(const input_event_t *k) {
    int step = k->event == BSP_BTN_CLICK ? 1
             : k->event == BSP_BTN_DOUBLE ? 2
             : k->event == BSP_BTN_LONG ? LIST_PAGE_STEP : 0;
    if (k->btn == BSP_BTN_UP) return -step;
    if (k->btn == BSP_BTN_DOWN) return step;
    return 0;
}

// 等待 OK（返回 true）或长按 OK（返回 false）。
static bool wait_ok_or_long(void) {
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        if (is_ok(&k)) return true;
        if (is_ok_long(&k)) return false;
    }
}

// ---------- 配网 ----------
static bool settings_menu(bool provisioning);

static void run_provision(void) {
    app_ui_show_message(UI_TONE_INFO, "正在启动配置热点", "配置 Wi-Fi 与 PAT", NULL, true);
    if (app_portal_start() != ESP_OK) ESP_LOGE(TAG, "配置热点启动失败");
    app_ui_show_provision();
    flush_keys();
    input_event_t k;
    for (;;) {
        if (wait_key(&k, -1) && is_ok_long(&k)) {
            portal_stage_t st = app_portal_stage();
            if (st == PORTAL_CONNECTING || st == PORTAL_CHECKING || st == PORTAL_DONE) continue;
            (void)settings_menu(true);
            app_ui_show_provision(); // 返回已有门户，不重复创建 HTTP/DNS/worker
            flush_keys();
        }
    }
}

static void request_provision(void) {
    esp_err_t e = app_wifi_request_provision();
    ESP_LOGE(TAG, "配网标记未保存，原地进入热点: %s", esp_err_to_name(e));
    run_provision(); // 不在失败写入后盲目重启
}

// 恢复出厂只能通过设置页的第二次确认，不把进入设置/一次 OK 当作删除授权。
static void confirm_factory_reset(void) {
    bool confirm = false;
    app_ui_show_reset_confirm(confirm);
    flush_keys();
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        const int d = move_delta(&k);
        if (d) {
            confirm = d > 0;
            app_ui_show_reset_confirm(confirm);
        } else if (!confirm && (is_ok(&k) || is_ok_long(&k))) {
            return;
        } else if (confirm && is_ok_long(&k)) {
            esp_err_t e = app_wifi_request_factory_reset();
            // 成功会重启；失败不清除任何配置，也不重启。
            ESP_LOGE(TAG, "重置意图保存失败: %s", esp_err_to_name(e));
            app_ui_show_message(UI_TONE_ERROR, "未能启动恢复出厂", "设置未清除，请稍后重试",
                                "OK 返回设置", false);
            flush_keys();
            wait_ok_or_long();
            return;
        }
    }
}

// 返回 true 表示刷新模板列表。默认选中“返回”，长按只返回，不执行清除。
static bool settings_menu(bool provisioning) {
    int selected = 3;
    app_ui_show_settings(provisioning, selected);
    flush_keys();
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        const int d = move_delta(&k);
        if (d) {
            selected += d;
            if (selected < 0) selected = 0;
            if (selected > 3) selected = 3;
            app_ui_settings_update(selected);
        } else if (is_ok_long(&k)) {
            return false;
        } else if (is_ok(&k)) {
            switch (selected) {
            case 0:
                if (provisioning) esp_restart();
                return true;
            case 1:
                if (provisioning) return false; // 已在配置，返回显示二维码即可
                request_provision();
                break;
            case 2:
                confirm_factory_reset();
                selected = 3;
                app_ui_show_settings(provisioning, selected);
                flush_keys();
                break;
            default:
                return false;
            }
        }
    }
}

// 新开机专用恢复路径：此时门户、TLS 和语音任务均未启动。
// 断电/部分失败时保留重置意图，下一次开机继续；不允许带着半清除配置联网。
static void finish_pending_reset(void) {
    for (;;) {
        bool pending = false;
        esp_err_t e = app_wifi_factory_reset_pending(&pending);
        if (e == ESP_OK && !pending) return;
        if (e == ESP_OK) {
            app_ui_show_message(UI_TONE_WARN, "正在恢复出厂设置", "请勿断电，完成后重新配置",
                                NULL, true);
            e = app_wifi_finish_factory_reset();
            if (e == ESP_OK) {
                app_ui_show_message(UI_TONE_OK, "设置已清除", "即将重启并开启配置热点", NULL, false);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
        ESP_LOGE(TAG, "恢复出厂未完成: %s", esp_err_to_name(e));
        app_ui_show_message(UI_TONE_ERROR, "设置清除未完成", "不会进入正常联网，请重试",
                            "OK 重试  长按 OK 重启", false);
        flush_keys();
        if (!wait_ok_or_long()) esp_restart();
    }
}

// ---------- 主流程 ----------
typedef enum {
    ST_WIFI,
    ST_TPL_LOAD,
    ST_TPL_PICK,
    ST_IDENT_LOAD,
    ST_IDENT_PICK,
    ST_CONV,
    ST_VOICE,
} app_stage_t;

static app_stage_t stage_wifi(void) {
    char ssid[33];
    char body[128];
    app_wifi_get_ssid(ssid, sizeof(ssid));
    app_text_sanitize(ssid, sizeof(ssid));
    snprintf(body, sizeof(body), "网络：%s\n长按 OK 进入设备设置", ssid);
    app_ui_show_message(UI_TONE_INFO, "正在连接 Wi-Fi", body, NULL, true);
    esp_err_t e = app_wifi_connect(WIFI_TIMEOUT_MS);
    // 启动连接期间保留设置入口，其余输入仍丢弃；联网等待结束后才显示菜单。
    input_event_t k;
    bool config_requested = false;
    while (wait_key(&k, 0)) if (is_ok_long(&k)) config_requested = true;
    flush_keys();
    if (config_requested) (void)settings_menu(false);
    if (e == ESP_OK) return ST_TPL_LOAD;
    ESP_LOGW(TAG, "Wi-Fi 未联网，自动进入配置热点: %s", esp_err_to_name(e));
    run_provision();
    return ST_WIFI;
}

static app_stage_t stage_load(bool templates) {
    if (!app_wifi_is_connected()) return ST_WIFI;
    app_ui_show_message(UI_TONE_INFO, templates ? "正在读取 Agent模板" : "正在读取用户身份",
                        "从 Qoder Cloud Agents 获取列表", NULL, true);
    qca_list_t *list = templates ? &s_templates : &s_idents;
    esp_err_t e = templates ? qca_list_templates(list) : qca_list_identities(list);
    flush_keys();
    if (e == ESP_OK && list->count > 0) {
        app_list_t *cur = templates ? &s_tpl_cur : &s_ident_cur;
        app_list_init(cur, list->count, APP_UI_LIST_ROWS);
        ESP_LOGI(TAG, "%s 共 %d 条%s, 剩余堆 %u", templates ? "Template" : "身份", list->count,
                 list->truncated ? "(已截断)" : "", (unsigned)esp_get_free_heap_size());
        return templates ? ST_TPL_PICK : ST_IDENT_PICK;
    }
    const int status = qca_api_last_status();
    const bool auth_failed = status == 401 || status == 403;
    const char *title = auth_failed ? "PAT 无效或无权限" :
        e == ESP_OK ? (templates ? "没有可用的 Agent模板" : "没有可用的用户身份") : "读取失败";
    const char *body = e == ESP_OK ? "请先在 QCA 平台创建后再刷新" : qca_api_last_error();
    const char *hint = templates || auth_failed ? "OK 重试  长按 OK 设置" : "OK 重试  长按 OK 返回";
    for (;;) {
        app_ui_show_message(UI_TONE_ERROR, title, body, hint, false);
        flush_keys();
        if (wait_ok_or_long()) return templates ? ST_TPL_LOAD : ST_IDENT_LOAD;
        if (!(templates || auth_failed)) return ST_TPL_PICK;
        if (settings_menu(false)) return ST_TPL_LOAD;
    }
}

static app_stage_t stage_pick(bool templates) {
    qca_list_t *list = templates ? &s_templates : &s_idents;
    app_list_t *cur = templates ? &s_tpl_cur : &s_ident_cur;
    app_list_fix_window(cur);
    if (templates) {
        app_ui_show_list("第1步 选择Agent模板", "选择  OK 确认  长按设置", list, cur);
    } else {
        app_ui_show_list("第2步 选择用户身份", "选择  OK 开聊  长按返回", list, cur);
    }
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        const int d = move_delta(&k);
        if (d) {
            app_list_move(cur, d);
            app_ui_list_update(list, cur);
        } else if (is_ok(&k)) {
            ESP_LOGI(TAG, "选中 %s: %s", templates ? "Template" : "身份",
                     list->items[cur->cursor].id);
            return templates ? ST_IDENT_LOAD : ST_CONV;
        } else if (is_ok_long(&k)) {
            if (!templates) return ST_TPL_PICK;
            if (settings_menu(false)) return ST_TPL_LOAD;
            app_ui_show_list("第1步 选择Agent模板", "选择  OK 确认  长按设置", list, cur);
            flush_keys();
        }
    }
}

static app_stage_t stage_conv(void) {
    if (!app_wifi_is_connected()) return ST_WIFI;
    if (!s_audio_ok) {
        app_ui_show_message(UI_TONE_ERROR, "音频不可用", "麦克风/喇叭初始化失败，无法语音对话",
                            "OK 返回", false);
        wait_ok_or_long();
        return ST_IDENT_PICK;
    }
    app_ui_show_message(UI_TONE_INFO, "正在接通助手", s_templates.items[s_tpl_cur.cursor].name,
                        NULL, true);
    esp_err_t e = qca_create_conversation(s_idents.items[s_ident_cur.cursor].id,
                                          s_templates.items[s_tpl_cur.cursor].id,
                                          s_conv_id, sizeof(s_conv_id));
    flush_keys();
    if (e == ESP_OK) return ST_VOICE;
    app_ui_show_message(UI_TONE_ERROR, "创建会话失败", qca_api_last_error(),
                        "OK 重试  长按 OK 返回", false);
    return wait_ok_or_long() ? ST_CONV : ST_IDENT_PICK;
}

static app_stage_t stage_voice(void) {
    app_ui_show_voice(s_templates.items[s_tpl_cur.cursor].name,
                      s_idents.items[s_ident_cur.cursor].name);
    ESP_LOGI(TAG, "开始语音会话，剩余堆 %u，最大块 %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_err_t e = qca_voice_start(s_conv_id);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "语音会话启动失败: %s", esp_err_to_name(e));
        app_ui_show_message(UI_TONE_ERROR, "语音启动失败", "内存或音频资源不足",
                            "OK 重试  长按 OK 返回", false);
        return wait_ok_or_long() ? ST_CONV : ST_IDENT_PICK;
    }
    input_event_t k;
    qca_voice_view_t v;
    for (;;) {
        const bool got = wait_key(&k, 200);
        qca_voice_get_view(&v);
        if (!got) continue;
        if (is_ok_long(&k)) {
            if (!qca_voice_stop()) {
                app_ui_voice_toast("正在结束，请稍候");
                continue;
            }
            flush_keys();
            return ST_IDENT_PICK;
        }
        if (is_ok(&k)) {
            if (v.finished) {
                if (!qca_voice_stop()) continue;
                flush_keys();
                // 出错 → 重新创建会话再连；正常结束 → 回到身份列表。
                return v.state == VS_ERROR ? ST_CONV : ST_IDENT_PICK;
            }
            qca_voice_toggle_pause();
            continue;
        }
        if ((k.btn == BSP_BTN_UP || k.btn == BSP_BTN_DOWN) && k.event != BSP_BTN_LONG) {
            int vol = qca_voice_get_volume() + (k.btn == BSP_BTN_UP ? 10 : -10);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            qca_voice_set_volume(vol);
            char t[24];
            snprintf(t, sizeof(t), "音量 %d%%", vol);
            app_ui_voice_toast(t);
        }
    }
}

static void app_task(void *arg) {
    (void)arg;
    esp_err_t e = app_wifi_init(true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(e));
        app_ui_show_message(UI_TONE_ERROR, "Wi-Fi 初始化失败", esp_err_to_name(e),
                            "长按 OK 重启", false);
        while (wait_ok_or_long()) {}
        esp_restart();
    }
    finish_pending_reset(); // 必须先完成已确认的清除，才允许加载 PAT 或启动门户
    const bool want_prov = app_wifi_take_provision_flag();
    esp_err_t pat_err = qca_pat_load();
    if (pat_err != ESP_OK && pat_err != ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "PAT 读取失败: %s", esp_err_to_name(pat_err));
    // IDF 驱动持久化在本设备重启后会丢 STA 配置；若应用 NVS 有镜像则先回填驱动，
    // 使 has_config 反映真实可用凭据，避免配网成功后又退回门户（镜像即真相源）。
    app_wifi_restore_creds();
    if (want_prov || !app_wifi_has_config() || !qca_pat_present()) {
        ESP_LOGI(TAG, "进入热点配置（用户请求或缺少网络/PAT）");
        run_provision();   // 不返回
    }
    ESP_LOGI(TAG, "配置已就绪，剩余堆 %u", (unsigned)esp_get_free_heap_size());

    app_stage_t st = ST_WIFI;
    for (;;) {
        switch (st) {
        case ST_WIFI:       st = stage_wifi(); break;
        case ST_TPL_LOAD:   st = stage_load(true); break;
        case ST_TPL_PICK:   st = stage_pick(true); break;
        case ST_IDENT_LOAD: st = stage_load(false); break;
        case ST_IDENT_PICK: st = stage_pick(false); break;
        case ST_CONV:       st = stage_conv(); break;
        case ST_VOICE:      st = stage_voice(); break;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Qoder Passport 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    app_ui_init();
    app_ui_show_message(UI_TONE_INFO, "正在启动", "Qoder Passport", NULL, true);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列分配失败，停止启动");
        app_ui_show_message(UI_TONE_ERROR, "内存不足", "请重新启动设备", NULL, false);
        return;
    }
    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }
    s_audio_ok = (bsp_audio_init() == ESP_OK);
    if (!s_audio_ok) ESP_LOGE(TAG, "音频初始化失败");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");
    s_input_ready = true;

    if (xTaskCreate(app_task, "app", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
    }
    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d 剩余堆 %u", button_err == ESP_OK, s_audio_ok,
             (unsigned)esp_get_free_heap_size());
}
