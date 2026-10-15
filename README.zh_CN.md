[English](README.md) · **简体中文**

# Qoder-Passport

运行在 **FoloToy AI Passport** 可穿戴设备（ESP32-C3）上的全双工语音伙伴，对接
**Qoder Cloud Agents（QCA）**。用三颗实体按键选定 Agent 模板与用户身份，随后通过
WebSocket 连接 `api.qoder.com`，进行可随时插话打断的实时语音对话。

Wi-Fi 凭据与 QCA 个人访问令牌（PAT）都在设备自建的 `192.168.4.1` 配置网页上填写，
**不会编译进固件**。

| | |
| --- | --- |
| 项目名 | `Qoder-Passport` |
| 当前版本 | **v0.1.0** |
| 目标硬件 | FoloToy AI Passport（ESP32-C3，8 MB Flash，**无 PSRAM**） |
| 工具链 | ESP-IDF **v5.5.3**，target `esp32c3` |
| 云端后端 | Qoder Cloud Agents — `https://api.qoder.com/api/v1`、`wss://api.qoder.com/api/v1/forward/realtime` |
| 许可证 | MIT（衍生自 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)） |

> [!NOTE]
> 本仓库是构建在 FoloToy AI Passport 开放硬件平台之上的衍生应用。
> `components/bsp` 板级支持包、`docs/hardware-design/` 硬件文档、`tools/` 构建门禁
> 均来自上游，沿用 [LICENSE](LICENSE) 中的 MIT 许可证与版权声明。
> `main/` 中的应用——界面、状态机、Wi-Fi 配网门户、QCA 接入与音频链路——是本项目的原创实现。

---

## 功能

1. 开机后回填已保存的 Wi-Fi 凭据，接入 2.4 GHz 网络。
2. 从 QCA 拉取 **Agent 模板**列表（屏幕上的第 1 步）。
3. 拉取**用户身份**列表（第 2 步）。
4. 建立实时语音会话：你说、Agent 答，双方都能打断。下行是 24 kHz PCM 经 base64
   通过 WebSocket 传输，上行是麦克风 PCM，带设备端增益与削波控制。
5. 实时显示状态词（连接中、聆听中、思考中、回答中、已暂停、已结束、出错了）与滚动
   转写行，并可在对话中直接调音量。

若没有网络或没有 PAT，设备改为启动自己的 WPA2 配置热点，屏幕上显示二维码、临时密码
和门户地址。

## 硬件

FoloToy AI Passport，60 × 95 × 8.5 mm 可穿戴，50 g。

| 子系统 | 型号 | 接口 |
| --- | --- | --- |
| 主控 | ESP32-C3，8 MB Flash，**无 PSRAM** | — |
| 屏幕 | ST7789P3，240 × 320，RGB565 | SPI2 @ 80 MHz，mode 0 |
| 背光 | LCD LED | GPIO21，LEDC 5 kHz / 10-bit |
| 输入 | UP / DOWN / OK 分压电阻梯 | GPIO0（ADC1_CH0），外部 10 kΩ 上拉 |
| 音频 | ES8311 codec，板载麦克风与喇叭 | I2S0 全双工 + 共享 I2C |
| 电池 | 500 mAh 锂电，CW2017 电量计 | I2C0，地址 `0x63` |
| 控制台 | USB Serial/JTAG | GPIO18/19，**波特率 115200** |
| 充电 | USB Type-C 2.0，5 V | — |
| 无线 | 2.4 GHz Wi-Fi 802.11 b/g/n | STA 模式（本固件已关闭蓝牙） |

完整引脚表、总线归属、时序常量与已知陷阱见
[硬件开发指南](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)
与[产品规格](docs/hardware-design/specifications.zh_CN.md)。

有两个硬件事实决定了本固件的形态：

- **无 PSRAM。** TLS 记录缓冲、音频链路、LVGL 与 Wi-Fi 协议栈全部挤在同一块约
  400 KB 的 SRAM 里。[sdkconfig.defaults](sdkconfig.defaults) 与 `main/tls_mfl.c`
  中的内存策略正是为此存在，不能随意放宽。
- **GPIO21 是背光。** 所以控制台必须留在 USB Serial/JTAG；切到 UART0 会把背光引脚
  当成 TX 驱动。

## 快速开始

完整的用户操作说明见[使用指南](docs/usage/getting-started.zh_CN.md)。

你需要一个带有 **Personal Access Token（PAT）** 的 Qoder 账号，以及一个 2.4 GHz
Wi-Fi 网络。PAT 在 Qoder 账号设置中创建；设备只把它存进自己的 NVS，绝不写入日志。

### 方式 A —— 烧录预编译发布版

从本仓库的 GitHub **Releases** 页面下载 `Qoder-Passport-<版本>-esp32c3-full.bin`，
写入偏移 `0x0`：

```bash
python -m esptool --chip esp32c3 -p <端口> -b 460800 \
  write_flash 0x0 Qoder-Passport-v0.1.0-esp32c3-full.bin
```

合并镜像会填充 bootloader、分区表与应用之间的空隙，因此从 `0x0` 整片写入会
**重置 NVS 与 PHY 区域**——已保存的 Wi-Fi 凭据、PAT 与音量都会被清空。空白设备或
有意做干净安装时这是正确选择。若要在升级时保留已存配置，请改用
[固件布局与烧录策略](docs/development/engineering/firmware-layout.zh_CN.md)
中描述的分段写入。

### 方式 B —— 从源码构建

```bash
git clone <本仓库> Qoder-Passport && cd Qoder-Passport
# 先安装 ESP-IDF v5.5.3 的 esp32c3 工具链，见环境准备文档。
source <path-to-esp-idf-v5.5.3>/export.sh
idf.py --version                 # 必须精确输出 ESP-IDF v5.5.3
./tools/validate.sh --firmware   # 构建 + 校验合并镜像 + 归档
```

门禁会产出 `build/FoloToy-AI-Passport-full.bin`（合并镜像，烧录到 `0x0`），以及
`build/firmware/<full-bin-sha256>/` 下按内容哈希归档的调试包，内含 ELF、MAP、各分量
镜像、`manifest.json` 与 `flash_args`。CMake 工程名仍沿用 `FoloToy-AI-Passport`
以兼容上游工具链；发布时把产物重命名为 `Qoder-Passport-<tag>-esp32c3-full.bin`。

环境部署、各操作系统依赖包、中国大陆镜像与工具链修复见
[环境准备](docs/development/engineering/environment-setup.zh_CN.md)；
构建与门禁细节见[构建与测试](docs/development/engineering/build-and-test.zh_CN.md)。

## 首次使用

1. 开机（长按硬件电源键约 0.5 秒）。
2. 没有已保存网络时，屏幕进入**配网中**：显示二维码、热点名称与临时 WPA2 密码。
3. 用手机或笔记本连上该热点，打开 `http://192.168.4.1`。
4. 页面会自动扫描网络。选中你的 Wi-Fi（隐藏网络可手动输入名称），填写 Wi-Fi 密码，
   粘贴 Qoder PAT。
5. 保存。设备会**先**用 QCA 接口校验 PAT 再落盘，随后联网并重启进入正常模式。
6. 第 1 步选 Agent 模板，第 2 步选用户身份，开始对话。

在模板页、配网页或任何网络/PAT 失败页**长按 OK** 可进入设备设置，从那里可以刷新列表、
重新配网或恢复出厂。恢复出厂需要二次确认且默认选中**取消**，即便选中清除也仍需长按
才执行。

## 按键说明

| 操作 | 列表页 | 语音页 | 出错页 |
| --- | --- | --- | --- |
| ▲/▼ 单击 | 选中项移动 1 | 音量 ±10 | — |
| ▲/▼ 双击 | 移动 2 | 音量 ±10 | — |
| ▲/▼ 长按 | 移动一页（5 行） | 音量 ±10 | — |
| OK 单击 | 确认 | 暂停 / 继续 | 重试 |
| OK 长按 | 模板页=设备设置；身份页=返回模板 | 结束对话 | 设备设置或返回 |

短按判定 180 ms，长按判定 500 ms。按键回调只入队，所有阻塞操作都在应用任务里执行。

## 软件栈与版本

由 [dependencies.lock](dependencies.lock) 锁定；请勿编辑 `managed_components/`。

| 组件 | 版本 | 用途 |
| --- | --- | --- |
| ESP-IDF | 5.5.3 | SDK 与工具链 |
| `lvgl/lvgl` | 9.6.0~1 | 界面渲染 |
| `espressif/esp_lvgl_port` | 2.9.0 | LVGL 任务与显示对接 |
| `espressif/esp_codec_dev` | 1.6.2 | ES8311 音频 |
| `espressif/esp_websocket_client` | 1.8.0 | QCA 实时语音通道 |
| `espressif/button` | 4.2.0 | 三键 ADC 分压输入 |
| `espressif/cmake_utilities` | 1.1.1 | 组件构建辅助 |
| `components/bsp` | 仓库内自带 | 板级支持：显示、音频、电池、按键、I2C |

本项目新增的直接依赖：`espressif/esp_websocket_client ^1.5.0`
（见 [main/idf_component.yml](main/idf_component.yml)）。

[sdkconfig.defaults](sdkconfig.defaults) 中的关键配置：8 MB Flash 且固定镜像头、
自定义三项[分区表](partitions.csv)、USB Serial/JTAG 控制台、关闭蓝牙、
`LV_MEM_SIZE=32768`、mbedTLS 静态缓冲配合 RFC 6066 最大分片长度协商
（发送 4 KB / 接收 8 KB）、8 KB TCP 窗口，以及关闭 Wi-Fi IRAM 优化把约 25 KB 还给堆。
每一项都附有注释说明当初是哪一个实测故障促成的。

完整取舍理由、部署要求与复现步骤见
[Qoder-Passport 技术栈](docs/development/engineering/qoder-passport-stack.zh_CN.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `main/` | Qoder-Passport 应用：界面、Wi-Fi、门户、QCA 接口、语音、音频 DSP |
| `main/portal.html` | 配网门户页面（嵌入固件） |
| `components/bsp/` | 上游板级支持包 |
| `assets/fonts/` | 生成的 LVGL 字体：GB2312 16 px 正文、28 px 状态词 |
| `tests/` | 主机测试：纯逻辑 C 套件与源码契约 Python 套件 |
| `tools/` | `validate.sh` 门禁、固件校验、归档工具、仓库规范检查 |
| `docs/` | 硬件、工程、CI、发布与参考文档 |
| `skills/` | 开发 / 环境 / 构建 / 真机测试 / 排障的 Agent 技能 |
| `main/demo_*.c` | 上游硬件测试示例，保留供查阅，**不编入固件** |

## 测试

```bash
./tools/validate.sh --static     # 仓库规范、工作流、主机 C 与 Python 测试
./tools/validate.sh --firmware   # ESP-IDF 构建、merge-bin、布局校验
./tools/validate.sh              # 完整门禁（需已激活 ESP-IDF）
```

`--static` 需要 Python 3、主机 C 编译器、`curl`、`tar` 与 SHA-256 工具。
C 套件覆盖列表窗口光标、按键消抖、JSON 流式解析、base64 流式解码、音频 DSP、
语音状态机、UTF-8 工具、门户表单解析，以及 BSP 的显示/音频/按键逻辑。
Python 套件是源码契约回归，用于钉住诸如"Wi-Fi 密码绝不进入日志格式串"
"凭据镜像必须先于尽力而为的驱动落盘写入"这类不变量。

**编译通过不等于硬件验收。** 真机结果在每份发布说明中单列。

## 发布

- 发布说明：[v0.1.0](docs/development/release/v0.1.0.zh_CN.md)
- CI：推送 tag 会触发
  [.github/workflows/build-firmware.yml](.github/workflows/build-firmware.yml)，
  用 ESP-IDF v5.5.3 / `esp32c3` 走同一套门禁构建并发布 GitHub Release。
  Pull Request 与推送到 `main` 会运行固件检查与静态检查工作流。
- 变更历史：本项目把发布细节记录在 `docs/development/release/` 下；
  上游的 [docs/CHANGELOG.md](docs/CHANGELOG.md) 保留给上游平台历史。

## 安全与隐私

- QCA PAT 与 Wi-Fi 密码存放在设备 NVS 的 `qca_app` 命名空间，只能通过本地门户录入，
  两者都不编译进固件：`main/qca_secrets.h` 已被 git 忽略，且没有任何编译单元 include 它。
- 任何日志都不会打印 SSID、密码或 PAT。凭据相关路径只记录长度与 `esp_err_t` 名称，
  并由 `tools/check_repo.py` 与 Python 契约测试共同强制。
- 配置热点使用 WPA2 加每次会话随机的密码，不是开放网络。门户写操作需要一次性 CSRF
  令牌，且请求必须来自 AP 接口，否则拒绝。
- TLS 证书校验与密码套件保持 ESP-IDF 默认，未做任何削弱；内存调优只缩小缓冲尺寸。
- 恢复出厂会清除 Wi-Fi 凭据、PAT、音量设置与配网标记；不整片擦除 Flash，
  也不删除云端会话。

## v0.1.0 已知限制

- 只记住一个 Wi-Fi 网络。设备上没有多网络管理，换网络需要重跑配网门户。
- 用于加快开机关联的信道提示是运行时学习并存进 NVS 的。提示过期时驱动仍会从该信道
  继续往后扫描，所以过期的代价只是时间，不会导致连不上。
- 开机时若已保存的网络不可见，设备会重试约 21 秒，然后回退到配置热点。这是有意设计，
  但路由器短暂离线时可能让人意外。
- 没有 OTA，升级必须通过 USB 烧录。
- 6.2 MB 的生成字体 C 源码已提交进仓库，使检出后无需字体工具链即可构建。重新生成需要
  `fonttools`、Node.js 与 `lv_font_conv@1.5.3`，以及 Noto Sans SC 可变字体，见
  [assets/fonts/build_fonts.py](assets/fonts/build_fonts.py) 与
  [中文字体指南](docs/development/engineering/lvgl-chinese-fonts.zh_CN.md)。

## 参与贡献

请阅读 [CONTRIBUTING](.github/CONTRIBUTING.zh_CN.md)、
[提交与 PR 约定](docs/contribution/commit-and-pr.zh_CN.md) 与
[文档约定](docs/contribution/doc-conventions.zh_CN.md)。每份维护中的 Markdown 都需要
英文 `.md` 与简体中文 `.zh_CN.md` 配对并互相链接，CI 中由 `tools/check_repo.py` 强制。

Agent 辅助开发请从 [AGENTS.zh_CN.md](AGENTS.zh_CN.md) 与
[AI 开发指南](docs/development/ai-guide.zh_CN.md) 开始。

## 许可证

MIT，见 [LICENSE](LICENSE)。上游平台部分 Copyright (c) 2026 FoloToy；
本衍生应用以相同条款分发。
