# Aegra 品牌与产品命名

| 项 | 值 |
| --- | --- |
| 状态 | 现行规范 |
| 品牌 | Aegra |
| 产品族 | Aegra Image |

## 正式名称

| 场景 | 名称 |
| --- | --- |
| 品牌简称 | Aegra |
| 产品 | Aegra Image |
| 个人版 | Aegra Image Personal |
| 企业版 | Aegra Image Enterprise |
| 中文正式表述 | Aegra Image（艾格拉映像） |

`Aegra` 源自 *Aegis* 的守护意象，`Image` 表示磁盘与系统映像保护能力。对外不得再使用 Disk Backup、Backup Manager 等旧工作名称。

## 工程标识

| 用途 | 约定 |
| --- | --- |
| CMake Project | `Aegra` |
| C++ namespace | `aegra` |
| CMake target 前缀 | `aegra_` |
| Windows 服务名 | `AegraService` |
| 安装目录 | `%ProgramFiles%\AegraImage\` |
| 运行时数据 | `%ProgramData%\AegraImage\` |
| 注册表 | `HKLM\Software\AegraImage` |
| PE 注入目录 | `Windows\System32\AegraImage\` |

可执行文件使用小写连字符命名，例如 `aegra-service.exe`、`aegra-worker.exe`、`aegra-repository-gateway.exe`。最终安装包命名在发布 ADR 中冻结。

## 使用规范

- UI、安装器、官网和正式文档使用“Aegra Image”。
- 技术路径、服务和注册表使用无空格的 `AegraImage`。
- 源码文件和 Target 遵循工程规范的 `snake_case`。
- 旧 ABI 宏、旧解决方案名和旧二进制名不迁移到新项目。
