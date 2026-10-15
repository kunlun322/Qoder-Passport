// main/demo_audio.c —— 播 1kHz 方波 / 录 3 秒后回放。
// 音频收发会阻塞较久,故放到独立任务里跑,不占用按键回调与 LVGL 任务。
// Continuous BGM + UI/NVS needs feed-latency and Flash/cache checks; a tone alone
// does not validate this. See docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md
// section 8.1 before reusing this demo for continuous playback.
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"   // bsp_lvgl_lock / bsp_lvgl_unlock(音频任务里操作 LVGL 要加锁)
#include "ui_pixel.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_audio";

#define SAMPLE_RATE   16000
#define TONE_HZ        1000
#define TONE_MS        1000
#define RECORD_SEC        3
#define CHUNK_SAMPLES   512          // 每次收发的采样数,控制临时缓冲大小
#define AUDIO_STOP_TIMEOUT_MS 2000

typedef enum {
    AUDIO_COMMAND_TONE = 1,
    AUDIO_COMMAND_RECORD,
    AUDIO_COMMAND_STOP,
} audio_command_t;

static lv_obj_t *s_scr, *s_status, *s_mascot;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static volatile bool s_cancel;

// LVGL 对象只能在持锁时操作;本函数从音频任务调用,故内部加锁。
static void set_status(const char *text) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_status) lv_label_set_text(s_status, text);
    bsp_lvgl_unlock();
}

static void play_tone(void) {
    set_status("playing 1kHz...");
    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) { set_status("format failed"); return; }
    bsp_audio_set_volume(80);

    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (!buf) { set_status("out of memory"); return; }

    const int period = SAMPLE_RATE / TONE_HZ;        // 每个方波周期的采样数
    int total = SAMPLE_RATE * TONE_MS / 1000;
    int phase = 0;
    while (total > 0 && !s_cancel) {
        int n = total < CHUNK_SAMPLES ? total : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < period / 2) ? 6000 : -6000;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            set_status("playback failed");
            break;
        }
        total -= n;
    }
    free(buf);
    if (!s_cancel && total == 0) set_status("done. OK: tone  UP: record");
}

static void record_and_play(void) {
    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) { set_status("format failed"); return; }

    size_t total = (size_t)SAMPLE_RATE * RECORD_SEC;
    int16_t *rec = malloc(total * sizeof(int16_t));   // 3s @16k 16bit = 96KB
    if (!rec) {
        // C3 无 PSRAM,96KB 可能分配不到 —— 明确告知而不是静默失败
        ESP_LOGE(TAG, "录音缓冲 %u 字节分配失败(C3 内存紧张,可缩短 RECORD_SEC)",
                 (unsigned)(total * sizeof(int16_t)));
        set_status("record buffer alloc failed");
        return;
    }

    set_status("recording 3s... speak now");
    size_t got = 0;
    while (got < total && !s_cancel) {
        size_t n = (total - got) < CHUNK_SAMPLES ? (total - got) : CHUNK_SAMPLES;
        if (bsp_audio_read(rec + got, n * sizeof(int16_t)) != ESP_OK) {
            free(rec);
            if (!s_cancel) set_status("recording failed");
            return;
        }
        got += n;
    }

    if (!s_cancel) {
        set_status("playing back...");
        bsp_audio_set_volume(80);
    }
    size_t played = 0;
    while (played < got && !s_cancel) {
        size_t n = (got - played) < CHUNK_SAMPLES ? (got - played) : CHUNK_SAMPLES;
        if (bsp_audio_write(rec + played, n * sizeof(int16_t)) != ESP_OK) {
            set_status("playback failed");
            break;
        }
        played += n;
    }
    free(rec);
    if (!s_cancel && got == total && played == total) set_status("done. OK: tone  UP: record");
}

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t command = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command == AUDIO_COMMAND_STOP) break;
        if (command == AUDIO_COMMAND_TONE) play_tone();
        else if (command == AUDIO_COMMAND_RECORD) record_and_play();
    }
    // The lifecycle owner retains the handle until it receives this acknowledgement.
    // No shared state or UI accesses are allowed after giving it. Stay alive so a
    // timed-out stop can safely retry; only the owner deletes/replaces this task.
    xSemaphoreGive(s_stopped);
    for (;;) vTaskSuspend(NULL);
}

void demo_audio_enter(void) {
    s_scr = ui_pixel_screen_create("AUDIO");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 62, 204, 168, UI_PAPER);

    lv_obj_t *record = ui_pixel_panel_create(panel, 58, 12, 72, 72, UI_INK);
    lv_obj_t *disc = lv_obj_create(record);
    lv_obj_set_size(disc, 36, 36);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_center(disc);

    s_status = lv_label_create(panel);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 176);
    lv_label_set_text(s_status, "OK: 1kHz TONE\nUP: RECORD + PLAY");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -9);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);

    lv_screen_load(s_scr);
}

esp_err_t demo_audio_start(void) {
    if (s_task) return ESP_OK;
    if (s_stopped) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
    }
    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) {
        set_status("Cannot create audio worker");
        return ESP_ERR_NO_MEM;
    }
    s_cancel = false;
    if (xTaskCreate(audio_task, "demo_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
        set_status("Cannot create audio worker");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t demo_audio_stop(void) {
    TaskHandle_t task = s_task;
    if (!task) {
        if (s_stopped) {
            vSemaphoreDelete(s_stopped);
            s_stopped = NULL;
        }
        return ESP_OK;
    }

    s_cancel = true;
    xTaskNotify(task, AUDIO_COMMAND_STOP, eSetValueWithOverwrite);
    if (!s_stopped ||
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(AUDIO_STOP_TIMEOUT_MS)) != pdTRUE) {
        set_status("Audio stop timed out; retry");
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelete(task);
    s_task = NULL;
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    return ESP_OK;
}

void demo_audio_exit(void) {
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_status = s_mascot = NULL; }
}

void demo_audio_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK || !s_task || s_cancel) return;
    uint32_t command = 0;
    if (btn == BSP_BTN_OK) command = AUDIO_COMMAND_TONE;
    else if (btn == BSP_BTN_UP) command = AUDIO_COMMAND_RECORD;
    if (!command) return;

    xTaskNotify(s_task, command, eSetValueWithOverwrite);
    if (!bsp_lvgl_lock(250)) return;
    if (s_mascot) ui_pixel_mascot_jump(s_mascot);
    bsp_lvgl_unlock();
}
