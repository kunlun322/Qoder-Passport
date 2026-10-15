// main/app_ui.h —— 「Qoder Passport」界面（240x320 竖屏，圆角 30）。
//
// 所有函数内部自行获取 LVGL 锁，可在应用任务里直接调用；不要在持锁时调用。
// 语音页由 LVGL 定时器每 60 ms 轮询 qca_voice_get_view()，语音任务不碰 LVGL。
#pragma once

#include <stdint.h>
#include "app_list.h"
#include "qca_api.h"

#define APP_UI_LIST_ROWS 5

typedef enum {
    UI_TONE_INFO = 0,   // 蓝灰：进行中
    UI_TONE_OK,         // 青绿：完成/就绪
    UI_TONE_WARN,       // 琥珀：需要用户操作
    UI_TONE_ERROR,      // 红：失败
} ui_tone_t;

void app_ui_init(void);

// 通用提示页：一个色点 + 标题 + 正文 + 底部按键提示。spinner=true 时显示转圈。
void app_ui_show_message(ui_tone_t tone, const char *title, const char *body,
                         const char *hint, bool spinner);

// 选择页：title 如“第1步 选择Agent模板”，右上角自动显示 n/N；hint 为底部按键提示
// （▲▼ 图标由本模块加在最前面）。
void app_ui_show_list(const char *title, const char *hint,
                      const qca_list_t *list, const app_list_t *cursor);
void app_ui_list_update(const qca_list_t *list, const app_list_t *cursor);

// 语音页。
void app_ui_show_voice(const char *template_name, const char *identity_name);   // 主标题=Agent 模板，副标题=用户身份
void app_ui_voice_toast(const char *text);   // 短暂覆盖转写行（如“音量 70%”）

// 热点配置页：显示连接二维码、临时密码、网页地址；轮询 app_portal_stage()。
void app_ui_show_provision(void);

// 设备设置：默认选中返回；热点页第一项为取消配置并重启，其余页面为刷新列表。
void app_ui_show_settings(bool provisioning, int selected);
void app_ui_settings_update(int selected);
// 恢复出厂二次确认：默认取消，选中清除后仍需长按 OK 才执行。
void app_ui_show_reset_confirm(bool confirm_selected);
