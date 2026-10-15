<p align="right">
  <a href="coding-conventions.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Coding Conventions

- Write C with four-space indentation and K&R braces, following neighboring files. Use `snake_case`, `BSP_*` public constants, `s_` file-local state, `bsp_` public BSP APIs, and `demo_<feature>_<action>` demo entry points. Prefer `static` for internal symbols.
- Keep baseline-demo UI text and default documentation in English. Application UI may use Chinese when its font support is implemented and verified. Explanatory source comments may use Chinese while retaining established English technical terms.
- Before adding Chinese UI text, complete the [font checklist](#chinese-fonts-and-missing-glyphs); changing strings to UTF-8 alone does not add missing glyphs.
- Put reusable hardware behavior in `components/bsp`; keep menus, animations, product interaction, and validation pages in `main`.
- Derivative applications must implement a redesigned UI and must not reuse the current demo test screens or `ui_pixel` visual shell. Renaming or recoloring the demo is not a redesign; follow the [mandatory UI redesign rule](../ai-guide.md#mandatory-ui-redesign-for-derivative-applications). Preserving the existing theme applies only to maintenance of the baseline hardware-test demo itself, not to application development.
- Show the battery level in the top-right corner of a user interface by default, unless the developer specifies a different placement or explicitly does not want it. Read it from `bsp_battery_soc()` (and `bsp_battery_mv()` where useful); render it as a small battery indicator or percentage in the top-right area of the screen, and degrade gracefully when it reads `-1` (unavailable). Avoid overlapping the application's own content. When maintaining the baseline theme, also avoid its cloud decoration (`add_cloud`, around `x≈188, y≈8`).
- Document non-trivial functions, state, ownership, blocking behavior, task context, initialization order, failure values, register choices, timing, synchronization, and hardware-specific constants. Explain why, not merely what.
- Add or update tests with code changes. If automation is not practical, record the test gap and exact manual validation path.
- If adding a cache, define expiration and cleanup unless durable retention is explicitly justified.
- The ESP32-C3 has no PSRAM. Review internal RAM and largest-contiguous-block impact before increasing LVGL buffers, audio allocations, network state, or task stacks.
- **Watch power consumption.** This is a wearable powered by a small battery; keep it efficient. Avoid keeping the screen lit for long periods: dim or turn off the backlight, and return to a low-power state (light/deep sleep) whenever the screen is idle, so the device is not left displaying a bright screen while doing nothing. See the guidance on sleep in [`../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md).

## Chinese fonts and missing glyphs

For configuration, converter commands, CMake integration, safe fallback code, and glyph-check examples, follow the [detailed LVGL Chinese text guide](lvgl-chinese-fonts.md).

The baseline enables Montserrat 14 and 20 in [`sdkconfig.defaults`](../../../sdkconfig.defaults); neither contains Chinese glyphs. A correct UTF-8 string can therefore appear as boxes or blank text. Serial-console output, an installed computer font, and a successful firmware build do not prove that LVGL can render that text.

### Before implementing or changing Chinese UI

1. **Define coverage.** Collect all displayed text: titles, buttons, hints, empty/error states, units, punctuation, and dynamic content. Include Latin letters, digits, full-width punctuation, and any `LV_SYMBOL_*` icons in the chosen font or fallback chain. Regenerate each affected size/subset when text changes. A built-in CJK subset or a subset extracted from fixed UI strings does not guarantee arbitrary Chinese names, Wi-Fi SSIDs, or server/user input; define the supported character set and an explicit unsupported-character policy.
2. **Generate reproducible assets.** Select a licensed font whose source actually contains the required glyphs. Prefer a subset at the sizes needed by the application instead of embedding an entire font family. Keep reusable sources/generated files under `assets/fonts/` and record the license, converter version, command, character inventory, size, and bit depth in the [assets documentation](../../../assets/README.md). Use output compatible with the LVGL version pinned in [`dependencies.lock`](../../../dependencies.lock) (currently 9.5.0). See the [official font converter](https://github.com/lvgl/lv_font_conv) for generation options; this baseline does not bundle an application-specific Chinese font.
3. **Compile and select the font.** Add the generated `.c` file to the consuming component's CMake `SRCS`, declare its exported symbol with `LV_FONT_DECLARE`, and assign it to the actual text widget/part. Copying a TTF/OTF or `.c` into `assets/` does not automatically compile, load, or select it. Check the generated `sdkconfig` has `CONFIG_LV_TXT_ENC_UTF8=y`; changing defaults alone does not necessarily update an existing configuration (see [build and test](build-and-test.md)).
4. **Check style overrides and fallback.** Changing the default font or a screen's inherited font does not override explicit child fonts. [`main/ui_pixel.c`](../../../main/ui_pixel.c) sets a local font in `ui_pixel_label()` and explicitly uses Montserrat 20 for its title; menu/demo labels also select fonts directly. Audit titles, labels, widget parts, and focused/pressed/disabled states. For mixed text, use a font containing all required glyphs or a deliberate fallback chain. Do not cast away `const` to modify Flash-resident font descriptors, create fallback cycles, or free a font while widgets still use it.
5. **Budget resources.** Inspect Flash/static RAM after generation and free heap/largest free block at runtime, including when networking is active. Constant bitmap font data can reside in Flash; runtime font loading, decoding, and caches may need RAM. Do not blindly enlarge the LVGL heap or display buffers to treat missing glyphs; more memory cannot supply glyphs absent from the font.

For example, after generating and linking `app_font_20` with the required glyphs, select it explicitly. This symbol is illustrative, not a font supplied by this repository. Run object operations in the LVGL task or while holding `bsp_lvgl_lock()`:

```c
LV_FONT_DECLARE(app_font_20);

/* Inside the application's UI creation function. */
lv_obj_t *label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &app_font_20, LV_PART_MAIN | LV_STATE_DEFAULT);
lv_label_set_text(label, "\u4E2D\u6587 ABC 123");
```

### Troubleshooting order

| Symptom | Check first |
| --- | --- |
| Latin text works; Chinese is blank or boxed | Actual widget font and its fallback coverage, UTF-8 input, and `CONFIG_LV_TXT_ENC_UTF8`; disabling `CONFIG_LV_USE_FONT_PLACEHOLDER` only hides the missing-glyph indicator, not the defect |
| Only some characters or new text fail | Missing subset entries, punctuation, each font size, and dynamic text outside the supported set; regenerate and relink the affected assets |
| Body text works but titles/buttons or selected states fail | Explicit local fonts, theme/state overrides, and the font on the widget part that draws the text |
| Glyph coverage is correct but text is invisible or cut off | Text color/opacity, hidden flags, label size, line height, parent clipping, and draw/allocation errors; do not assume every blank label is a font defect |

### Required acceptance

- Check every required printable Unicode code point against each font/fallback chain actually used. With the pinned LVGL API, `lv_font_get_glyph_dsc()` must succeed and `is_placeholder` must be false; a placeholder is not coverage. Decode UTF-8 into code points rather than testing individual bytes. Record uncovered code points and add a repeatable coverage check alongside the application's font-generation pipeline.
- On the device, verify all pages and error/empty states, Chinese/Latin/digit/punctuation/icon mixtures, different sizes and interaction states, long text, and dynamic content both inside and outside the declared supported set. Check for boxes, blanks, clipping, and fallback baseline/line-height mismatches; do not accept one successful sample label as whole-application validation.
- Report the tested text/character set, font sizes, and resource changes. The baseline's host tests do not validate a derivative application's font coverage or screen output. Without a physical-device check, report `Device tests: NOT RUN` and explicitly list Chinese rendering under `Unverified`.

Font selection and fallback semantics: [LVGL 9.5 font overview](https://docs.lvgl.io/9.5/main-modules/fonts/overview.html). Missing-glyph handling: [LVGL 9.5 implementation](https://github.com/lvgl/lvgl/blob/v9.5.0/src/font/lv_font.c).
