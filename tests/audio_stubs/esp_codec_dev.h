#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_CODEC_DEV_OK ESP_OK
#define ESP_CODEC_DEV_NOT_SUPPORT ESP_ERR_NOT_SUPPORTED
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel) ((uint16_t)1 << (channel))
typedef enum { ESP_CODEC_DEV_TYPE_NONE = 0, ESP_CODEC_DEV_TYPE_IN = 1,
               ESP_CODEC_DEV_TYPE_OUT = 2, ESP_CODEC_DEV_TYPE_IN_OUT = 3 } esp_codec_dev_type_t;
typedef struct { uint8_t bits_per_sample, channel; uint16_t channel_mask;
                 uint32_t sample_rate; int mclk_multiple; } esp_codec_dev_sample_info_t;
typedef struct audio_codec_ctrl_if_t audio_codec_ctrl_if_t;
typedef struct audio_codec_data_if_t audio_codec_data_if_t;
typedef struct audio_codec_if_t audio_codec_if_t;
typedef struct { int type; struct { uint16_t addr; uint8_t port; } i2c; } audio_codec_ctrl_info_t;
struct audio_codec_ctrl_if_t {
    int (*open)(const audio_codec_ctrl_if_t *, void *, int);
    bool (*is_open)(const audio_codec_ctrl_if_t *);
    int (*read_reg)(const audio_codec_ctrl_if_t *, int, int, void *, int);
    int (*write_reg)(const audio_codec_ctrl_if_t *, int, int, void *, int);
    int (*get_info)(const audio_codec_ctrl_if_t *, audio_codec_ctrl_info_t *);
    int (*close)(const audio_codec_ctrl_if_t *);
};
struct audio_codec_data_if_t {
    int (*open)(const audio_codec_data_if_t *, void *, int);
    bool (*is_open)(const audio_codec_data_if_t *);
    int (*enable)(const audio_codec_data_if_t *, esp_codec_dev_type_t, bool);
    int (*set_fmt)(const audio_codec_data_if_t *, esp_codec_dev_type_t, esp_codec_dev_sample_info_t *);
    int (*read)(const audio_codec_data_if_t *, uint8_t *, int);
    int (*write)(const audio_codec_data_if_t *, uint8_t *, int);
    int (*close)(const audio_codec_data_if_t *);
};
struct audio_codec_if_t { int (*enable)(const audio_codec_if_t *, bool); };
typedef struct { int unused; } audio_codec_gpio_if_t;
typedef struct test_codec_dev *esp_codec_dev_handle_t;
typedef struct { esp_codec_dev_type_t dev_type; const audio_codec_if_t *codec_if;
                 const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *);
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *);
int esp_codec_dev_close(esp_codec_dev_handle_t);
void esp_codec_dev_delete(esp_codec_dev_handle_t);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_write(esp_codec_dev_handle_t, void *, int);
int esp_codec_dev_read(esp_codec_dev_handle_t, void *, int);
