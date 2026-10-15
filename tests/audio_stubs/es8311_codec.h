#pragma once
#include "esp_codec_dev_defaults.h"
#define ESP_CODEC_DEV_WORK_MODE_BOTH 3
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    int codec_mode, pa_pin;
    bool pa_reverted, master_mode, use_mclk;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
    bool no_dac_ref;
} es8311_codec_cfg_t;
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *);
