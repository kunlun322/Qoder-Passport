#!/usr/bin/env python3
"""Static contracts for the terminal deep-sleep shutdown path."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def initializer(source: str, symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{", source)
    if not match:
        raise AssertionError(f"initializer not found: {symbol}")
    end = source.find("};", match.end())
    if end < 0:
        raise AssertionError(f"initializer is unterminated: {symbol}")
    return source[match.end():end]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


def register_pairs(block: str) -> list[tuple[int, int]]:
    return [
        (int(reg, 16), int(value, 16))
        for reg, value in re.findall(r"\{\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*\}", block)
    ]


class DeepSleepContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audio = read("components/bsp/src/bsp_audio.c")
        cls.battery = read("components/bsp/src/bsp_battery.c")
        cls.display = read("components/bsp/src/bsp_display.c")
        cls.i2c = read("components/bsp/src/bsp_i2c.c")
        cls.demo = read("main/demo_low_power.c")

    def test_es8311_force_sleep_sequence_is_complete_and_ordered(self) -> None:
        expected = [
            (0x32, 0x00), (0x17, 0x00), (0x0E, 0xFF), (0x12, 0x02),
            (0x14, 0x00), (0x0D, 0xFA), (0x15, 0x00), (0x02, 0x10),
            (0x00, 0x00), (0x00, 0x1F), (0x01, 0x30), (0x01, 0x00),
            (0x45, 0x01), (0x0D, 0xFC), (0x02, 0x00),
        ]
        actual = register_pairs(initializer(self.audio, "s_es8311_sleep_sequence"))
        self.assertEqual(actual, expected)

    def test_es8311_critical_registers_are_read_back(self) -> None:
        # The C host test exhausts all readback values against the actual policy.
        # This integration contract makes sure the driver uses it, and that an
        # I2C read failure cannot pass even when the output byte matches.
        body = function_body(self.audio, "es8311_force_sleep_once")
        self.assertIn("s_ctrl->write_reg", body)
        self.assertIn("s_ctrl->read_reg", body)
        self.assertIn("i < bsp_es8311_sleep_check_count", body)
        self.assertIn("&bsp_es8311_sleep_checks[i]", body)
        self.assertRegex(
            body,
            r"read_result == ESP_CODEC_DEV_OK\s*&&\s*"
            r"bsp_es8311_sleep_check_matches\(item, actual\)",
        )

    def test_es8311_force_sleep_retries_once_after_five_ms(self) -> None:
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_ATTEMPTS\s+2\b")
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_RETRY_MS\s+5\b")
        body = function_body(self.audio, "es8311_force_sleep")
        self.assertIn("attempt <= ES8311_SLEEP_ATTEMPTS", body)
        self.assertIn("pdMS_TO_TICKS(ES8311_SLEEP_RETRY_MS)", body)

    def test_audio_suspend_does_not_require_a_silent_open(self) -> None:
        body = function_body(self.audio, "bsp_audio_sleep")
        self.assertIn("es8311_force_sleep()", body)
        self.assertIn("audio_disable_i2s_channels()", body)
        self.assertNotIn("bsp_audio_set_format", body)

    def test_i2s_pins_are_released_only_by_deep_sleep_api(self) -> None:
        body = function_body(self.audio, "bsp_audio_prepare_deep_sleep")
        for pin in ("BSP_I2S_MCLK", "BSP_I2S_BCLK", "BSP_I2S_WS",
                    "BSP_I2S_DOUT", "BSP_I2S_DIN"):
            self.assertIn(pin, body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_cw2017_sleep_is_verified_and_retried(self) -> None:
        body = function_body(self.battery, "bsp_battery_sleep")
        self.assertIn("attempt <= 2", body)
        self.assertIn("cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP)", body)
        self.assertIn("pdMS_TO_TICKS(5)", body)
        self.assertIn("cw_read(CW_REG_CONFIG, &actual, 1)", body)
        self.assertIn("actual == CW_CONFIG_SLEEP", body)

    def test_shared_i2c_is_released_after_device_transactions(self) -> None:
        body = function_body(self.i2c, "bsp_i2c_prepare_deep_sleep")
        self.assertIn("BSP_I2C_SDA", body)
        self.assertIn("BSP_I2C_SCL", body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_lcd_safe_levels_and_holds_are_configured(self) -> None:
        pins = initializer(self.display, "s_deep_sleep_pins")
        levels = initializer(self.display, "s_deep_sleep_levels")
        self.assertRegex(pins, r"BSP_LCD_CS.*BSP_LCD_SCLK.*BSP_LCD_MOSI.*BSP_LCD_DC.*BSP_LCD_BL")
        self.assertRegex(levels, r"1\s*,\s*0\s*,\s*0\s*,\s*0\s*,\s*0")
        body = function_body(self.display, "bsp_display_prepare_deep_sleep")
        self.assertIn("esp_lcd_panel_disp_on_off(s_panel, false)", body)
        self.assertIn("esp_lcd_panel_disp_sleep(s_panel, true)", body)
        self.assertIn("ledc_stop", body)
        self.assertIn("gpio_hold_en", body)
        self.assertIn("gpio_deep_sleep_hold_en", body)

    def test_lcd_holds_are_released_before_spi_initialization(self) -> None:
        release = function_body(self.display, "display_release_deep_sleep_holds")
        self.assertIn("gpio_deep_sleep_hold_dis", release)
        self.assertIn("gpio_hold_dis", release)
        init = function_body(self.display, "bsp_display_init")
        self.assertLess(init.index("display_release_deep_sleep_holds()"),
                        init.index("spi_bus_initialize"))

    def test_terminal_shutdown_order_precedes_deep_sleep(self) -> None:
        body = function_body(self.demo, "sleep_task")
        calls = [
            "bsp_battery_sleep()",
            "bsp_audio_sleep()",
            "bsp_audio_prepare_deep_sleep()",
            "bsp_i2c_prepare_deep_sleep()",
            "bsp_display_prepare_deep_sleep()",
            "esp_deep_sleep_start()",
        ]
        positions = [body.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(body.index("bsp_lvgl_lock(1000)"),
                        body.index("bsp_display_prepare_deep_sleep()"))


if __name__ == "__main__":
    unittest.main()
