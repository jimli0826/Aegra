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
| `repository` | [repository.md](repository.md) | 企业 CAS、Gateway、索引与维护 |
| `virtualization` | [virtualization.md](virtualization.md) | 平台无关虚拟化用例与契约 |
| `adapters` | [adapters.md](adapters.md) | Windows、Storage、PostgreSQL、Dokan 和厂商实现 |
| `apps` | [apps.md](apps.md) | 进程入口、依赖注入和运行时边界 |

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

阶段 1 已建立 `base`、`contracts` 和 `ports` 骨架。阶段 2 已建立 Memory Block Adapter、Backup Session、Recovery Reader、fixed-size Chunker，以及带有界背压和取消的 Backup/Restore Pipeline。阶段 3 已完成通用 Manifest、字符串键 CBOR、Zstandard/metadata crypto Adapter，以及单 volume 个人版 `.bkf` 的端到端纵向切片。阶段 4 已完成 ZERO run 和加密 `.bhx` Sidecar。阶段 5 已完成 chunk 边界透明分卷、分卷组发布/中止清理、连续发现与跨卷恢复。阶段 6 已完成基于父 Sidecar 的稀疏增量层、零变化增量和显式 base-first Chain Reader；下一阶段实现差异备份或进入企业 Repository MVP。
