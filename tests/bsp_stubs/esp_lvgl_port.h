#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
typedef struct { uint8_t *data; struct { uint32_t stride; } header; } lv_draw_buf_t;
typedef struct _lv_display_t { lv_draw_buf_t buffer; } lv_display_t;
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef struct { lv_display_t *target; lv_area_t *area; } lv_event_t;
typedef struct { int unused; } lv_event_dsc_t;
typedef struct { int unused; } lvgl_port_cfg_t;
#define ESP_LVGL_PORT_INIT_CONFIG() {0}
#define LV_EVENT_FLUSH_START 1
#define LV_COLOR_FORMAT_RGB565 1
typedef struct {
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_panel_io_handle_t io_handle;
    uint32_t buffer_size;
    bool double_buffer;
    int hres, vres;
    struct { bool swap_xy, mirror_x, mirror_y; } rotation;
    struct { bool buff_dma, swap_bytes; } flags;
} lvgl_port_display_cfg_t;
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *);
esp_err_t lvgl_port_deinit(void);
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *);
esp_err_t lvgl_port_remove_disp(lv_display_t *);
void lv_display_add_event_cb(lv_display_t *, void (*)(lv_event_t *), int, void *);
uint32_t lv_display_get_event_count(lv_display_t *);
static inline void *lv_event_get_target(lv_event_t *ev) { return ev->target; }
static inline void *lv_event_get_param(lv_event_t *ev) { return ev->area; }
static inline lv_draw_buf_t *lv_display_get_buf_active(lv_display_t *disp) { return &disp->buffer; }
static inline int lv_display_get_color_format(lv_display_t *disp) { (void)disp; return LV_COLOR_FORMAT_RGB565; }
static inline int32_t lv_area_get_width(const lv_area_t *a) { return a->x2 - a->x1 + 1; }
