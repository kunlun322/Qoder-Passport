// main/audio_dsp.h —— 实时语音用的小型定点 DSP（纯逻辑，可 host 测试）。
//
// * QCA Realtime 下行是 24 kHz PCM16 单声道，而 ES8311 全双工 I2S 只能工作在
//   一个采样率上（上行需要 16 kHz），所以下行在设备端做 24k→16k（3:2）重采样。
//   先做 [1,2,1]/4 低通抑制混叠，再按 3 进 2 出线性插值（输出点位于 0 与 1.5）。
// * 电平统计（帧平均绝对值）供本地 VAD / 插话检测使用。
// * 增益斜坡用于淡入淡出，避免起停爆音。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t lp1, lp2;   // 低通历史 x[n-1], x[n-2]
    int16_t pend[3];    // 等待凑满 3 个的低通输出
    uint8_t npend;
} dsp_rs_t;

void dsp_rs_init(dsp_rs_t *rs);

// 24k→16k 流式重采样。out 容量至少为 (n + 2) * 2 / 3 + 1 个样本。返回输出样本数。
size_t dsp_rs_24to16(dsp_rs_t *rs, const int16_t *in, size_t n, int16_t *out);

// 帧平均绝对值（0..32768）。n==0 返回 0。
int dsp_mean_abs(const int16_t *pcm, size_t n);

// 原地乘以从 g0 线性过渡到 g1 的增益（Q15，32768 = 1.0）。
void dsp_gain_ramp(int16_t *pcm, size_t n, int32_t g0_q15, int32_t g1_q15);

// 用 64 点正弦表合成一段正弦音（叠加写入 out，带饱和）。phase 为 Q16 相位累加器，
// 跨调用保持连续。amp 为峰值（0..32767）。
void dsp_tone_add(int16_t *out, size_t n, uint32_t freq_hz, uint32_t sample_rate,
                  int16_t amp, uint32_t *phase_q16);
