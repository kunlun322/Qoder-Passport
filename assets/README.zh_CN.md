<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

| 文件 | 字级、字重与字符集 | 用途与来源 |
| --- | --- | --- |
| [`fonts/app_font_cjk_16.c`](fonts/app_font_cjk_16.c) | 16 px，4 bpp，wght 500；可打印 ASCII + 源字体包含的全部 GB2312 码点（码点清单：[`app_font_cjk_16.codepoints.txt`](fonts/app_font_cjk_16.codepoints.txt)） | Qoder Passport 应用的正文字体；`LV_SYMBOL_*` 回退到 Montserrat 14。 |
| [`fonts/app_font_status_28.c`](fonts/app_font_status_28.c) | 28 px，4 bpp，wght 500；仅含 [`status_28_chars.txt`](fonts/status_28_chars.txt) 中的字符 | 语音状态大字（聆听中、已暂停……）。 |

来源：Noto Sans SC 可变字体（`NotoSansSC-VF.ttf`），© Adobe，SIL Open Font License 1.1（保留字体名 "Source"）；先实例化为 wght 500 并子集化，再用 lv_font_conv 1.5.3 转换。在门禁之外用 `python assets/fonts/build_fonts.py <NotoSansSC-VF.ttf> <work_dir>` 重新生成（需要 fontTools 与 Node.js）。`tests/test_font_coverage.py` 会检查应用所有界面字符串均被字库覆盖。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |
| [`images/readme-screen-provisioning.png`](images/readme-screen-provisioning.png) | 1152 × 1536，PNG RGB | 首次使用截图：设备自建 WPA2 配网热点，含二维码与门户地址。 |
| [`images/readme-screen-agent-templates.png`](images/readme-screen-agent-templates.png) | 1152 × 1536，PNG RGB | 首次使用截图：第 1 步，从 QCA 拉取的 Agent 模板列表。 |
| [`images/readme-screen-identities.png`](images/readme-screen-identities.png) | 1152 × 1536，PNG RGB | 首次使用截图：第 2 步，从 QCA 拉取的用户身份列表。 |
| [`images/readme-screen-voice-chat.png`](images/readme-screen-voice-chat.png) | 1152 × 1536，PNG RGB | 首次使用截图：一次实时全双工语音会话，含助手回复行。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

四张 `readme-screen-*.png` 界面截图来源：项目负责人于 2026-10-15 提供，用于嵌入中英文
项目 README 的界面一览；按提供时的字节原样提交。文件 XMP 中带有 TC260 AIGC 隐式标识
（AI 生成合成图像），按原样保留。配网截图中的热点名称、临时密码与二维码由设备在每次
配网时重新生成；经项目负责人明确同意，作为上一条规则的一次性例外发布。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
