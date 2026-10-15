// main/voice_logic.h —— 实时语音的“何时开始 / 何时暂停”判定（纯逻辑，可 host 测试）。
//
// 本板 ES8311 没有回声参考通道，做不了 AEC：助手说话时喇叭声会被麦克风原样拾取。
// 若此时继续上行，服务端 VAD 会把助手自己的声音当成用户插话。因此采用
// “播放时上行门控 + 本地插话检测”：
//   * 回答中（喇叭在放）上行关闭；本地比较麦克风电平与“回声估计”
//     （回声增益 × 近 200 ms 输出电平），明显高出即判定用户插话：
//     先压低播放（duck），持续确认后打断助手、立即打开上行。
//   * 聆听 / 思考中上行常开，由服务端 VAD 判断一句话的起止。
// 所有时间以毫秒传入，帧长固定 20 ms。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VOICE_FRAME_MS 20

// ---------------- 插话检测 ----------------
#define BARGE_HIST_FRAMES 10   // 200 ms 输出电平历史

typedef struct {
    int gain_q8;                      // 回声增益（麦克风电平/输出电平）×256
    int out_hist[BARGE_HIST_FRAMES];  // 最近每帧的输出电平
    int out_idx;
    int suspect_frames;
    int confirm_frames;
} barge_det_t;

typedef enum { BARGE_NONE = 0, BARGE_SUSPECT, BARGE_CONFIRM } barge_res_t;

void barge_init(barge_det_t *d, int gain_q8);
// 每播放一帧（20 ms）调用一次，level 为该帧输出平均绝对值（静音帧传 0）。
void barge_push_output(barge_det_t *d, int level);
// 每采集一帧调用一次。返回 SUSPECT（建议压低播放）或 CONFIRM（确认插话）。
barge_res_t barge_feed_mic(barge_det_t *d, int mic_level);
// 当前回声估计（供调试日志）。
int barge_echo_estimate(const barge_det_t *d);
// 由提示音测得的输出/输入电平计算回声增益（×2 余量，限定在 0.125~4.0）。
int barge_calibrate_gain_q8(int out_level, int mic_level);
// 标定时音量为 calib_vol；当前音量 vol 下的回声增益（0.5 dB/级），限幅 16..4096。
int barge_gain_for_volume(int gain_q8, int calib_vol, int vol);

// ---------------- 本地 VAD（只做“有没有人在说话”的粗判） ----------------
typedef struct {
    int floor;           // 噪声底估计
    int start_frames;    // 连续多少帧高于门限才算“开始说话”
    int end_frames;      // 连续多少帧低于门限才算“说完”
    int above, below;
    bool in_speech;
} vad_t;

typedef enum { VAD_NONE = 0, VAD_SPEECH_START, VAD_SPEECH_END } vad_ev_t;

void vad_init(vad_t *v, int start_frames, int end_frames);
vad_ev_t vad_feed(vad_t *v, int level);
int vad_threshold(const vad_t *v);
void vad_reset_run(vad_t *v);   // 清除进行中的计数（保留噪声底）

// ---------------- 语音口令 ----------------
// 用户整句很短且含“暂停 / 停一下 / 休息一下”时视为暂停口令。
bool voice_is_pause_phrase(const char *utf8);

// ---------------- 会话状态机 ----------------
typedef enum {
    VS_CONNECTING = 0,  // 连接中
    VS_LISTENING,       // 聆听中：上行开，直接说话
    VS_THINKING,        // 思考中：用户刚说完，等待助手回答
    VS_SPEAKING,        // 回答中：喇叭在放，上行门控
    VS_PAUSED,          // 已暂停：上行关闭，等待“出声唤回”或 OK
    VS_ENDED,           // 已结束
    VS_ERROR,           // 出错了
} vs_state_t;

typedef enum {
    VE_READY = 0,          // 服务端 voice.ready
    VE_SPEECH_START,       // 本地 VAD：用户开始说话
    VE_SPEECH_END,         // 本地 VAD：用户说完（静音 700 ms）
    VE_SERVER_THINKING,    // 服务端状态 thinking/processing
    VE_AUDIO_FIRST,        // 本轮第一段助手音频到达
    VE_ACTIVITY,           // 任意一侧有活动（音频片段、转写），刷新空闲计时
    VE_PLAYBACK_DRAINED,   // 助手音频播完
    VE_BARGE,              // 本地确认插话
    VE_SERVER_INTERRUPT,   // 服务端 playback.interrupt：停播并回到聆听（不再回发 interrupt）
    VE_PAUSE_PHRASE,       // 用户说了暂停口令
    VE_OK_SHORT,           // 短按 OK：暂停/继续 切换
    VE_TICK,               // 周期调用（建议 ≥ 每 100 ms）
    VE_ERROR,              // 网络/服务错误
    VE_CLOSED,             // 连接关闭
} vs_event_t;

#define VS_TAIL_MS          250      // 播完后再等 250 ms 才开上行（喇叭余响）
#define VS_THINK_TIMEOUT_MS 15000    // 思考 15 s 无回应 → 回到聆听
#define VS_IDLE_PAUSE_MS    60000    // 双方 60 s 无活动 → 自动暂停
#define VS_PAUSE_END_MS     300000   // 暂停 5 min → 自动结束会话

typedef struct {
    vs_state_t st;
    int64_t since_ms;          // 进入当前状态的时刻
    int64_t last_activity_ms;  // 最近一次活动
    int64_t tail_until_ms;     // 上行恢复时刻
} voice_fsm_t;

typedef struct {
    bool changed;          // 状态发生变化（需刷新界面）
    bool send_interrupt;   // 需向服务端发送 interrupt（并取消本地播放）
    bool pause_chime;      // 播放“暂停”下行提示音
    bool resume_chime;     // 播放“开始/继续”上行提示音
    bool disconnect;       // 需结束会话
} vs_actions_t;

// WebSocket 意外断线属于可重试错误，不能伪装成“正常结束”。1000 是正常关闭码。
vs_event_t voice_disconnect_event(bool ready, bool transport_failed, int close_code);
void voice_fsm_init(voice_fsm_t *f, int64_t now_ms);
vs_actions_t voice_fsm_event(voice_fsm_t *f, vs_event_t ev, int64_t now_ms);
bool voice_fsm_uplink_open(const voice_fsm_t *f, int64_t now_ms);
const char *voice_state_word(vs_state_t st);    // 大字状态（28px 字库已覆盖）
const char *voice_state_guide(vs_state_t st);   // 一行操作指引（16px 字库）
