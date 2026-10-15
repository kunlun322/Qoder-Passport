<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

| File | Size, weight, charset | Use and source |
| --- | --- | --- |
| [`fonts/app_font_cjk_16.c`](fonts/app_font_cjk_16.c) | 16 px, 4 bpp, wght 500; printable ASCII + all GB2312 code points present in the source (code-point list: [`app_font_cjk_16.codepoints.txt`](fonts/app_font_cjk_16.codepoints.txt)) | Body text of the Qoder Passport app; falls back to Montserrat 14 for `LV_SYMBOL_*`. |
| [`fonts/app_font_status_28.c`](fonts/app_font_status_28.c) | 28 px, 4 bpp, wght 500; only the characters in [`status_28_chars.txt`](fonts/status_28_chars.txt) | Large voice-state words (listening, paused, and so on). |

Source: Noto Sans SC variable font (`NotoSansSC-VF.ttf`), © Adobe, SIL Open Font License 1.1 (Reserved Font Name "Source"); instanced at wght 500 and subset before conversion with lv_font_conv 1.5.3. Regenerate outside the gate with `python assets/fonts/build_fonts.py <NotoSansSC-VF.ttf> <work_dir>` (needs fontTools and Node.js). `tests/test_font_coverage.py` checks that every on-screen string of the app is covered.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
