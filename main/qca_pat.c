// main/qca_pat.c —— 见 qca_pat.h。
#include "qca_pat.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "portal_form.h"

static const char *TAG = "qca_pat";

#define NVS_NS      "qca_app"
#define NVS_KEY_PAT "pat"

static char s_pat[QCA_PAT_MAX];

esp_err_t qca_pat_load(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        s_pat[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    if (e != ESP_OK) return e;
    size_t len = sizeof(s_pat);
    e = nvs_get_str(h, NVS_KEY_PAT, s_pat, &len);
    nvs_close(h);
    if (e != ESP_OK) {
        s_pat[0] = '\0';
        return e == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : e;
    }
    if (!form_pat_valid(s_pat, QCA_PAT_MAX - 1)) {
        memset(s_pat, 0, sizeof(s_pat));
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "已读取 PAT（内容不记录）");
    return ESP_OK;
}

bool qca_pat_present(void) {
    return s_pat[0] != '\0';
}

const char *qca_pat(void) {
    return s_pat;
}

esp_err_t qca_pat_use(const char *pat, bool persist) {
    if (!pat || (pat[0] && !form_pat_valid(pat, QCA_PAT_MAX - 1))) return ESP_ERR_INVALID_ARG;
    esp_err_t e = ESP_OK;
    if (persist) {
        nvs_handle_t h;
        e = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (e != ESP_OK) return e;
        e = pat[0] ? nvs_set_str(h, NVS_KEY_PAT, pat) : nvs_erase_key(h, NVS_KEY_PAT);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK && pat != s_pat) {
        memset(s_pat, 0, sizeof(s_pat));
        snprintf(s_pat, sizeof(s_pat), "%s", pat);
    }
    if (persist && e != ESP_OK) ESP_LOGE(TAG, "PAT 保存失败: %s", esp_err_to_name(e));
    return e;
}
