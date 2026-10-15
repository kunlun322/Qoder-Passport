**English** · [简体中文](qoder-passport-stack.zh_CN.md)

# Qoder-Passport hardware and software stack

The authoritative component, version, and deployment facts for building and
running Qoder-Passport v0.1.0. Where this document and a source file disagree,
the source file wins: [sdkconfig.defaults](/sdkconfig.defaults),
[partitions.csv](/partitions.csv), [dependencies.lock](/dependencies.lock),
[main/CMakeLists.txt](/main/CMakeLists.txt), and
[components/bsp/include/bsp_pins.h](/components/bsp/include/bsp_pins.h).

## 1. Hardware

Product: FoloToy AI Passport wearable, 60 x 95 x 8.5 mm, 50 g, transparent
enclosure. Specifications:
[specifications.md](/docs/hardware-design/specifications.md).

| Subsystem | Part | Bus / interface | Notes |
| --- | --- | --- | --- |
| MCU | ESP32-C3 | — | 8 MB Flash, **no PSRAM**, 2.4 GHz Wi-Fi b/g/n, BLE 5 |
| Display panel | ST7789P3, 240 x 320, RGB565 | SPI2, 80 MHz, mode 0 | No MISO: the panel cannot be read back |
| Backlight | LCD LED | GPIO21, LEDC 5 kHz, 10-bit | Conflicts with the UART0 default TX |
| Buttons | UP / DOWN / OK | GPIO0, ADC1_CH0 | Resistor ladder, external 10 kΩ pull-up to 3.3 V |
| Audio codec | ES8311 | I2S0 full duplex + I2C0 | MCU is I2S master, codec is slave |
| Microphone / speaker | on-board | via ES8311 | mono mic requires `no_dac_ref=true` |
| Fuel gauge | CW2017 | I2C0, 7-bit `0x63` | 100 kHz; 500 mAh cell profile |
| Battery | 500 mAh Li-ion | — | vendor profile matched at init |
| Console | USB Serial/JTAG | GPIO18 / GPIO19 | **115200 baud**, not UART0 |
| Charging / power | USB Type-C 2.0, 5 V | — | separate hardware power button |
| NFC | passive NTAG213 | — | not used by this application |

### Pin map

| GPIO | Function | Notes |
| ---: | --- | --- |
| 0 | three-button ADC node | ADC1_CH0, external 10 kΩ pull-up, boot strapping pin |
| 1 | LCD CS | ST7789P3 chip select |
| 2 | I2S DOUT | MCU to ES8311 |
| 3 | I2S WS | MCU is master |
| 4 | I2S DIN | ES8311 to MCU |
| 5 | I2S BCLK | shared TX/RX |
| 6 | I2S MCLK | required by the codec, 256 x fs |
| 7 | I2C SCL | open drain, shared by ES8311 and CW2017 |
| 8 | LCD SCLK | SPI2, 80 MHz, mode 0 |
| 9 | LCD MOSI | no MISO |
| 10 | I2C SDA | internal pull-up enabled |
| 18 / 19 | USB Serial/JTAG | reserved for the console |
| 20 | LCD DC | command / data select |
| 21 | backlight PWM | LEDC output |

LCD reset and amplifier enable are both `-1` in the BSP: the panel uses a
software reset and the amplifier is treated as always enabled. A GPIO absent
from this table is not automatically free.

### Button electrical detail

The ladder pulls GPIO0 to ground through 0 Ω (UP), 1 kΩ (DOWN), and 2.2 kΩ (OK),
against an external 10 kΩ pull-up to 3.3 V. Nominal levels and acceptance
windows, with `ADC_ATTEN_DB_12`:

| State | Nominal | Window (mV) |
| --- | ---: | --- |
| UP pressed | ~0 mV | `[0, 150)` |
| DOWN pressed | ~300 mV | `[150, 447)` |
| OK pressed | ~595 mV | `[447, 1900)` |
| released | ~3300 mV | `>= BSP_BTN_MV_RELEASED_MIN` |

Never use the internal pull-up (about 45 kΩ and inaccurate). Press timing is
passed explicitly: `BSP_BTN_SHORT_PRESS_MS = 180`, `BSP_BTN_LONG_PRESS_MS = 500`.

### Two constraints that shape the firmware

1. **No PSRAM.** The ~400 KB SRAM holds the Wi-Fi and lwIP buffers, the mbedTLS
   record buffers, the LVGL pool, the audio pipeline, and the heap all at once.
   Every memory-related `sdkconfig` value below exists because of a measured
   allocation failure, not preference.
2. **GPIO21 is the backlight.** The console must stay on USB Serial/JTAG; any
   change to the log interface requires re-checking pin usage.

## 2. Toolchain

| Item | Required version | Notes |
| --- | --- | --- |
| ESP-IDF | **exactly v5.5.3** | tag commit `b31fcc7a314a44ad992b58f589f7d1d8a4fadff6`. Stop if `idf.py --version` reports anything else |
| Target | `esp32c3` | `idf.py set-target esp32c3` on a fresh checkout |
| Cross compiler | the `riscv32-esp-elf` toolchain installed by ESP-IDF | do not substitute a system RISC-V gcc |
| Python | whatever ESP-IDF v5.5.3's installer provisions | the v0.1.0 artifacts were produced with CPython 3.13.13 in the installer environment |
| esptool | 4.12.0 | invoked as `python -m esptool` from the IDF Python environment |
| CMake / Ninja / ccache | provided by the IDF installer | ccache is off by default in 5.5.3; enable with `IDF_CCACHE_ENABLE=1` |

Bootstrap, host OS packages, mirrors, and repair steps:
[environment-setup.md](/docs/development/engineering/environment-setup.md).

## 3. Managed components

Pinned by [dependencies.lock](/dependencies.lock). The first build downloads
them into `managed_components/`, which is git-ignored and must never be edited.

| Component | Locked version | Constraint source | Purpose |
| --- | --- | --- | --- |
| `lvgl/lvgl` | 9.6.0~1 | `esp_lvgl_port` (>=8,<10) | UI rendering |
| `espressif/esp_lvgl_port` | 2.9.0 | BSP (>=5.2) | LVGL task, display and touch plumbing |
| `espressif/esp_codec_dev` | 1.6.2 | BSP (>=4.0) | ES8311 audio |
| `espressif/esp_websocket_client` | 1.8.0 | [main/idf_component.yml](/main/idf_component.yml) (`^1.5.0`) | QCA realtime voice channel |
| `espressif/button` | 4.2.0 | BSP (>=4.0) | IoT Button, ADC ladder |
| `espressif/cmake_utilities` | 1.1.1 | transitive | component build helpers |
| `components/bsp` | vendored in-repo | — | display, audio, battery, buttons, I2C, pins |

The only manifest dependency this project added is `esp_websocket_client`.
A normal build must not leave an unexplained diff in `dependencies.lock`;
regenerate it with ESP-IDF 5.5.3 after any manifest change.

### Component registry access

The build needs network access to the ESP Component Registry. In mainland China
set the file-storage mirror for the current terminal:

```bash
export IDF_COMPONENT_STORAGE_URL="https://components-file.espressif.cn"
```

This changes only where component archives are downloaded; the versions still
come from the tracked lock file.

## 4. Font toolchain (only needed to regenerate fonts)

The generated fonts are **committed**, so a plain checkout builds without any of
this. Regeneration uses [assets/fonts/build_fonts.py](/assets/fonts/build_fonts.py):

| Item | Version | Purpose |
| --- | --- | --- |
| `fonttools` | 4.60.1 | variable-font instancing at wght=500 and subsetting |
| Node.js + `lv_font_conv` | 1.5.3 (`npx -y lv_font_conv@1.5.3`) | emits LVGL C sources, 4 bpp, uncompressed |
| Noto Sans SC variable font | SIL OFL 1.1 | glyph source |

Outputs: `assets/fonts/app_font_cjk_16.c` (printable ASCII plus every GB2312
code point present in the source font, about 6.2 MB) and
`assets/fonts/app_font_status_28.c` (only the fixed status words plus digits).
Each has a sibling `.codepoints.txt`. Coverage is checked by
`tests/test_font_coverage.py`, which **is** part of the gate. Details:
[lvgl-chinese-fonts.md](/docs/development/engineering/lvgl-chinese-fonts.md).

## 5. Host test toolchain

| Item | Needed by | Notes |
| --- | --- | --- |
| Python 3 | the Python contract suites, `tools/*.py` | no third-party packages required |
| A host C compiler (`cc`) | the C logic suites | compiled with `-std=c11 -Wall -Wextra -Werror`; on Ubuntu/Debian install `build-essential` |
| `curl`, `tar`, a SHA-256 tool | `tools/validate.sh --static` | used to fetch and unpack a checksum-pinned `actionlint` |
| `actionlint` | workflow linting | downloaded by `tools/install-actionlint.sh` if absent |
| git | `tools/check_repo.py` | it enumerates files with `git ls-files --cached --others --exclude-standard` |
| Node.js | `tests/test_portal_page.mjs` | **not registered in the gate**; run manually |

Without a host C compiler, `--static` and the full gate cannot run. In that case
run `--firmware` plus the Python suites individually, and report the C execution
as NOT RUN rather than PASS.

## 6. Build configuration

All values live in [sdkconfig.defaults](/sdkconfig.defaults); the gate builds
from it in an isolated temporary directory and never touches a developer's root
`sdkconfig`.

### Flash, partitions, console

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_IDF_TARGET` | `esp32c3` | — |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | `y` | product baseline |
| `CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` | `n` | fixed 8 MB image header so `merge-bin` is deterministic |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | uses [partitions.csv](/partitions.csv) |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `y` | GPIO21 is the backlight, so UART0 is unusable |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | `y` | no secondary console |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick for the audio and retry timers |

### Partition layout

| Partition | Type / subtype | Offset | Size |
| --- | --- | ---: | ---: |
| `nvs` | data / nvs | `0x9000` | `0x6000` (24 KB) |
| `phy_init` | data / phy | `0xf000` | `0x1000` (4 KB) |
| `factory` | app / factory | `0x10000` | `0x7f0000` |

No OTA slot. The v0.1.0 application is about 2.60 MB, so roughly 69% of the
`factory` partition is free. Policy:
[firmware-layout.md](/docs/development/engineering/firmware-layout.md).

### UI

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_LV_COLOR_DEPTH_16` | `y` | RGB565 panel |
| `CONFIG_LV_DEF_REFR_PERIOD` | `20` | up to 50 FPS |
| `CONFIG_LV_MEM_SIZE` | `32768` | LVGL 9.3+ reads bytes only; the legacy `LV_MEM_SIZE_KILOBYTES` is silently overridden by a 64 KB default, which once starved a BLE controller |
| `CONFIG_LV_TXT_ENC_UTF8` | `y` | Chinese UI text |
| `CONFIG_LV_USE_FONT_PLACEHOLDER` | `y` | missing glyphs render visibly instead of blank |
| `CONFIG_LV_USE_QRCODE` | `y` | provisioning QR code |

The LVGL flush buffer is one 240 x 20 RGB565 buffer, about 9.6 KB, and the final
flush is masked to a 30 px global corner radius.

### Bluetooth

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_BT_ENABLED` | `n` | provisioning moved to a WPA2 web portal; the radio budget goes to Wi-Fi and TLS |

The upstream BluFi reference lives on the `demo/blufi-provisioning` branch and is
documented in
[wifi-provisioning.md](/docs/development/engineering/wifi-provisioning.md).

### TLS memory strategy (no PSRAM)

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | `n` | dynamic buffers allocate a receive block per record; during a voice session the heap is fragmented by Wi-Fi/lwIP and a full-length 16 KB server record needed about 16.7 KB contiguous, which repeatedly failed with `-0x7F00` |
| `CONFIG_MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` | `y` | enables the RFC 6066 negotiation |
| `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` | `y` | allows different in/out lengths |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | `8192` | enough to reassemble the ~5.6 KB certificate-chain handshake message |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | `4096` | — |
| `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | `n` | keeps only the peer certificate digest, saving ~4 KB of heap |

`main/tls_mfl.c` wraps `mbedtls_ssl_config_defaults` with
`-Wl,--wrap=` and `-Wl,-u,` so the linker keeps the wrapper, requesting a 4 KB
maximum fragment length from the server. `api.qoder.com` honours it.

Dynamic buffering is deliberately **not** combined with max-fragment-length:
mbedTLS reassembles handshake messages using the full receive-buffer length while
dynamic buffers allocate only for the current record, and a fragmented
certificate then overruns the heap — that combination crashed at boot on device.

Certificate validation and cipher suites are unchanged. Nothing here lowers TLS
security; it only shrinks buffers.

### TCP / lwIP

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | `8192` | a 16 KB backlog window crowded out contiguous TLS record memory during 24 kHz base64 downlink |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | `11520` | — |
| `CONFIG_LWIP_TCP_RECVMBOX_SIZE` | `16` | — |
| `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` | `16` | matches the reference C3 tuning |
| `CONFIG_LWIP_DHCPS_ADD_DNS` | `y` | the portal DHCP server hands out the DNS address so captive redirection works |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | `1024` | room for the portal CSRF header |

### Wi-Fi buffers

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_ESP_WIFI_IRAM_OPT` | `n` | on the C3, IRAM and DRAM share one SRAM; disabling returns ~25 KB of code to Flash and that RAM to the heap. Realtime voice is ~64 KB/s, far below the throughput ceiling without the optimisation |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | `n` | same |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `8` | down from 10, ~3 KB saved |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | `10` | with the defaults (32 each) a barge-in momentarily pushed Wi-Fi/lwIP to ~25 KB and minimum free heap fell to 772 bytes. 10 full-length frames is ~20 KB; an 8 KB TCP window keeps about 6 segments in flight plus 4 out-of-order, and the Kconfig floor is the 8 static buffers |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | `16` | — |

Packets above the buffer ceiling are recovered by TCP retransmission; this does
not corrupt data or TLS.

## 7. Application architecture

| Module | Responsibility |
| --- | --- |
| `main/main.c` | the application task: boot decision, template/identity selection, session lifecycle, button dispatch |
| `main/app_ui.c` | all LVGL screens. Every entry takes the LVGL lock internally |
| `main/app_list.h` | header-only cursor/window logic, host-testable |
| `main/app_fonts.c` | font selection |
| `main/app_wifi.c` | STA lifecycle, credential mirror in NVS, channel hint, backoff retry |
| `main/app_portal.c` | softAP + DNS + HTTP portal, CSRF, async config worker |
| `main/portal.html` | the portal page, embedded with `EMBED_TXTFILES` |
| `main/portal_form.c` | form parsing and validation |
| `main/qca_pat.c` | PAT load/store in NVS namespace `qca_app` |
| `main/qca_api.c` | HTTPS calls to the QCA REST API |
| `main/qca_list_parse.c` | template and identity list parsing |
| `main/qca_voice.c` | the realtime WebSocket voice session, playback and capture tasks |
| `main/voice_logic.c` | pure voice state machine, host-testable |
| `main/audio_dsp.c` | gain and clipping, host-testable |
| `main/b64_stream.c` | streaming base64 decode, host-testable |
| `main/qjson.c` | streaming JSON parsing, host-testable |
| `main/utf8_util.c` | UTF-8 helpers, host-testable |
| `main/tls_mfl.c` | the mbedTLS max-fragment-length wrapper |
| `main/key_guard.h` | button debounce/guard logic |

Threading model: `app_task` runs the whole flow serially and is the only place
that performs blocking HTTPS. Button callbacks only enqueue events. LVGL is
driven by its own task, and the voice screen polls `qca_voice_get_view()` from an
LVGL timer every 60 ms — the voice task never touches LVGL.

### Cloud endpoints

| Purpose | Value |
| --- | --- |
| Host | `api.qoder.com` |
| REST base | `https://api.qoder.com/api/v1` |
| Realtime voice | `wss://api.qoder.com/api/v1/forward/realtime` |
| Protocol version | `voice.realtime.v1` |
| Conversation title | `Qoder Passport` |

Defined in [main/qca_config.h](/main/qca_config.h). The device needs outbound
TCP 443 to that host with full TLS certificate validation.

### Credential storage

Namespace `qca_app`, keys `pat`, `wifi_ssid`, `wifi_pass`, `wifi_ch`. The Wi-Fi
credentials are mirrored here in addition to the driver's own
`WIFI_STORAGE_FLASH` persistence, because the driver copy was observed to vanish
across a reboot on this device while the `qca_app` copy survived. Restore only
fills the driver when the driver is empty. No log line ever prints an SSID,
password, or PAT.

## 8. Deployment requirements

### To build

| Requirement | Detail |
| --- | --- |
| Disk | roughly 3 GB for ESP-IDF and its toolchains, ~200 MB for `managed_components/`, ~900 MB for a build tree |
| Network | access to the ESP Component Registry, or the China mirror above |
| Host OS | Linux (Ubuntu/Debian, Fedora, Arch), macOS (Xcode CLT + Homebrew), native Windows (Espressif ESP-IDF Tools Installer), or WSL2 |
| Packages (Ubuntu/Debian) | `git wget curl flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 build-essential` |
| Packages (macOS) | `brew install cmake ninja ccache dfu-util libusb python` plus Xcode Command Line Tools |
| Packages (Windows) | the Espressif installer provides the toolchain; run commands in the installer-created ESP-IDF terminal |
| Docker alternative | `docker run --rm -v "$PWD:/project" -w /project -e IDF_GIT_SAFE_DIR=/project espressif/idf:v5.5.3 ./tools/validate.sh --firmware` — build only, no USB flashing |

WSL2 can build but flashing and monitoring need USB forwarding into the WSL
instance; otherwise flash from a native Windows ESP-IDF terminal.

### To flash and observe

| Requirement | Detail |
| --- | --- |
| USB port and data cable | the device enumerates as `VID:PID = 303A:1001` |
| `esptool` | 4.12.0 via `python -m esptool` from the IDF Python environment |
| Linux serial permission | membership in `dialout` (or `uucp`), then re-login |
| Console baud | **115200** on USB Serial/JTAG |

Enumerate ports read-only before writing anything, and confirm the target device
rather than assuming the first port. Flashing requires explicit user consent;
detecting a device is not consent.

### To operate

| Requirement | Detail |
| --- | --- |
| Wi-Fi | 2.4 GHz 802.11 b/g/n network |
| Cloud | outbound HTTPS and WSS to `api.qoder.com` on port 443 |
| Account | a Qoder Personal Access Token with permission to list templates and identities and to open realtime sessions |

## 9. Validation gate

```bash
source <path-to-esp-idf-v5.5.3>/export.sh
./tools/validate.sh --static      # repo conventions + host C and Python tests
./tools/validate.sh --firmware    # ESP-IDF build, merge-bin, layout validation, archive
./tools/validate.sh               # both
```

`--firmware` produces `build/FoloToy-AI-Passport-full.bin` and a bundle under
`build/firmware/<full-bin-sha256>/` with the ELF, MAP, application image,
`bootloader/bootloader.bin`, `partition_table/partition-table.bin`,
`manifest.json`, and `flash_args`. Verify a bundle without an activated IDF:

```bash
python3 tools/archive_firmware.py verify build/firmware/<full-bin-sha256>
```

The gate validates the partition-table MD5, partition bounds, unique labels,
non-overlap, and that the application offset matches a sufficiently large app
partition. A failed run does not validate a stale file left in `build/`.
CI runs the same script:
[.github/workflows/firmware-checks.yml](/.github/workflows/firmware-checks.yml)
and [.github/workflows/static-checks.yml](/.github/workflows/static-checks.yml).

Details: [build-and-test.md](/docs/development/engineering/build-and-test.md).
