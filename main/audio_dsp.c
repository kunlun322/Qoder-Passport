// main/audio_dsp.c —— 见 audio_dsp.h。
#include "audio_dsp.h"

static int16_t sat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void dsp_rs_init(dsp_rs_t *rs) {
    rs->lp1 = rs->lp2 = 0;
    rs->npend = 0;
    rs->pend[0] = rs->pend[1] = rs->pend[2] = 0;
}

size_t dsp_rs_24to16(dsp_rs_t *rs, const int16_t *in, size_t n, int16_t *out) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t x = in[i];
        const int16_t y = (int16_t)((x + 2 * (int32_t)rs->lp1 + rs->lp2) / 4);
        rs->lp2 = rs->lp1;
        rs->lp1 = (int16_t)x;
        rs->pend[rs->npend++] = y;
        if (rs->npend == 3) {
            out[w++] = rs->pend[0];
            out[w++] = (int16_t)(((int32_t)rs->pend[1] + rs->pend[2]) / 2);
            rs->npend = 0;
        }
    }
    return w;
}

int dsp_mean_abs(const int16_t *pcm, size_t n) {
    if (n == 0) return 0;
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = pcm[i];
        acc += (uint32_t)(v < 0 ? -v : v);
    }
    return (int)(acc / n);
}

void dsp_gain_ramp(int16_t *pcm, size_t n, int32_t g0_q15, int32_t g1_q15) {
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        const int32_t g = g0_q15 + (int32_t)(((int64_t)(g1_q15 - g0_q15) * (int64_t)i) / (int64_t)n);
        pcm[i] = sat16((int32_t)(((int64_t)pcm[i] * g) >> 15));
    }
}

// sin(2*pi*k/64) * 32767，k = 0..15（四分之一周期，其余由对称性得到）。
static const int16_t SIN_Q[17] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170,
    25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767,
};

static int32_t sin64(uint32_t k) {
    k &= 63u;
    if (k < 16) return SIN_Q[k];
    if (k < 32) return SIN_Q[32 - k];
    if (k < 48) return -SIN_Q[k - 32];
    return -SIN_Q[64 - k];
}

void dsp_tone_add(int16_t *out, size_t n, uint32_t freq_hz, uint32_t sample_rate,
                  int16_t amp, uint32_t *phase_q16) {
    if (sample_rate == 0) return;
    // 每样本相位增量：freq/sr 个周期 = freq*64/sr 个表格点，Q16。
    const uint32_t step = (uint32_t)(((uint64_t)freq_hz * 64u * 65536u) / sample_rate);
    uint32_t ph = *phase_q16;
    for (size_t i = 0; i < n; i++) {
        const uint32_t k = ph >> 16;
        const int32_t frac = (int32_t)(ph & 0xFFFFu);
        const int32_t a = sin64(k), b = sin64(k + 1);
        const int32_t s = a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
        out[i] = sat16(out[i] + (int32_t)(((int64_t)s * amp) / 32767));
        ph += step;
        ph &= (64u << 16) - 1u;
    }
    *phase_q16 = ph;
}
