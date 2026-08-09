# 模块开发文档

模块文档定义职责、依赖、公共接口、关键不变量、验证方式和完成标准。实现前必须阅读受影响模块及其直接依赖模块文档。

| 模块 | 文档 | 主要职责 |
| --- | --- | --- |
| `base` | [base.md](base.md) | 最小类型、错误、取消和安全算术 |
| `contracts` | [contracts.md](contracts.md) | 跨模块、跨进程 DTO 与版本契约 |
| `ports` | [ports.md](ports.md) | 外部能力抽象 |
| `format` | [format.md](format.md) | Manifest、个人 Archive、企业 Repository 二进制格式 |
| `pipeline` | [pipeline.md](pipeline.md) | 通用 Backup/Restore 数据面 |
| `application` | [application.md](application.md) | Use Case、权限前置和任务编排 |
| `personal_repository` | [personal_repository.md](personal_repository.md) | 个人版 Archive Store、可重建 Catalog、链图与删除计划 |
| `repository` | [repository.md](repository.md) | 企业 CAS、Gateway、索引与维护 |
| `virtualization` | [virtualization.md](virtualization.md) | 平台无关虚拟化用例与契约 |
| `adapters` | [adapters.md](adapters.md) | Windows、Storage、PostgreSQL、Dokan 和厂商实现 |
| 个人版 SQLite 控制面 | [control_plane_sqlite.md](control_plane_sqlite.md) | Repository 连接、Job/Schedule/Event stores、schema 迁移 |
| Windows Local Storage | [storage_local.md](storage_local.md) | 本地对象路径安全、staging、发布、generation 与删除 |
| `apps` | [apps.md](apps.md) | 进程入口、依赖注入和运行时边界 |
| `apps/worker` | [worker_host.md](worker_host.md) | 单任务 Host、进程协议、deadline 与退出码 |
| `apps/service` | [service_host.md](service_host.md) | 本地控制面 Host、Service IPC 与生命周期 |
| Service 协议 V4 wire | [../protocol/SERVICE_CONTROL_PROTOCOL_V4.md](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md) | Desktop↔Service 逐条 kind 字段、枚举与示例 JSON（含文件浏览/恢复） |
| `adapters/windows_ipc` | [windows_ipc.md](windows_ipc.md) | Named Pipe framing、Service ACL 与调用方身份 |
| `apps/desktop` | [desktop.md](desktop.md) | Qt/QML 客户端、Service 连接与页面迁移边界 |
| Repository Catalog 查询 | [personal_repository_catalog_query.md](personal_repository_catalog_query.md) | Scanner、Service 分页查询与 Desktop 列表 |
| 个人版 Verify | [personal_archive_verify.md](personal_archive_verify.md) | `.bkf` 完整只读认证与解压校验 |
| Windows 个人版恢复 | [windows_personal_restore.md](windows_personal_restore.md) | 卷恢复；Full/Incremental tip 整盘 disk→disk（Worker + Service + Desktop） |
| Windows 文件集备份 | [windows_file_set_backup.md](windows_file_set_backup.md) | schema 4 file_set Full/Incremental；ADR-0020 metadata signature 设计待实施 |
| Desktop / Service 完成计划 | [../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md](../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md) | UI 国际化、剩余页面迁移、Service 控制面与 agent 分工 |
| 文件集备份与恢复设计 | [../architecture/FILE_SET_BACKUP_RESTORE.md](../architecture/FILE_SET_BACKUP_RESTORE.md) | 文件/目录保护、V7 Archive、V4 Service、Windows 文件语义与恢复边界 |
| 文件集备份与恢复开发计划 | [../development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md](../development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md) | F0-F11 工作包、agent 文件所有权、前置关系与验收 |
| 文件集增量设计 | [../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md) | metadata signature、完整 tip Index、父 stream 与链式恢复 |
| 文件集增量开发计划 | [../development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md](../development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md) | FI0-FI10 历史工作包与 metadata signature 改造计划入口 |
| Metadata signature 改造计划 | [../development/FILE_SET_METADATA_SIGNATURE_DEVELOPMENT_PLAN.md](../development/FILE_SET_METADATA_SIGNATURE_DEVELOPMENT_PLAN.md) | ADR-0020 的 agent 工作包、文件所有权、构建与人工验收 |
| 文件集产品上限与稳定码 | [../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md) | 上限、message_code、人工损坏/失败矩阵 |
| Volume Set 去重设计 | [../architecture/VOLUME_SET_DEDUPLICATION.md](../architecture/VOLUME_SET_DEDUPLICATION.md) | 单 VolumeChunk 固定块去重、增量协作、恢复与指标 |
| 个人版 `.bkf` V7 | [../format/PERSONAL_BACKUP_FORMAT_V7.md](../format/PERSONAL_BACKUP_FORMAT_V7.md) | volume_set/file_set Archive 字节布局与 File Index |
| Repository Catalog V2 | [../format/PERSONAL_REPOSITORY_FORMAT_V2.md](../format/PERSONAL_REPOSITORY_FORMAT_V2.md) | content_kind 与文件统计可重建摘要 |

## 通用模块模板

新增子模块文档至少包含：

1. 目标与非目标；
2. 允许和禁止依赖；
3. 公共接口及所有权、线程、错误和取消语义；
4. 核心不变量；
5. 目录与 CMake Target；
6. 构建、静态检查和必要的人工验证；
7. 可观测性与安全要求；
8. Definition of Done。

## 当前阶段

阶段 1 至阶段 13 已建立核心模块、个人版 Archive/Repository、Windows 数据源与 Worker、Service IPC、SQLite 控制面和 Qt Desktop 的主要纵向切片。当前 S4 已完成 Inventory、Repository connection 和多连接 Recovery Point API 并接入 Service composition root。file_set 首版工作包 **F0–F10 已完成**（V7 Archive、Catalog V2、Service V4、Worker Full 备份/Verify/选择性恢复、Desktop UX、发布门禁）；文件 Incremental 已按 ADR-0020 切换为 metadata signature（同路径 `write_time + logical_size`），历史 USN 合同、Port 和 Windows reader 已删除。ADR-0021/FS1-FS5 已增加 FAT32 VSS snapshot source 与能力降级恢复目标；FAT32 不保存 stable File ID/ACL，目标单文件上限为 4 GiB - 1。ADR-0022 已冻结 Volume Set 单 Chunk 去重设计，生产实现尚未开始。本期仍不支持 reparse、hard link、sparse 或 ADS。仓库不维护测试用例，阶段验收使用生产 Target 构建、静态/架构检查和必要的人工运行或 UI 验证。
