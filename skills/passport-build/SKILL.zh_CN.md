---
name: passport-build
description: 验证并打包 FoloToy AI Passport 固件，交付已校验的合并镜像及匹配的调试产物。用于构建、测试和固件打包请求，不执行烧录、提交或发布。
---

<p align="right"><strong>简体中文</strong> · <a href="SKILL.md">English</a></p>

# 构建与打包固件

确定目标工作区，执行 `git status --short --branch`，阅读其中的 `AGENTS.md`、
`docs/development/engineering/build-and-test.zh_CN.md` 和
`docs/development/engineering/firmware-layout.zh_CN.md`。下列命令均在该工作区
根目录运行，不猜测开发者私有 IDF 路径。

## 构建用户要求的配置

1. 确认已激活 ESP-IDF 5.5.3。缺失时使用可用的 `passport-setup`，或其对应
   环境指南。取得必要的联网／系统权限，不绕过审批。
2. 核对已跟踪 defaults、依赖与分区是否对应用户的应用。门禁从
   `sdkconfig.defaults` 生成隔离配置，不使用被忽略的根目录 `sdkconfig`。
   用户需要把本地独有设置交付到固件时，明确解决差异，不静默覆盖配置或
   构建不同版本。
3. 迭代时运行最小相关检查，最终交付前执行 `./tools/validate.sh`。复用现有
   门禁，不另造构建流程。允许有效的自定义分区。仅构建检查可执行
   `./tools/validate.sh --firmware`，在报告中标明未运行的 host tests。
4. 本轮失败不能证明 `build/` 中残留旧文件有效。修复范围内的失败或报告
   阻碍，不能把旧产物冒充失败构建的结果交付。

## 确定准确的交付产物

门禁保留 `build/FoloToy-AI-Passport-full.bin`，以及按内容哈希归档的
`build/firmware/<full-bin-sha256>/`。交接前运行：

```text
python3 tools/archive_firmware.py verify <archive-directory>
```

报告实际归档路径、完整镜像哈希，以及 `manifest.json` 中匹配的 ELF 身份。
归档保留 ELF、MAP、合并镜像、应用镜像、bootloader、分区表及 `flash_args`。
额外自定义分区镜像不单独保存，分段烧录需要时应另行取得。相同固件重复归档
按构建指南复用首份已验证 MAP。不能用之后重新编译的 ELF 替代，尤其是存在
未提交修改的构建。哈希用于关联产物，不代表
硬件验收通过，也不代表已验证供应方的可信身份。

交付验证通过、从 `0x0` 烧录的合并 `full.bin`，不得把应用单体 `.bin` 写到
该地址。说明合并烧录可能重置数据；保留设置可能需要按分区规范采用兼容的
分段烧录流程。无需备份原固件不代表获得全片擦除授权。

生成的二进制与调试包不得提交。固件／ELF 可能含应用内嵌秘密，未检查内容并
取得授权前，不上传调试文件或发布产物。

## 报告

分别报告 `Build`、`Host tests`、`Device tests`、`Unverified`，列出实际
检查与配置差异。完整实现固件修改后按 AI 指南邀请真机测试。仅构建／打包
不授权 USB 烧录、Git 写操作或发布。
