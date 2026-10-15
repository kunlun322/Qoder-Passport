#!/usr/bin/env python3
"""Source-contract regression checks, not execution of ESP-IDF/NVS or device tests."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI = (ROOT / "main/app_wifi.c").read_text(encoding="utf-8")
MAIN = (ROOT / "main/main.c").read_text(encoding="utf-8")
UI = (ROOT / "main/app_ui.c").read_text(encoding="utf-8")
VOICE = (ROOT / "main/qca_voice.c").read_text(encoding="utf-8")
PAT = (ROOT / "main/qca_pat.c").read_text(encoding="utf-8")


def function(source, name):
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", source)
    assert match, name
    # Strip comments/strings only for matching braces; retain original body for checks.
    start = match.end()
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\{|\}')
    depth = 1
    for token in tokens.finditer(source, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.start()]
    raise AssertionError("Unbalanced function: " + name)


def ordered(test, body, *items):
    cursor = 0
    for item in items:
        position = body.find(item, cursor)
        test.assertGreaterEqual(position, cursor, f"Missing/out-of-order operation: {item}")
        cursor = position + len(item)


class FactoryResetContract(unittest.TestCase):
    def test_intent_committed_before_restart_without_deletion(self):
        body = function(WIFI, "app_wifi_request_factory_reset")
        ordered(self, body, "nvs_set_u8(h, NVS_KEY_RESET, 1)", "nvs_commit(h)",
                "if (e != ESP_OK) return e", "esp_restart()")
        self.assertNotIn("nvs_erase", body)
        self.assertNotIn("esp_wifi_restore", body)
        self.assertNotIn("qca_pat_use", body)

    def test_pending_read_errors_are_not_treated_as_false(self):
        body = function(WIFI, "app_wifi_factory_reset_pending")
        self.assertIn("if (!pending) return ESP_ERR_INVALID_ARG", body)
        self.assertIn("if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK", body)
        self.assertIn("*pending = s_reset_in_progress", body)
        self.assertIn("if (e == ESP_OK) *pending = s_reset_in_progress || v != 0", body)
        self.assertIn("return e", body)

    def test_reset_finishes_before_any_credentials_or_workers_load(self):
        body = function(MAIN, "app_task")
        ordered(self, body, "app_wifi_init(true)", "finish_pending_reset()",
                "app_wifi_take_provision_flag()", "qca_pat_load()",
                "app_wifi_restore_creds()", "run_provision()")
        body = function(MAIN, "finish_pending_reset")
        ordered(self, body, "app_wifi_factory_reset_pending(&pending)",
                "app_wifi_finish_factory_reset()", "设置已清除", "esp_restart()")
        self.assertIn("if (e == ESP_OK && !pending) return", body)
        self.assertIn("if (!wait_ok_or_long()) esp_restart()", body)

    def test_scoped_idempotent_deletion_order(self):
        body = function(WIFI, "app_wifi_finish_factory_reset")
        ordered(self, body, "if (!pending) return ESP_ERR_INVALID_STATE",
                "app_wifi_set_auto_reconnect(false)", "esp_wifi_stop()",
                "esp_wifi_set_storage(WIFI_STORAGE_FLASH)", "esp_wifi_restore()",
                "app_wifi_clear_creds()", 'qca_pat_use("", true)',
                "qca_voice_reset_volume()",
                "if (e != ESP_OK) return e", "nvs_erase_key(h, NVS_KEY_PROV)",
                "nvs_erase_key(h, NVS_KEY_RESET)", "nvs_commit(h)")
        self.assertIn("ESP_ERR_NVS_NOT_FOUND", body)
        self.assertNotIn("nvs_flash_erase", WIFI)
        self.assertNotIn("nvs_erase_all", WIFI)
        self.assertNotIn("nvs_erase_all", PAT + VOICE)
        self.assertIn('NVS_KEY_RESET "reset"', WIFI)
        self.assertIn('NVS_KEY_PAT "pat"', PAT)
        self.assertIn('VOL_NVS_NS   "qca_voice"', VOICE)
        self.assertIn('VOL_NVS_KEY  "vol"', VOICE)

    def test_volume_reset_rejects_running_voice_and_clears_cache(self):
        body = function(VOICE, "qca_voice_reset_volume")
        ordered(self, body, "if (s_running) return ESP_ERR_INVALID_STATE",
                "nvs_erase_key(h, VOL_NVS_KEY)", "nvs_commit(h)",
                "if (e == ESP_OK) {", "s_volume = DEFAULT_VOLUME")
        self.assertIn("s_vol_loaded = false", body)
        self.assertIn("s_vol_saved = -1", body)
        self.assertIn("ESP_ERR_NVS_NOT_FOUND", body)

    def test_final_commit_failure_keeps_local_reset_latch(self):
        body = function(WIFI, "app_wifi_finish_factory_reset")
        ordered(self, body, "s_reset_in_progress = true", "esp_wifi_stop()",
                "nvs_erase_key(h, NVS_KEY_RESET)", "nvs_commit(h)",
                "if (e != ESP_OK) {", "nvs_set_u8(h, NVS_KEY_RESET, 1)",
                "if (e == ESP_OK) s_reset_in_progress = false")
        self.assertIn("ESP_ERR_NVS_NOT_FOUND", body)

    def test_absent_volume_namespace_does_not_require_creating_it(self):
        body = function(VOICE, "qca_voice_reset_volume")
        ordered(self, body, "nvs_open(VOL_NVS_NS, NVS_READONLY", "ESP_ERR_NVS_NOT_FOUND",
                "e = ESP_OK", "else if (e == ESP_OK)",
                "nvs_open(VOL_NVS_NS, NVS_READWRITE")

    def test_confirmation_is_cancel_by_default_and_needs_long_ok(self):
        body = function(MAIN, "settings_menu")
        self.assertIn("int selected = 3", body)
        self.assertIn("confirm_factory_reset()", body)
        body = function(MAIN, "confirm_factory_reset")
        self.assertIn("bool confirm = false", body)
        self.assertIn("!confirm && (is_ok(&k) || is_ok_long(&k))", body)
        ordered(self, body, "confirm && is_ok_long(&k)", "app_wifi_request_factory_reset()")
        self.assertIn("confirm = d > 0", body)

    def test_settings_entry_and_clear_scope_are_visible(self):
        self.assertIn("长按设置", function(MAIN, "stage_pick"))
        self.assertIn("进入设备设置", function(UI, "app_ui_show_provision"))
        body = function(UI, "app_ui_show_reset_confirm")
        for text in ("删除 Wi-Fi 和 PAT", "恢复默认音量", "不删除云端会话", "重启后重新配置",
                     "取消，保留设置", "确认清除并重启"):
            self.assertIn(text, body)
        self.assertIn("bsp_lvgl_lock", body)
        self.assertIn("bsp_lvgl_unlock", body)
        self.assertIn("settings_menu(true)", function(MAIN, "run_provision"))
        self.assertEqual(function(MAIN, "run_provision").count("app_portal_start()"), 1)


if __name__ == "__main__":
    unittest.main()
