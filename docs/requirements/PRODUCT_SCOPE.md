# Aegra Image 产品范围

| 属性 | 内容 |
| --- | --- |
| 状态 | 重构产品基线 |
| 来源 | 旧 PRD 的有效需求，经新架构决议收敛 |

## 产品目标

Aegra Image 为 Windows 物理机和主流虚拟化平台提供映像级备份、验证、挂载与恢复。个人版强调本机单文件和易用性；企业版强调集中控制、全局去重、审计、规模化运维和灾难恢复。

## 个人版范围

- 卷、磁盘和系统备份。
- 全量、差异和增量备份链。
- 本地、SMB、S3 和 Azure 等存储目标，通过统一 Storage Port 接入。
- 卷恢复、整盘恢复、系统盘 WinPE 离线恢复。
- 恢复点检查、文件级挂载和虚拟磁盘呈现。
- 计划、保留策略、验证和任务进度。
- 单文件 `.bkf` V6，格式见[个人版格式规范](../format/PERSONAL_BACKUP_FORMAT_V6.md)。

个人版不使用企业 CAS Repository，不要求 PostgreSQL，也不维护 Repository 级全局 Chunk Index。
个人版可以使用受管理 Archive Store：`.bkf` Archive Group 是恢复权威，Storage Root 中的 Catalog 和
本机 SQLite 都是可重建投影。具体见
[个人版 Repository ADR](../adr/0010-personal-repository-authority-and-catalog.md)。

## 企业版范围

- Agent、Connector、受保护资源、策略、计划、任务和权限管理。
- 以 Repository 为去重安全域的全局去重。
- Pack、不可变 Index Segment、Recovery Point Manifest/Commit/Catalog。
- PostgreSQL 控制面、审计、Outbox 和可重建查询投影。
- Repository Gateway、GC、Compaction、Scrub、索引重建和灾难恢复。
- VMware 与 Hyper-V 的发现、快照、CBT/RCT、备份和恢复。
- 多租户隔离、配额、限流和审计。

## 必须满足的不变量

- PostgreSQL 丢失后，Repository 与密钥足以发现 Recovery Point、重建 Chunk Index 并恢复。
- Recovery Point 只有在 Commit Object 发布后可见。
- 物理机和虚拟机复用同一个 Backup/Restore Pipeline。
- 系统盘恢复必须离线执行，并在写盘前重新确认目标磁盘身份。
- 密钥和凭据不进入普通 Job 字符串、日志或数据库明文字段。
- 所有持久化格式和跨进程协议显式版本化。

## 当前非目标

- 未发布旧格式、旧 REST API、旧 SQLite Schema 或旧 ABI 的兼容。
- 在核心 Pipeline 中直接集成厂商 SDK、对象存储 SDK 或数据库。
- 跨租户默认去重。
- 在第一阶段同时实现所有虚拟化平台和 Instant Recovery。

## 开发优先级

1. 工程基线、模块边界和核心端口。
2. 内存 Adapter 与通用 Backup/Restore Pipeline。
3. Manifest 与个人版 `.bkf`。
4. 企业 Repository MVP。
5. PostgreSQL 控制面和投影重建。
6. Repository 维护能力。
7. VMware、Hyper-V 与高级恢复模式。
