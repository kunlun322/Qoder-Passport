#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct test_channel *i2s_chan_handle_t;
typedef struct {
    int id, role, dma_desc_num, dma_frame_num;
    bool auto_clear_after_cb, auto_clear_before_cb;
    int intr_priority;
} i2s_chan_config_t;
typedef struct {
    struct { int sample_rate_hz, clk_src, ext_clk_freq_hz, mclk_multiple; } clk_cfg;
    struct {
        int data_bit_width, slot_bit_width, slot_mode, slot_mask, ws_width;
        bool ws_pol, bit_shift, left_align, big_endian, bit_order_lsb;
    } slot_cfg;
    struct {
        int mclk, bclk, ws, dout, din;
        struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
    } gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_CLK_SRC_DEFAULT 0
#define I2S_MCLK_MULTIPLE_256 256
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_BIT_WIDTH_AUTO 0
#define I2S_SLOT_MODE_STEREO 2
#define I2S_STD_SLOT_BOTH 3
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
