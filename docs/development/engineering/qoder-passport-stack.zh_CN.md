[English](qoder-passport-stack.md) · **简体中文**

# Qoder-Passport 硬件与软件栈

构建并运行 Qoder-Passport v0.1.0 所需的权威组件、版本与部署事实。本文与源文件冲突时
以源文件为准：[sdkconfig.defaults](/sdkconfig.defaults)、
[partitions.csv](/partitions.csv)、[dependencies.lock](/dependencies.lock)、
[main/CMakeLists.txt](/main/CMakeLists.txt) 与
[components/bsp/include/bsp_pins.h](/components/bsp/include/bsp_pins.h)。

## 1. 硬件

产品：FoloToy AI Passport 可穿戴设备，60 × 95 × 8.5 mm，50 g，透明外壳。
规格见 [specifications.zh_CN.md](/docs/hardware-design/specifications.zh_CN.md)。

| 子系统 | 型号 | 总线 / 接口 | 备注 |
| --- | --- | --- | --- |
| 主控 | ESP32-C3 | — | 8 MB Flash，**无 PSRAM**，2.4 GHz Wi-Fi b/g/n，BLE 5 |
| 屏幕 | ST7789P3，240 × 320，RGB565 | SPI2，80 MHz，mode 0 | 无 MISO，屏幕不可回读 |
| 背光 | LCD LED | GPIO21，LEDC 5 kHz，10-bit | 与 UART0 默认 TX 冲突 |
| 按键 | UP / DOWN / OK | GPIO0，ADC1_CH0 | 分压电阻梯，外部 10 kΩ 上拉到 3.3 V |
| 音频 codec | ES8311 | I2S0 全双工 + I2C0 | MCU 为 I2S 主机，codec 为从机 |
| 麦克风 / 喇叭 | 板载 | 经 ES8311 | 单声道麦克风必须 `no_dac_ref=true` |
| 电量计 | CW2017 | I2C0，7 位地址 `0x63` | 100 kHz；500 mAh 电芯 profile |
| 电池 | 500 mAh 锂电 | — | 初始化时匹配厂商 profile |
| 控制台 | USB Serial/JTAG | GPIO18 / GPIO19 | **115200 波特**，不是 UART0 |
| 充电 / 供电 | USB Type-C 2.0，5 V | — | 另有独立硬件电源键 |
| NFC | 无源 NTAG213 | — | 本应用未使用 |

### 引脚表

| GPIO | 功能 | 备注 |
| ---: | --- | --- |
| 0 | 三键 ADC 节点 | ADC1_CH0，外部 10 kΩ 上拉，属 boot strapping 引脚 |
| 1 | LCD CS | ST7789P3 片选 |
| 2 | I2S DOUT | MCU → ES8311 |
| 3 | I2S WS | MCU 为主机 |
| 4 | I2S DIN | ES8311 → MCU |
| 5 | I2S BCLK | 收发共享 |
| 6 | I2S MCLK | codec 需要，256 × fs |
| 7 | I2C SCL | 开漏，ES8311 与 CW2017 共用 |
| 8 | LCD SCLK | SPI2，80 MHz，mode 0 |
| 9 | LCD MOSI | 无 MISO |
| 10 | I2C SDA | 已启用内部上拉 |
| 18 / 19 | USB Serial/JTAG | 控制台专用 |
| 20 | LCD DC | 命令 / 数据选择 |
| 21 | 背光 PWM | LEDC 输出 |

BSP 中 LCD 复位与功放使能都是 `-1`：屏幕走软件复位，功放视为常开。
未列在此表中的 GPIO 不代表空闲可用。

### 按键电气细节

电阻梯把 GPIO0 分别经 0 Ω（UP）、1 kΩ（DOWN）、2.2 kΩ（OK）拉到地，外部 10 kΩ
上拉到 3.3 V。在 `ADC_ATTEN_DB_12` 下的标称电平与判定窗口：

| 状态 | 标称 | 窗口 (mV) |
| --- | ---: | --- |
| 按下 UP | ~0 mV | `[0, 150)` |
| 按下 DOWN | ~300 mV | `[150, 447)` |
| 按下 OK | ~595 mV | `[447, 1900)` |
| 松开 | ~3300 mV | `>= BSP_BTN_MV_RELEASED_MIN` |

绝不要使用内部上拉（约 45 kΩ 且不准）。按键时间显式传入：
`BSP_BTN_SHORT_PRESS_MS = 180`、`BSP_BTN_LONG_PRESS_MS = 500`。

### 决定固件形态的两个约束

1. **无 PSRAM。** 约 400 KB 的 SRAM 要同时容纳 Wi-Fi 与 lwIP 缓冲、mbedTLS 记录缓冲、
   LVGL 内存池、音频链路与系统堆。下面每一个与内存相关的 `sdkconfig` 取值都源于一次
   实测的分配失败，不是偏好问题。
2. **GPIO21 是背光。** 控制台必须留在 USB Serial/JTAG；任何日志接口改动都要重新核对
   引脚占用。

## 2. 工具链

| 项 | 要求版本 | 备注 |
| --- | --- | --- |
| ESP-IDF | **必须精确为 v5.5.3** | tag commit `b31fcc7a314a44ad992b58f589f7d1d8a4fadff6`。`idf.py --version` 输出不符就必须停下 |
| Target | `esp32c3` | 全新检出需 `idf.py set-target esp32c3` |
| 交叉编译器 | ESP-IDF 安装的 `riscv32-esp-elf` | 不要用系统自带的 RISC-V gcc 替代 |
| Python | ESP-IDF v5.5.3 安装器所提供的版本 | v0.1.0 产物是在安装器环境的 CPython 3.13.13 下产出的 |
| esptool | 4.12.0 | 在 IDF Python 环境中以 `python -m esptool` 调用 |
| CMake / Ninja / ccache | 由 IDF 安装器提供 | 5.5.3 默认关闭 ccache，用 `IDF_CCACHE_ENABLE=1` 打开 |

环境部署、各操作系统依赖包、镜像与修复步骤见
[环境准备](/docs/development/engineering/environment-setup.zh_CN.md)。

## 3. 托管组件

由 [dependencies.lock](/dependencies.lock) 锁定。首次构建会下载到 `managed_components/`，
该目录已被 git 忽略，绝不可手工编辑。

| 组件 | 锁定版本 | 约束来源 | 用途 |
| --- | --- | --- | --- |
| `lvgl/lvgl` | 9.6.0~1 | `esp_lvgl_port`（>=8,<10） | 界面渲染 |
| `espressif/esp_lvgl_port` | 2.9.0 | BSP（>=5.2） | LVGL 任务、显示与触摸对接 |
| `espressif/esp_codec_dev` | 1.6.2 | BSP（>=4.0） | ES8311 音频 |
| `espressif/esp_websocket_client` | 1.8.0 | [main/idf_component.yml](/main/idf_component.yml)（`^1.5.0`） | QCA 实时语音通道 |
| `espressif/button` | 4.2.0 | BSP（>=4.0） | IoT Button，ADC 分压输入 |
| `espressif/cmake_utilities` | 1.1.1 | 传递依赖 | 组件构建辅助 |
| `components/bsp` | 仓库内自带 | — | 显示、音频、电池、按键、I2C、引脚 |

本项目新增的唯一 manifest 依赖是 `esp_websocket_client`。正常构建不应让
`dependencies.lock` 出现无法解释的差异；改动 manifest 后须用 ESP-IDF 5.5.3 重新生成。

### 组件仓库访问

构建需要访问 ESP Component Registry。中国大陆可为当前终端设置文件存储镜像：

```bash
export IDF_COMPONENT_STORAGE_URL="https://components-file.espressif.cn"
```

它只改变组件压缩包的下载来源，版本仍取自被跟踪的 lock 文件。

## 4. 字体工具链（仅重新生成字体时需要）

生成的字体**已提交进仓库**，因此普通检出无需以下任何东西即可构建。重新生成使用
[assets/fonts/build_fonts.py](/assets/fonts/build_fonts.py)：

| 项 | 版本 | 用途 |
| --- | --- | --- |
| `fonttools` | 4.60.1 | 可变字体实例化到 wght=500 并做子集化 |
| Node.js + `lv_font_conv` | 1.5.3（`npx -y lv_font_conv@1.5.3`） | 输出 LVGL C 源码，4 bpp，不压缩 |
| Noto Sans SC 可变字体 | SIL OFL 1.1 | 字形来源 |

产物：`assets/fonts/app_font_cjk_16.c`（可打印 ASCII 加源字体中包含的全部 GB2312 码位，
约 6.2 MB）与 `assets/fonts/app_font_status_28.c`（只含固定状态词与数字）。
各自带一个 `.codepoints.txt` 兄弟文件。覆盖率由 `tests/test_font_coverage.py` 检查，
该套件**已纳入门禁**。细节见
[中文字体指南](/docs/development/engineering/lvgl-chinese-fonts.zh_CN.md)。

## 5. 主机测试工具链

| 项 | 被谁需要 | 备注 |
| --- | --- | --- |
| Python 3 | Python 契约套件、`tools/*.py` | 无需第三方包 |
| 主机 C 编译器（`cc`） | C 逻辑套件 | 以 `-std=c11 -Wall -Wextra -Werror` 编译；Ubuntu/Debian 装 `build-essential` |
| `curl`、`tar`、SHA-256 工具 | `tools/validate.sh --static` | 用于下载并解包校验和锁定的 `actionlint` |
| `actionlint` | 工作流 lint | 缺失时由 `tools/install-actionlint.sh` 下载 |
| git | `tools/check_repo.py` | 它用 `git ls-files --cached --others --exclude-standard` 枚举文件 |
| Node.js | `tests/test_portal_page.mjs` | **未注册进门禁**，需手动运行 |

没有主机 C 编译器时，`--static` 与完整门禁无法运行。此时应改跑 `--firmware` 加逐个
Python 套件，并把 C 执行部分如实报告为 NOT RUN，而不是 PASS。

## 6. 构建配置

全部取值都在 [sdkconfig.defaults](/sdkconfig.defaults)；门禁在隔离的临时目录中据此构建，
不会碰开发者根目录的 `sdkconfig`。

### Flash、分区、控制台

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_IDF_TARGET` | `esp32c3` | — |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | `y` | 产品基线 |
| `CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` | `n` | 固定 8 MB 镜像头，使 `merge-bin` 结果确定 |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | 使用 [partitions.csv](/partitions.csv) |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | `y` | GPIO21 是背光，UART0 不可用 |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | `y` | 不设第二控制台 |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick，供音频与重试定时器使用 |

### 分区布局

| 分区 | 类型 / 子类型 | 偏移 | 大小 |
| --- | --- | ---: | ---: |
| `nvs` | data / nvs | `0x9000` | `0x6000`（24 KB） |
| `phy_init` | data / phy | `0xf000` | `0x1000`（4 KB） |
| `factory` | app / factory | `0x10000` | `0x7f0000` |

无 OTA 槽。v0.1.0 应用约 2.60 MB，`factory` 分区还剩约 69% 空闲。策略见
[固件布局](/docs/development/engineering/firmware-layout.zh_CN.md)。

### 界面

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_LV_COLOR_DEPTH_16` | `y` | RGB565 屏 |
| `CONFIG_LV_DEF_REFR_PERIOD` | `20` | 最高 50 FPS |
| `CONFIG_LV_MEM_SIZE` | `32768` | LVGL 9.3+ 只认字节数；旧的 `LV_MEM_SIZE_KILOBYTES` 会被默认 64 KB 静默覆盖，曾因此让 BLE 控制器报 NO_MEM |
| `CONFIG_LV_TXT_ENC_UTF8` | `y` | 中文界面文本 |
| `CONFIG_LV_USE_FONT_PLACEHOLDER` | `y` | 缺字形时可见地显示占位，而不是一片空白 |
| `CONFIG_LV_USE_QRCODE` | `y` | 配网二维码 |

LVGL 刷新缓冲为一个 240 × 20 的 RGB565 缓冲，约 9.6 KB；最终 flush 会按全局 30 px
圆角做遮罩。

### 蓝牙

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_BT_ENABLED` | `n` | 配网改为 WPA2 网页门户，射频预算让给 Wi-Fi 与 TLS |

上游 BluFi 参考实现在 `demo/blufi-provisioning` 分支，记录于
[Wi-Fi 配网](/docs/development/engineering/wifi-provisioning.zh_CN.md)。

### TLS 内存策略（无 PSRAM）

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | `n` | 动态缓冲按"每条记录"临时分配接收区；语音会话中堆被 Wi-Fi/lwIP 收包切碎，服务端满长 16 KB 记录需约 16.7 KB 连续块，实机反复以 `-0x7F00` 失败 |
| `CONFIG_MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` | `y` | 启用 RFC 6066 协商 |
| `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` | `y` | 允许收发长度不同 |
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | `8192` | 足以重组约 5.6 KB 的证书链握手消息 |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | `4096` | — |
| `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | `n` | 只保留对端证书摘要，省约 4 KB 堆 |

`main/tls_mfl.c` 用 `-Wl,--wrap=` 与 `-Wl,-u,` 包装 `mbedtls_ssl_config_defaults`，
确保链接器保留该包装函数，向服务端请求 4 KB 最大分片长度。`api.qoder.com` 已确认会响应。

动态缓冲与 max-fragment-length 是**刻意不组合使用**的：mbedTLS 重组握手消息时按完整
接收缓冲长度搬移分片，而动态缓冲只按当前记录分配，证书被分片后会越界写堆——这个组合
在实机上开机即崩溃。

证书校验与密码套件均未改动。这里没有任何一项削弱 TLS 安全性，只缩小缓冲尺寸。

### TCP / lwIP

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | `8192` | 24 kHz base64 下行时，16 KB 积压窗口会挤掉 TLS 记录所需的连续内存 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | `11520` | — |
| `CONFIG_LWIP_TCP_RECVMBOX_SIZE` | `16` | — |
| `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` | `16` | 与参考 C3 调优一致 |
| `CONFIG_LWIP_DHCPS_ADD_DNS` | `y` | 门户 DHCP 下发 DNS 地址，强制门户跳转才能生效 |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | `1024` | 给门户 CSRF 头留空间 |

### Wi-Fi 缓冲

| 配置 | 取值 | 原因 |
| --- | --- | --- |
| `CONFIG_ESP_WIFI_IRAM_OPT` | `n` | C3 的 IRAM 与 DRAM 共用同一块 SRAM；关闭可把约 25 KB 代码挪回 Flash，把这块 RAM 还给堆。实时语音约 64 KB/s，远低于不开优化时的吞吐上限 |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | `n` | 同上 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `8` | 由 10 降到 8，省约 3 KB |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | `10` | 用默认值（收发各 32）时，插话瞬间 Wi-Fi/lwIP 一度占约 25 KB，最低剩余堆只剩 772 字节。10 个满长帧约 20 KB；8 KB TCP 窗口最多约 6 个报文在途加 4 个乱序缓存，且 Kconfig 下限是静态的 8 个 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | `16` | — |

超出缓冲上限的报文由 TCP 重传恢复，不会破坏数据或 TLS。

## 7. 应用架构

| 模块 | 职责 |
| --- | --- |
| `main/main.c` | 应用任务：开机判定、模板/身份选择、会话生命周期、按键分发 |
| `main/app_ui.c` | 全部 LVGL 页面，每个入口内部自行取 LVGL 锁 |
| `main/app_list.h` | 纯头文件的光标/窗口逻辑，可主机测试 |
| `main/app_fonts.c` | 字体选择 |
| `main/app_wifi.c` | STA 生命周期、NVS 凭据镜像、信道提示、退避重试 |
| `main/app_portal.c` | softAP + DNS + HTTP 门户、CSRF、异步配置 worker |
| `main/portal.html` | 门户页面，通过 `EMBED_TXTFILES` 嵌入 |
| `main/portal_form.c` | 表单解析与校验 |
| `main/qca_pat.c` | PAT 在 NVS 命名空间 `qca_app` 中的读写 |
| `main/qca_api.c` | 调用 QCA REST 接口的 HTTPS 请求 |
| `main/qca_list_parse.c` | 模板与身份列表解析 |
| `main/qca_voice.c` | 实时 WebSocket 语音会话、播放与采集任务 |
| `main/voice_logic.c` | 纯语音状态机，可主机测试 |
| `main/audio_dsp.c` | 增益与削波，可主机测试 |
| `main/b64_stream.c` | 流式 base64 解码，可主机测试 |
| `main/qjson.c` | 流式 JSON 解析，可主机测试 |
| `main/utf8_util.c` | UTF-8 工具，可主机测试 |
| `main/tls_mfl.c` | mbedTLS 最大分片长度包装 |
| `main/key_guard.h` | 按键消抖/防误触逻辑 |

线程模型：`app_task` 串行执行整个流程，是唯一做阻塞 HTTPS 的地方；按键回调只入队。
LVGL 由自己的任务驱动，语音页由 LVGL 定时器每 60 ms 轮询 `qca_voice_get_view()`——
语音任务从不触碰 LVGL。

### 云端端点

| 用途 | 取值 |
| --- | --- |
| 主机 | `api.qoder.com` |
| REST 基址 | `https://api.qoder.com/api/v1` |
| 实时语音 | `wss://api.qoder.com/api/v1/forward/realtime` |
| 协议版本 | `voice.realtime.v1` |
| 会话标题 | `Qoder Passport` |

定义在 [main/qca_config.h](/main/qca_config.h)。设备需要能对该主机的 443 端口发起
出站 TCP，并完整执行 TLS 证书校验。

### 凭据存储

命名空间 `qca_app`，键为 `pat`、`wifi_ssid`、`wifi_pass`、`wifi_ch`。
Wi-Fi 凭据除驱动自身的 `WIFI_STORAGE_FLASH` 持久化外，还在这里保留一份镜像，
因为本设备上实测驱动那份会跨重启丢失，而 `qca_app` 那份能存活。回填只在驱动为空时
进行。任何日志都不会打印 SSID、密码或 PAT。

## 8. 部署要求

### 构建所需

| 要求 | 细节 |
| --- | --- |
| 磁盘 | ESP-IDF 及工具链约 3 GB，`managed_components/` 约 200 MB，构建目录约 900 MB |
| 网络 | 能访问 ESP Component Registry，或使用上面的中国镜像 |
| 主机系统 | Linux（Ubuntu/Debian、Fedora、Arch）、macOS（Xcode CLT + Homebrew）、原生 Windows（乐鑫 ESP-IDF Tools 安装器）或 WSL2 |
| 依赖包（Ubuntu/Debian） | `git wget curl flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 build-essential` |
| 依赖包（macOS） | `brew install cmake ninja ccache dfu-util libusb python`，外加 Xcode Command Line Tools |
| 依赖包（Windows） | 乐鑫安装器提供工具链；命令须在其创建的 ESP-IDF 终端中运行 |
| Docker 替代 | `docker run --rm -v "$PWD:/project" -w /project -e IDF_GIT_SAFE_DIR=/project espressif/idf:v5.5.3 ./tools/validate.sh --firmware`——仅构建，不能 USB 烧录 |

WSL2 可以构建，但烧录与串口监视需要把 USB 转发进 WSL 实例；否则请在原生 Windows 的
ESP-IDF 终端里烧录。

### 烧录与观察所需

| 要求 | 细节 |
| --- | --- |
| USB 口与数据线 | 设备以 `VID:PID = 303A:1001` 枚举 |
| `esptool` | 4.12.0，在 IDF Python 环境中以 `python -m esptool` 调用 |
| Linux 串口权限 | 加入 `dialout`（或 `uucp`）组后重新登录 |
| 控制台波特率 | USB Serial/JTAG 上 **115200** |

写入前先只读枚举串口，确认目标设备而不是默认用第一个口。烧录需要用户明确同意；
检测到设备本身不等于获得授权。

### 运行所需

| 要求 | 细节 |
| --- | --- |
| Wi-Fi | 2.4 GHz 802.11 b/g/n 网络 |
| 云端 | 到 `api.qoder.com` 443 端口的出站 HTTPS 与 WSS |
| 账号 | 一个 Qoder Personal Access Token，需有权列出模板与身份、并开启实时会话 |

## 9. 验证门禁

```bash
source <path-to-esp-idf-v5.5.3>/export.sh
./tools/validate.sh --static      # 仓库规范 + 主机 C 与 Python 测试
./tools/validate.sh --firmware    # ESP-IDF 构建、merge-bin、布局校验、归档
./tools/validate.sh               # 两者
```

`--firmware` 产出 `build/FoloToy-AI-Passport-full.bin`，以及
`build/firmware/<full-bin-sha256>/` 下的归档包，内含 ELF、MAP、应用镜像、
`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`manifest.json`
与 `flash_args`。无需激活 IDF 即可校验归档：

```bash
python3 tools/archive_firmware.py verify build/firmware/<full-bin-sha256>
```

门禁会校验分区表 MD5、分区边界、标签唯一性、不重叠，以及应用偏移是否落在足够大的
app 分区内。构建失败时，`build/` 里遗留的旧文件不代表本次结果。CI 跑的是同一个脚本：
[.github/workflows/firmware-checks.yml](/.github/workflows/firmware-checks.yml)
与 [.github/workflows/static-checks.yml](/.github/workflows/static-checks.yml)。

细节见[构建与测试](/docs/development/engineering/build-and-test.zh_CN.md)。
