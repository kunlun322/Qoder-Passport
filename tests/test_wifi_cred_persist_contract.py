#!/usr/bin/env python3
"""Source-contract regression for the app-NVS Wi-Fi credential mirror.

Why this exists: on this device the IDF driver's WIFI_STORAGE_FLASH persistence
was observed to drop the STA config across reboot. boot24.log captured the failed
state after a *successful* portal save: the save path returned ESP_OK for
qca_pat_use/set_storage/set_config (the only way the screen reaches PORTAL_DONE),
yet the next boot logged `has_config get=ESP_OK ssid_len=0` while `pat_present=1`.
So the app namespace (qca_app) PAT survived but the driver-owned Wi-Fi config did
not, and the device fell straight back into the portal.

The fix mirrors the Wi-Fi credentials into the same reliable qca_app namespace and
restores them into the driver at boot when the driver has none. These checks pin
that contract. Contracts only: no ESP-IDF, NVS, device, or network execution.

The suite also pins the connect strategy adopted after comparing against the
reference implementation (78/esp-wifi-connect 3.3.1, used by FoloToy's xiaozhi
passport build): scan all channels and pick the strongest same-SSID AP, let the
driver retry once, remember the AP's primary channel as a scan-start hint, and
keep the retry backoff out of the default event loop while making it long enough
to clear the Comeback Time that home APs send after an APSTA channel switch.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI = (ROOT / "main/app_wifi.c").read_text(encoding="utf-8")
PORTAL = (ROOT / "main/app_portal.c").read_text(encoding="utf-8")
MAIN = (ROOT / "main/main.c").read_text(encoding="utf-8")


def function(source, name):
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", source)
    assert match, f"Missing function: {name}"
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


class WifiCredMirrorContract(unittest.TestCase):
    def test_namespace_and_keys_reuse_the_pat_proven_one(self):
        # The mirror must live in qca_app (the namespace proven to survive reboot),
        # not a new one, and use short NVS-legal key names.
        self.assertIn('#define NVS_NS        "qca_app"', WIFI)
        self.assertIn('#define NVS_KEY_SSID  "wifi_ssid"', WIFI)
        self.assertIn('#define NVS_KEY_PASS  "wifi_pass"', WIFI)
        self.assertIn('#define NVS_KEY_CH    "wifi_ch"', WIFI)
        for key in ("wifi_ssid", "wifi_pass", "wifi_ch"):
            self.assertLessEqual(len(key), 15, "NVS key names must be <= 15 chars")

    def test_save_writes_both_fields_then_commits(self):
        body = function(WIFI, "app_wifi_save_creds")
        ordered(self, body, "nvs_open(NVS_NS, NVS_READWRITE",
                "nvs_set_str(h, NVS_KEY_SSID", "nvs_set_str(h, NVS_KEY_PASS",
                "nvs_set_u8(h, NVS_KEY_CH", "nvs_commit(h)")
        # Rejects an empty SSID instead of persisting a blank network.
        self.assertIn("if (sn == 0) return ESP_ERR_INVALID_ARG", body)
        # Scrubs the stack copies of SSID/password before returning.
        self.assertIn("memset(s, 0, sizeof(s))", body)
        self.assertIn("memset(p, 0, sizeof(p))", body)

    def test_restore_is_conditional_and_refills_the_driver(self):
        body = function(WIFI, "app_wifi_restore_creds")
        # Only fills when the driver is empty; never overwrites a working config.
        ordered(self, body, "if (app_wifi_has_config()) return false",
                "load_creds(", "esp_wifi_set_config(WIFI_IF_STA")
        self.assertIn("WIFI_STORAGE_FLASH", body)
        # Bail out (no phantom config) when the mirror is absent/blank.
        self.assertIn("if (e != ESP_OK || !ssid[0])", body)
        self.assertIn("memset(pass, 0, sizeof(pass))", body)

    def test_clear_is_idempotent_over_all_mirror_keys(self):
        body = function(WIFI, "app_wifi_clear_creds")
        ordered(self, body, "nvs_erase_key(h, NVS_KEY_SSID",
                "nvs_erase_key(h, NVS_KEY_PASS", "nvs_erase_key(h, NVS_KEY_CH",
                "nvs_commit(h)")
        # A missing key is success, so factory reset stays retry-safe.
        self.assertEqual(body.count("ESP_ERR_NVS_NOT_FOUND"), 3)
        # No stale channel hint survives a factory reset.
        self.assertIn("s_ch_hint = 0", body)

    def test_portal_saves_mirror_on_success_path_before_flash_config(self):
        body = function(PORTAL, "config_worker")
        # The mirror is written from the same `c` that connected (covers the
        # "reuse old password" case where s_pending.pass is empty), before the
        # best-effort IDF flash write, and only on the auth-success branch.
        ordered(self, body, "qca_pat_use(qca_pat(), true)",
                "app_wifi_save_creds((const char *)c.sta.ssid",
                "esp_wifi_set_storage(WIFI_STORAGE_FLASH)",
                "esp_wifi_set_config(WIFI_IF_STA, &c)", "PORTAL_DONE")

    def test_boot_restores_before_the_portal_decision(self):
        body = function(MAIN, "app_task")
        ordered(self, body, "qca_pat_load()", "app_wifi_restore_creds()",
                "app_wifi_has_config()", "run_provision()")

    def test_factory_reset_clears_the_mirror(self):
        body = function(WIFI, "app_wifi_finish_factory_reset")
        ordered(self, body, "esp_wifi_restore()", "app_wifi_clear_creds()",
                'qca_pat_use("", true)')

    def test_credentials_are_never_logged_as_strings(self):
        # Every ESP_LOG in the mirror code may only use %s for esp_err_to_name;
        # SSID/password buffers must never reach a format string. Lengths (%u) ok.
        for fn in ("app_wifi_save_creds", "app_wifi_restore_creds",
                   "app_wifi_clear_creds", "load_creds", "remember_channel"):
            body = function(WIFI, fn)
            for m in re.finditer(r"ESP_LOG\w+\([^;]*\);", body):
                stmt = m.group()
                if "%s" in stmt:
                    self.assertIn("esp_err_to_name", stmt,
                                  f"%s log without err_TO_NAME in {fn}: {stmt}")
        self.assertIn("ssid_len=%u", function(WIFI, "app_wifi_restore_creds"))

    # ---- connect strategy, aligned with 78/esp-wifi-connect 3.3.1 ----

    def test_connect_strategy_is_set_on_both_config_paths(self):
        # Portal first connect: the channel of a brand-new network is unknown, so
        # scan every channel, pick the strongest same-SSID AP (multi-AP/mesh homes),
        # and let the driver retry once before moving on to the next AP.
        portal = function(PORTAL, "config_worker")
        for setting in ("WIFI_ALL_CHANNEL_SCAN", "WIFI_CONNECT_AP_BY_SIGNAL",
                        "failure_retry_cnt = 1", "c.sta.channel = 0"):
            self.assertIn(setting, portal, f"config_worker missing {setting}")

        # Boot refill: always sort by signal, but scan breadth follows the hint.
        restore = function(WIFI, "app_wifi_restore_creds")
        self.assertIn("WIFI_CONNECT_AP_BY_SIGNAL", restore)
        # Slice from the branch itself so the explanatory comment above it (which
        # names failure_retry_cnt while explaining why it is not used) is excluded.
        branch = restore[restore.index("if (s_ch_hint) {"):]
        fast, sep, thorough = branch.partition("} else {")
        self.assertTrue(sep, "restore must branch on the channel hint")
        # Known channel -> fast scan starting there. This is where the boot speedup
        # comes from; ALL_CHANNEL_SCAN would walk all 13 channels on every boot.
        self.assertIn("c.sta.channel = s_ch_hint", fast)
        self.assertIn("WIFI_FAST_SCAN", fast)
        # IDF documents failure_retry_cnt as requiring WIFI_ALL_CHANNEL_SCAN, so it
        # must not be set on the fast-scan branch where the driver would ignore it.
        self.assertNotIn("failure_retry_cnt", fast)
        # Unknown channel -> thorough scan plus the driver-level retry.
        self.assertIn("c.sta.channel = 0", thorough)
        self.assertIn("WIFI_ALL_CHANNEL_SCAN", thorough)
        self.assertIn("failure_retry_cnt = 1", thorough)

    def test_channel_hint_is_learned_from_the_connected_event(self):
        # WIFI_EVENT_STA_CONNECTED already carries the AP primary channel, so no
        # extra esp_wifi_sta_get_ap_info() call is needed.
        self.assertIn("WIFI_EVENT_STA_CONNECTED, on_wifi_event", WIFI)
        handler = function(WIFI, "on_wifi_event")
        self.assertIn("WIFI_EVENT_STA_CONNECTED", handler)
        self.assertIn("remember_channel(c->channel)", handler)
        remember = function(WIFI, "remember_channel")
        # Skip channel 0 and no-change writes, so NVS is not worn on every reconnect.
        self.assertIn("if (ch == 0 || ch == s_ch_hint) return", remember)
        ordered(self, remember, "nvs_set_u8(h, NVS_KEY_CH", "nvs_commit(h)")

    def test_channel_hint_is_loaded_and_applied_as_scan_start(self):
        load = function(WIFI, "load_creds")
        # Optional key: pre-existing mirrors lack it, so a read failure is ignored
        # and stays 0 ("no preference") rather than failing the whole restore.
        self.assertIn("if (ch) *ch = 0", load)
        self.assertIn("(void)nvs_get_u8(h, NVS_KEY_CH, ch)", load)
        restore = function(WIFI, "app_wifi_restore_creds")
        self.assertIn("c.sta.channel = s_ch_hint", restore)
        # A network switch must not carry the previous AP's channel over.
        self.assertIn("c.sta.channel = 0", function(PORTAL, "config_worker"))

    def test_retry_backoff_never_blocks_the_event_loop(self):
        # The old code called vTaskDelay inside the default event-loop callback,
        # stalling every other event (softAP/DHCP included) for up to 1.5 s.
        handler = function(WIFI, "on_wifi_event")
        self.assertNotIn("vTaskDelay", handler)
        self.assertIn("esp_timer_start_once(s_retry_timer", handler)
        callback = function(WIFI, "retry_timer_cb")
        # The timer can outlive the caller's timeout, so re-check intent first.
        self.assertIn("if (s_want && !s_connected) esp_wifi_connect()", callback)
        init = function(WIFI, "app_wifi_init")
        ordered(self, init, "esp_timer_create(&ta, &s_retry_timer)",
                "WIFI_EVENT_STA_DISCONNECTED", "WIFI_EVENT_STA_CONNECTED")
        # Give-up path cancels the pending retry instead of leaving it dangling.
        connect = function(WIFI, "app_wifi_connect")
        ordered(self, connect, "s_want = false",
                "if (s_retry_timer) esp_timer_stop(s_retry_timer)",
                "esp_wifi_disconnect()")

    def test_backoff_clears_the_ap_comeback_window_and_fits_the_timeout(self):
        # Home APs answer the first association after an APSTA channel switch with
        # status=30 + Comeback Time (~1.1 s observed by the reference project), and
        # the driver's internal retries ignore it. So the floor must clear ~1.1 s,
        # and the cap must keep the worst-case total inside the 25 s portal budget.
        floor = int(re.search(r"#define RETRY_BACKOFF_MIN_MS (\d+)", WIFI).group(1))
        cap = int(re.search(r"#define RETRY_BACKOFF_MAX_MS (\d+)", WIFI).group(1))
        tries = int(re.search(r"#define CONNECT_RETRY_MAX (\d+)", WIFI).group(1))
        self.assertGreaterEqual(floor, 1200)
        self.assertGreaterEqual(cap, floor)
        total = sum(min(floor * i, cap) for i in range(1, tries + 1))
        self.assertLessEqual(total, 15000, f"worst-case backoff {total} ms is too long")
        self.assertIn("if (ms > RETRY_BACKOFF_MAX_MS) ms = RETRY_BACKOFF_MAX_MS",
                      function(WIFI, "on_wifi_event"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
