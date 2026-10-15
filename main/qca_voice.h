// main/qca_voice.h —— QCA Realtime 实时语音会话（WebSocket + 麦克风上行 + 喇叭下行）。
//
// 线程模型：
//   ctl 任务   ：持有状态机（voice_logic），处理事件、执行动作（打断、提示音、结束）。
//   ws 任务    ：esp_websocket_client 内部任务，流式解析下行消息并把 PCM 写入播放缓冲。
//   mic 任务   ：每 20 ms 读一帧麦克风，做本地 VAD / 插话检测，按 80 ms 打包上行。
//               mic/play 任务与播放缓冲在 TLS 握手完成后才创建，会话结束释放（C3 无 PSRAM）。
//   play 任务  ：从播放缓冲取 PCM，预缓冲、淡入淡出、压低（duck），写 I2S。
// 界面通过 qca_voice_get_view() 轮询快照，不在语音任务里碰 LVGL。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice_logic.h"

// 语音页对话卡片 5 行 × 约 12 字；168 字节 ≈ 56 个汉字，加“助手：”前缀正好放满。
#define QCA_VOICE_LINE_MAX 168

typedef struct {
    vs_state_t state;
    int level;                      // 0..100，麦克风或喇叭电平，用于呼吸环
    bool finished;                  // 会话已结束（正常或出错），任务均已退出
    uint32_t line_seq;              // line 每次更新 +1
    bool line_is_user;              // true=用户说的话，false=助手/系统
    char line[QCA_VOICE_LINE_MAX];  // 最近一句转写或提示（已净化）
    char error[64];                 // 出错原因（state==VS_ERROR 时有效）
} qca_voice_view_t;

// 连接 conv_id 对应的 Realtime 会话并开始语音交互。非阻塞。
esp_err_t qca_voice_start(const char *conv_id);

// 短按 OK：暂停 / 继续。
void qca_voice_toggle_pause(void);

// 请求结束并等待退出屏障（每次最多等 4 s）。true=所有任务/缓冲已清理，允许重试；
// false=仍在收尾，调用方必须留在当前页等待，不能开始新会话。可重复调用。
bool qca_voice_stop(void);

void qca_voice_get_view(qca_voice_view_t *out);

// 喇叭音量 0..100（会话外也可调用，下一次会话生效）。
void qca_voice_set_volume(int percent);
int qca_voice_get_volume(void);

// 恢复出厂：只删除音量 key，必须在会话完全停止后由应用任务调用。
esp_err_t qca_voice_reset_volume(void);
