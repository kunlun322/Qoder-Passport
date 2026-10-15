#pragma once
#include "esp_codec_dev.h"
typedef struct { uint8_t port, addr; void *bus_handle; int clock_speed_hz; } audio_codec_i2c_cfg_t;
typedef struct { uint8_t port; void *rx_handle, *tx_handle; int clk_src; } audio_codec_i2s_cfg_t;
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *);
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *);
int audio_codec_delete_data_if(const audio_codec_data_if_t *);
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *);
int audio_codec_delete_codec_if(const audio_codec_if_t *);
