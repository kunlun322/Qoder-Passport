// main/voice_logic.c —— 见 voice_logic.h。
#include "voice_logic.h"

#include <string.h>

// ---------------- 插话检测 ----------------
#define BARGE_MIC_MIN        1500   // 绝对下限：再安静的环境也要高于此值才算人声
#define BARGE_MARGIN         600
#define BARGE_SUSPECT_FRAMES 3      // 60 ms
#define BARGE_CONFIRM_FRAMES 4      // 80 ms（在 duck 之后继续累积）

void barge_init(barge_det_t *d, int gain_q8) {
    memset(d, 0, sizeof(*d));
    d->gain_q8 = gain_q8 > 0 ? gain_q8 : 256;
}

void barge_push_output(barge_det_t *d, int level) {
    d->out_hist[d->out_idx] = level < 0 ? 0 : level;
    d->out_idx = (d->out_idx + 1) % BARGE_HIST_FRAMES;
}

int barge_echo_estimate(const barge_det_t *d) {
    int mx = 0;
    for (int i = 0; i < BARGE_HIST_FRAMES; i++) {
        if (d->out_hist[i] > mx) mx = d->out_hist[i];
    }
    return (int)(((int64_t)mx * d->gain_q8) >> 8);
}

barge_res_t barge_feed_mic(barge_det_t *d, int mic_level) {
    const int est = barge_echo_estimate(d);
    const bool suspect = mic_level > BARGE_MIC_MIN && mic_level > est + est / 2 + BARGE_MARGIN;
    const bool confirm = mic_level > BARGE_MIC_MIN && mic_level > 2 * est + BARGE_MARGIN;
    d->suspect_frames = suspect ? d->suspect_frames + 1 : 0;
    // 确认计数允许单帧回落（语音能量起伏），连续两帧不满足才清零。
    if (confirm) {
        d->confirm_frames++;
    } else if (d->confirm_frames > 0 && !suspect) {
        d->confirm_frames = 0;
    }
    if (d->confirm_frames >= BARGE_CONFIRM_FRAMES) {
        d->confirm_frames = 0;
        d->suspect_frames = 0;
        return BARGE_CONFIRM;
    }
    if (d->suspect_frames >= BARGE_SUSPECT_FRAMES) return BARGE_SUSPECT;
    return BARGE_NONE;
}

int barge_calibrate_gain_q8(int out_level, int mic_level) {
    if (out_level <= 0) return 256;
    int g = (int)(((int64_t)mic_level * 256 * 2) / out_level);
    if (g < 32) g = 32;
    if (g > 1024) g = 1024;
    return g;
}

// esp_codec_dev 默认音量曲线：0→-50 dB、100→0 dB，线性 0.5 dB/级（BSP 未改曲线）。
// 输出电平在音量之前测量，所以标定后调音量须按同样的 dB 缩放回声增益。
#define VOL_STEP_UP_Q16   69419   // 10^(+0.5/20) ≈ 1.05925
#define VOL_STEP_DOWN_Q16 61870   // 10^(-0.5/20) ≈ 0.94406
int barge_gain_for_volume(int gain_q8, int calib_vol, int vol) {
    if (gain_q8 <= 0) gain_q8 = 256;
    if (calib_vol < 0 || calib_vol > 100 || vol < 0 || vol > 100) return gain_q8;
    int64_t g = (int64_t)gain_q8 << 16;
    const int steps = vol - calib_vol;
    const int64_t f = steps > 0 ? VOL_STEP_UP_Q16 : VOL_STEP_DOWN_Q16;
    for (int i = 0; i < (steps > 0 ? steps : -steps); i++) g = (g * f) >> 16;
    int r = (int)((g + 32768) >> 16);
    if (r < 16) r = 16;
    if (r > 4096) r = 4096;
    return r;
}

// ---------------- 本地 VAD ----------------
#define VAD_FLOOR_INIT 200
#define VAD_FLOOR_MIN  50
#define VAD_FLOOR_MAX  3000
#define VAD_THR_MIN    800

void vad_init(vad_t *v, int start_frames, int end_frames) {
    memset(v, 0, sizeof(*v));
    v->floor = VAD_FLOOR_INIT;
    v->start_frames = start_frames > 0 ? start_frames : 1;
    v->end_frames = end_frames > 0 ? end_frames : 1;
}

int vad_threshold(const vad_t *v) {
    const int t = 3 * v->floor;
    return t > VAD_THR_MIN ? t : VAD_THR_MIN;
}

void vad_reset_run(vad_t *v) {
    v->above = v->below = 0;
    v->in_speech = false;
}

vad_ev_t vad_feed(vad_t *v, int level) {
    const int thr = vad_threshold(v);
    const bool loud = level > thr;
    // 噪声底：只在不说话且不响时慢速跟踪（约 0.6 s 时间常数）。
    if (!v->in_speech && !loud) {
        v->floor += (level - v->floor) / 32;
        if (v->floor < VAD_FLOOR_MIN) v->floor = VAD_FLOOR_MIN;
        if (v->floor > VAD_FLOOR_MAX) v->floor = VAD_FLOOR_MAX;
    }
    if (loud) {
        v->above++;
        v->below = 0;
    } else {
        v->below++;
        if (!v->in_speech) v->above = 0;
    }
    if (!v->in_speech && v->above >= v->start_frames) {
        v->in_speech = true;
        v->below = 0;
        return VAD_SPEECH_START;
    }
    if (v->in_speech && v->below >= v->end_frames) {
        v->in_speech = false;
        v->above = 0;
        return VAD_SPEECH_END;
    }
    return VAD_NONE;
}

// ---------------- 语音口令 ----------------
bool voice_is_pause_phrase(const char *s) {
    if (!s) return false;
    // 太长的句子多半是在讨论“暂停”这个词，而不是下指令（约 12 个汉字以内）。
    if (strlen(s) > 36) return false;
    static const char *const PHRASES[] = { "暂停", "停一下", "休息一下" };
    for (size_t i = 0; i < sizeof(PHRASES) / sizeof(PHRASES[0]); i++) {
        if (strstr(s, PHRASES[i])) return true;
    }
    return false;
}

// ---------------- 状态机 ----------------
static bool is_active(vs_state_t st) {
    return st == VS_LISTENING || st == VS_THINKING || st == VS_SPEAKING;
}

static void go(voice_fsm_t *f, vs_actions_t *a, vs_state_t st, int64_t now) {
    if (f->st != st) a->changed = true;
    f->st = st;
    f->since_ms = now;
}

vs_event_t voice_disconnect_event(bool ready, bool transport_failed, int close_code) {
    if (!ready || transport_failed || close_code != 1000) return VE_ERROR;
    return VE_CLOSED;
}

void voice_fsm_init(voice_fsm_t *f, int64_t now_ms) {
    memset(f, 0, sizeof(*f));
    f->st = VS_CONNECTING;
    f->since_ms = now_ms;
    f->last_activity_ms = now_ms;
}

vs_actions_t voice_fsm_event(voice_fsm_t *f, vs_event_t ev, int64_t now) {
    vs_actions_t a = { 0 };
    if (f->st == VS_ENDED || f->st == VS_ERROR) return a;

    switch (ev) {
    case VE_READY:
        if (f->st == VS_CONNECTING) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
            a.resume_chime = true;
        }
        break;
    case VE_SPEECH_START:
        if (f->st == VS_PAUSED) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
            a.resume_chime = true;
        } else if (f->st == VS_THINKING) {
            go(f, &a, VS_LISTENING, now);   // 用户还没说完，接着听
            f->last_activity_ms = now;
        } else if (f->st == VS_LISTENING) {
            f->last_activity_ms = now;
        }
        break;
    case VE_SPEECH_END:
    case VE_SERVER_THINKING:
        if (f->st == VS_LISTENING) {
            go(f, &a, VS_THINKING, now);
            f->last_activity_ms = now;
        }
        break;
    case VE_AUDIO_FIRST:
        if (f->st == VS_LISTENING || f->st == VS_THINKING) {
            go(f, &a, VS_SPEAKING, now);
            f->last_activity_ms = now;
        }
        break;
    case VE_ACTIVITY:
        if (is_active(f->st)) f->last_activity_ms = now;
        break;
    case VE_PLAYBACK_DRAINED:
    case VE_SERVER_INTERRUPT:
        // 服务端停播后会转入 idle 等待用户；本地同样回到聆听，等喇叭余响后开上行。
        if (f->st == VS_SPEAKING) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
            f->tail_until_ms = now + VS_TAIL_MS;
        }
        break;
    case VE_BARGE:
        if (f->st == VS_SPEAKING) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
            f->tail_until_ms = now;   // 立即开上行，用户的话不能丢
            a.send_interrupt = true;
        }
        break;
    case VE_PAUSE_PHRASE:
        if (is_active(f->st)) {
            go(f, &a, VS_PAUSED, now);
            a.send_interrupt = true;  // 不让助手去回答“暂停”这句话
            a.pause_chime = true;
        }
        break;
    case VE_OK_SHORT:
        if (is_active(f->st)) {
            go(f, &a, VS_PAUSED, now);
            a.send_interrupt = true;
            a.pause_chime = true;
        } else if (f->st == VS_PAUSED) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
            f->tail_until_ms = now;
            a.resume_chime = true;
        }
        break;
    case VE_TICK:
        if (f->st == VS_THINKING && now - f->since_ms >= VS_THINK_TIMEOUT_MS) {
            go(f, &a, VS_LISTENING, now);
            f->last_activity_ms = now;
        } else if ((f->st == VS_LISTENING || f->st == VS_THINKING) &&
                   now - f->last_activity_ms >= VS_IDLE_PAUSE_MS) {
            go(f, &a, VS_PAUSED, now);
            a.pause_chime = true;
        } else if (f->st == VS_PAUSED && now - f->since_ms >= VS_PAUSE_END_MS) {
            go(f, &a, VS_ENDED, now);
            a.disconnect = true;
        }
        break;
    case VE_ERROR:
        go(f, &a, VS_ERROR, now);
        a.disconnect = true;
        break;
    case VE_CLOSED:
        go(f, &a, VS_ENDED, now);
        a.disconnect = true;
        break;
    }
    return a;
}

bool voice_fsm_uplink_open(const voice_fsm_t *f, int64_t now_ms) {
    if (f->st != VS_LISTENING && f->st != VS_THINKING) return false;
    return now_ms >= f->tail_until_ms;
}

const char *voice_state_word(vs_state_t st) {
    switch (st) {
    case VS_CONNECTING: return "连接中";
    case VS_LISTENING:  return "聆听中";
    case VS_THINKING:   return "思考中";
    case VS_SPEAKING:   return "回答中";
    case VS_PAUSED:     return "已暂停";
    case VS_ENDED:      return "已结束";
    case VS_ERROR:      return "出错了";
    }
    return "";
}

const char *voice_state_guide(vs_state_t st) {
    switch (st) {
    case VS_CONNECTING: return "正在接通助手，请稍候";
    case VS_LISTENING:  return "直接说话，说完停一下即可";
    case VS_THINKING:   return "助手正在想，马上回答";
    case VS_SPEAKING:   return "想插话就大声说，它会停下";
    case VS_PAUSED:     return "对着它说句话即可继续";
    case VS_ENDED:      return "会话已结束，OK 返回";
    case VS_ERROR:      return "OK 重试，长按 OK 返回";
    }
    return "";
}
