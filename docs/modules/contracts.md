# `contracts` 模块开发文档

## 目标

定义跨模块和跨进程可序列化的稳定 DTO、标识和能力版本。契约描述“传递什么”，不描述“如何执行”。

## 职责

- `BackupJob`、`RestoreJob`、`TaskProgress`、`TaskResult`。
- `ProtectedResource`、`StorageLocation`、`RecoveryPointSummary`。
- `SecretRef`、租户/任务/资源稳定标识。
- Schema 版本、能力协商和验证函数。

## 依赖

只依赖 `base`。不得依赖 `ports`、Windows、Qt、HTTP、数据库或厂商 SDK。

## 契约规则

- 每个跨进程根消息包含 `schema_version`。
- DTO 拥有其数据，不保存 `string_view`、裸指针、Handle 或数据库连接。
- 密码、Token 和密钥只以 `SecretRef` 表示。
- 错误码和枚举显式定值；未知关键版本直接拒绝。
- JSON/CBOR/Protobuf 属于 Adapter 编码选择，DTO 本身不依赖序列化库。
- Job 至少包含 job、tenant、operation、source、credential refs、trace 和 deadline；
  Backup/Restore/Export 还需要 target，Verify 明确不需要 target。

## 当前状态

`JobRequest` schema **4** 是当前 Worker 的版本化任务信封，拥有 job、tenant、operation、`content_kind`、
source/target、`SecretRef`、trace 和 deadline。`content_kind` 为 `volume_set` 或 `file_set`，payload 互斥：

- **volume_set**：`source_refs` / `target_ref` / 可选 `RestoreOptions`；`file_source_refs` 与
  `file_restore_target` 必须为空。
- **file_set backup**：`file_source_refs`（1..100）+ `target_ref`；`source_refs` 为空。
- **file_set restore**：`source_refs`（archive 路径）+ `file_restore_target`；`target_ref` 为空。

Backup Job 还必须拥有 `BackupOptions`：显式 `type`、`file_uuid`、`created_utc_ms`，全量必须拥有不同于
`file_uuid` 的 `backup_set_uuid`。`deduplication_enabled` 必须显式存在：volume_set 默认 true 并可为 false，
file_set 必须 false。volume 增量同时拥有 `parent_source_ref`（`parent_credential_ref` 可选）。
file_set 允许 Full/Incremental：`selection_fingerprint` 必填；Incremental 可携带 `candidate_parent_uuid`，
并使用 ADR-0020 的 `mtime_size_v1` metadata baseline；禁止 volume 风格 `parent_source_ref`。Service 在提交
Worker 前分配持久化身份、创建时间与 selection fingerprint；Worker 不得重新生成 Archive 身份。`SecretRef`
只保存凭据定位符，禁止保存明文 Secret。

`TaskProgress` schema 4 同时携带 `job_id` 与 `trace_id`；`logical_bytes` 可为 null（文件枚举阶段未知总量），
并增加 `discovered_entries` / `processed_entries`。`TaskResult` schema 4 增加 `entry_count`、
`stream_count`、可选 `partial_restore`，以及 file_set backup 的 `requested_backup_type` /
`effective_backup_type` / `effective_parent_uuid` / `incremental_downgrade_reason`。不得复制 Adapter
的原始错误文本。ADR-0022 另增加非负 `deduplicated_block_count` / `deduplicated_logical_bytes`；仅成功的
volume_set backup 可为非零，其它任务固定为 0。

`file_set.h` 定义 `ContentKind`、名称编码、`FileSourceRef`、`FileEntryDesc`、`StableFileIdentity`、
`FileSelectionFingerprint`、
`FileContentStorage`、`IncrementalDowngradeReason`、`FileRestoreTarget`、`PartialRestoreStats` 与
产品上限常量；Contracts/Ports 禁止路径类型、HANDLE、Qt、JSON。
ADR-0020 已删除 `FileJournalCheckpoint`、`FileJournalState`、`FileChangeBatch` 等历史 USN 合同；current
Archive、Catalog 和 IPC 只表达 selection fingerprint、`FileChangeDetectionMethod` 与有效降级原因 1、2、3、9。
`StableFileIdentity` 的 null 值可用于不提供稳定文件身份的 FAT32 条目；不得合成路径哈希身份。

`WorkerResponse` / `WorkerCommand` / `WorkerEvent` 语义不变；具体 framing 见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

`ServiceRequest` / `ServiceResponse` / `ServiceEvent` schema **4**（API 4）定义本地 Desktop 控制面契约。
新增 query kind 13–15（浏览文件源、列出 RP 条目、PrepareFileRestore）与 command kind 48
（StartFileRestore）。`UpsertScheduleCommand` 使用 tagged `ProtectionSpecInput`（`volume_source_ids` 与
file selections 互斥）。`ScheduleSummary` / `JobSummary` / `RecoveryPointSummary` 携带 `content_kind`。
Repository 新增 query kind 17 `ListRepositoryDirectories`，仅接受 Connect 返回的 session-bound location
token 与分页参数，响应复用不含绝对路径的 `FileSourceNodePage`。
完整 wire 见 [ADR-0017](../adr/0017-service-control-protocol-v4.md) 与
[SERVICE_CONTROL_PROTOCOL_V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)。产品未发布，不实现 V3 解析。

Volume schedule 创建/更新：

- **创建**（无 `schedule_id`）：`protection.volume_source_ids`、Backup options、加密与 1–32 字符口令；
  口令经 Service 用 DPAPI `CRYPTPROTECT_LOCAL_MACHINE` 保护（`pOptionalEntropy` = `schedule_id`）
  并以 Base64 写入 SQLite。`backup_type` 一律存 Incremental；定时与 Run now 请求增量，无父则降 Full。
- **更新**（有 `schedule_id`）：不得携带 `archive_password`；保护源、
  `exclude_page_and_hibernation_files`、`deduplication_enabled`、`encryption_enabled` 与保护口令创建后冻结。
  允许修改 `display_name`、`enabled`、`repository_connection_id`、`trigger`
  （Daily/Weekly/Monthly；Monthly 携带 `day_of_month_mask`）。
  更换 destination 清空 `last_recovery_point_id`（下次增量降 Full）。

file_set schedule（F6）：

- **创建**：`protection.file_selections[]` 携带短期 browse `node_token`；Service 在当前 pipe session 下
  解析为 durable `FileSourceRef`（canonical `selection_id` UUID + `volume_identity` + 相对组件），
  规范化/去重后写入控制面；不记录调用者身份。同样固定 Incremental。
- **更新**：不得更换 `content_kind` 或重新提交 file selections（`schedule.source_frozen`）；
  加密选项冻结。
- Desktop 永不发送绝对路径；列表摘要含 `display_label` 与 volume-relative `display_chain`（UI 名，不是路径）。

Volume Restore：Prepare 必须携带 Repository connection、Recovery Point 和 opaque target source ID；
Start 只接受 opaque preflight token 且 `confirmed=true`。file_set 恢复使用 kind 15/48，不得走 volume
Prepare/Start。

## 验证

- 审查每个消息的必填字段、版本、枚举和组合不变量。
- 对编码 roundtrip、未知可选字段、损坏输入和 Golden message 执行聚焦的人工协议验证。
- 检查日志和调试输出不包含 Secret。

## 完成标准

- 契约不泄漏传输、平台或持久化实现。
- 版本行为和拒绝规则有完整文档，并与所有消费者保持一致。
- 所有消费者只依赖稳定 DTO，不解析另一进程的内部结构。
