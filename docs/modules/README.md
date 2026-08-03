# 模块开发文档

模块文档定义职责、依赖、公共接口、关键不变量、测试和完成标准。实现前必须阅读受影响模块及其直接依赖模块文档。

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
| Windows Local Storage | [storage_local.md](storage_local.md) | 本地对象路径安全、staging、发布、generation 与删除 |
| `apps` | [apps.md](apps.md) | 进程入口、依赖注入和运行时边界 |
| `apps/worker` | [worker_host.md](worker_host.md) | 单任务 Host、进程协议、deadline 与退出码 |
| `apps/service` | [service_host.md](service_host.md) | 本地控制面 Host、Service IPC 与生命周期 |
| `apps/desktop` | [desktop.md](desktop.md) | Qt/QML 客户端、Service 连接与页面迁移边界 |
| Repository Catalog 查询 | [personal_repository_catalog_query.md](personal_repository_catalog_query.md) | Scanner、Service 分页查询与 Desktop 列表 |
| 个人版 Verify | [personal_archive_verify.md](personal_archive_verify.md) | `.bkf` 完整只读认证与解压校验 |
| Windows 个人版恢复 | [windows_personal_restore.md](windows_personal_restore.md) | 显式 `.bkf` 链到非系统 Volume 的安全恢复 |
| Desktop / Service 完成计划 | [../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md](../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md) | UI 国际化、剩余页面迁移、Service 控制面与 agent 分工 |

## 通用模块模板

新增子模块文档至少包含：

1. 目标与非目标；
2. 允许和禁止依赖；
3. 公共接口及所有权、线程、错误和取消语义；
4. 核心不变量；
5. 目录与 CMake Target；
6. 单元、契约、损坏输入和集成测试；
7. 可观测性与安全要求；
8. Definition of Done。

## 当前阶段

阶段 1 已建立 `base`、`contracts` 和 `ports` 骨架。阶段 2 已建立 Memory Block Adapter、Backup Session、Recovery Reader、fixed-size Chunker，以及带有界背压和取消的 Backup/Restore Pipeline。阶段 3 已完成通用 Manifest、字符串键 CBOR、Zstandard/metadata crypto Adapter，以及单 volume 个人版 `.bkf` 的端到端纵向切片。阶段 4 已完成 ZERO run 和加密 `.bhx` Sidecar。阶段 5 已完成 chunk 边界透明分卷、分卷组发布/中止清理、连续发现与跨卷恢复。阶段 6 已完成基于父 Sidecar 的稀疏增量层、零变化增量和显式 base-first Chain Reader。阶段 7 已完成 Chunk Payload XChaCha20-Poly1305 认证加密，并把 ChunkHeader 与 BlockEntry 绑定为 AAD。阶段 8A 已完成 Windows Volume 枚举和稳定对象重叠 I/O Block Source。阶段 8B 已完成独立的多 Volume VSS Snapshot Session、专用 MTA 工作线程和确定性生命周期测试。阶段 8C 已完成 Inventory、VSS、Block Source、个人 Archive 与 Backup Pipeline 的单 Volume Worker Composition Root。阶段 8D 已完成 SecretRef、TaskResult、凭据/随机源 Port，以及带 trace、deadline、取消和脱敏结果的个人版 Worker 任务入口。阶段 8E 已完成版本化 JSON 进程协议、WorkerResponse、稳定退出码、异常收口，以及外部停止与运行中 deadline 的合并取消。阶段 8F 已完成 Windows 系统时钟、CNG 随机源、Credential Manager Resolver 和 stdin/stdout 单任务 Worker 可执行入口。阶段 8G 已完成双向 Job/Cancel 与 Progress/Result 会话契约、可取消本地 Named Pipe framing、Worker `--pipe` 入口和真实拒绝进程测试。阶段 9 已完成个人版 Archive Verify Pipeline、Verify Worker Task 和只读认证校验测试。阶段 10 已完成显式 base-first Archive 链到非系统 Windows Volume 的 Block Sink、Restore Worker Task、逐层凭据编排与安全边界测试。阶段 11 已完成 Job schema 2、单卷增量 Backup Worker、显式父 Archive/凭据映射和父覆盖保护。阶段 12A 已完成个人版 Repository Descriptor/Catalog/Tombstone V1 codec 和 Recovery Point 链图核心。阶段 12B 已完成细粒度 Object Storage Port、Memory Object Storage 参考实现和公共 Contract Test。阶段 12C 已完成 Windows Local Object Storage Adapter。阶段 13A 已完成本地 Service 与 Qt 6 Desktop 连接/重连骨架。阶段 13B 已完成 Repository Catalog Scanner、Application 查询用例、Service 分页 API，以及 Desktop Recovery Point 列表和真实 Local Storage 进程测试。阶段 13C 已按旧项目 `blueExtra` 视觉基线迁移无边框窗口、可折叠侧栏、Repository 卡片和 Recovery Point 右侧抽屉，同时保留新的 Service IPC 边界。D0 已建立 Qt Linguist 五语言基础、`LocaleController`、message-code 映射与最小语言切换入口；D1 已将 Desktop 客户端拆为 transport、protocol、request coordinator 与 `RecoveryPointModel`。S0 已直接替换未发布 schema 2，完成 Service V3 Request/Response/Event envelope、强类型控制面 DTO、幂等命令、分页、事件背压契约和 Desktop 当前查询接入。管理员真实卷 E2E 仍按环境条件单独执行。
