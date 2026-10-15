<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 技能目录（Skills）

这些技能把仓库工程规范整理为有明确边界的 AI 工作流。开发应用时从
`passport-develop` 开始，单项任务直接使用对应技能。技能引用已有文档和
验证门禁，不再维护另一套硬件规则或构建命令。

## 目录约定

- 每个 skill 一个子目录，目录名即 skill 名称，命名简短、语义明确。
- 每个 skill 目录必须包含 `SKILL.md`，顶部用 YAML frontmatter 标注 `name` 与 `description`，其中 `description` 作为触发指纹，说清「何时触发、做什么」。
- 复杂的 skill 可在其目录下增加 `references/`（长文档）、`scripts/`（可执行脚本）、`assets/`（模板/样例），与主文件分开放。
- 文档应为纯 markdown，不含二进制内容。

## 如何添加一个 skill

1. 在 `skills/` 下新建以 skill 名命名的目录。
2. 目录内新建 `SKILL.md`，顶部写 `name` + `description` frontmatter。
3. 视需要增加 `references/`、`scripts/`、`assets/` 子目录。
4. 在本 `README.md` 的索引表中登记该 skill 的名称与一句话说明。

## 现有技能索引

| 技能 | 功能 |
| --- | --- |
| [passport-develop](passport-develop/SKILL.zh_CN.md) | 将需求落实为应用、测试及已验证交付，按需协调其余核心技能。 |
| [passport-setup](passport-setup/SKILL.zh_CN.md) | 检查并准备 ESP-IDF 5.5.3 环境，不覆盖其他安装，不假定能够访问 USB。 |
| [passport-build](passport-build/SKILL.zh_CN.md) | 运行统一门禁，保留已校验的合并固件和匹配的 ELF/MAP。 |
| [passport-device-test](passport-device-test/SKILL.zh_CN.md) | 获得授权后识别目标、刷写指定固件，区分串口日志与实机验收。 |
| [passport-debug](passport-debug/SKILL.zh_CN.md) | 依据日志和匹配符号，诊断崩溃、内存、中文空白及外设故障。 |
| [issue-suggestions](issue-suggestions/SKILL.zh_CN.md) | 发布后，收集开发者的改进点，整理成提交到上游的功能建议 issue。 |
| [experience-pr](experience-pr/SKILL.zh_CN.md) | 发布后，收集可复用的开发经验，并作为文档 PR 提交。 |
| [plays-archive](plays-archive/SKILL.zh_CN.md) | 发布后，把已发布应用归档到相对仓库根目录的 `docs/reference/<username>/<app-name>/`，附 AI 生成的双语说明；仅记录封面元数据，不提交图片。 |

## 必需准备，由 AI 自行完成

上表中的五个 `passport-*` 技能是必需项，不是可选建议。遵循
[必需技能规范](../AGENTS.zh_CN.md#必需-ai-技能)：AI 在开发前自行检查可用性，
缺少时自行安装。具体方式由 AI 根据当前工具、操作系统和权限决定，用户
无需提供安装方法或命令。

使用本仓库 `skills/` 中维护的源文件。仓库安装助手是可选工具，不是强制
安装方式。不能假定所有 AI 工具都采用相同的发现规则；需要时查阅所用工具
的文档，Codex 可参考[官方技能指南](https://learn.chatgpt.com/docs/build-skills)。

安装后验证当前 AI 环境确实能够发现／读取技能，不能仅凭文件已创建就宣称
可用。安装或启用受阻时，说明原因及所需的最小协助。解决阻碍期间可直接
阅读源文件辅助工作，但不能将此报告为安装成功。保留已有技能／配置，并
按环境要求取得必要权限。

## 使用示例

```text
使用 $passport-develop 开发离线中文番茄钟应用。
使用 $passport-setup 检查开发环境，先不要重新安装任何东西。
使用 $passport-build 打包当前固件，不烧录。
使用 $passport-device-test 测试已校验固件，保留我的设置。
使用 $passport-debug 分析这段崩溃日志，先不要改代码。
```

也可以用自然语言请求，由技能描述进行匹配。安装技能不等于授予 USB、联网、
Git 或发布权限。没有设备不阻塞写代码和主机测试；构建不授权烧录，诊断不
授权修改。完整实现固件需求后，AI 应主动邀请真机测试，而不是自行刷写。

## 维护与验证

每个技能独立目录，配对维护中英文说明，明确触发描述，并在本索引登记。
确定性操作交给可审查的工具，项目规范仍以权威文档为准。按
[技能验证](validation.zh_CN.md)执行路由和边界场景测试。统一静态门禁测试
安装助手及归档工具，完整门禁还会编译并校验实际调试归档；这些不代替真机测试。
