# 个人版 SQLite 控制面 Adapter（S2 / S6）

## 目标与非目标

个人版本地 Service 的权威控制面持久化：Repository 连接、SecretRef、Job、Schedule、Event/Audit 与
schema version。`.bkf`、Recovery Point、Manifest 与 Chunk Index **不是**本库权威数据；数据库删除后
可从 Repository 与配置重新建立索引。

非目标：Worker Supervisor（S3）与 Inventory/Repository API 编排（S4）的业务逻辑、Schedule 触发引擎与
完整审计查询产品能力（S8）、Desktop 接入。Service composition root 可以构造并注入本 Adapter。

## 依赖与 Target

```text
src/ports/include/aegra/ports/control_plane.h
src/adapters/sqlite/
├── CMakeLists.txt
├── include/aegra/adapters/sqlite/sqlite_control_plane.h
└── src/
    ├── sqlite_internal.h
    ├── sqlite_command_store.cpp
    ├── sqlite_control_plane.cpp
    ├── sqlite_support.cpp
    ├── sqlite_schema.cpp
    ├── sqlite_repository_connection_store.cpp
    ├── sqlite_job_store.cpp
    ├── sqlite_schedule_store.cpp
    ├── sqlite_service_settings_store.cpp
    └── sqlite_audit_event_store.cpp
```

| Target | 依赖 |
| --- | --- |
| `Aegra::Ports`（接口） | `base`、`contracts` |
| `Aegra::AdapterSqlite` | PUBLIC `Ports`；PRIVATE `unofficial::sqlite3::sqlite3` |

禁止：Qt、Service 协议 codec、Windows SDK、Archive/Repository 数据面、明文密码列。

## 端口拆分

- `IRepositoryConnectionStore`：upsert/get/list/set_default/remove（仅删除控制面引用）。
- `IJobStore`：insert/get/list/CAS `transition`/`mark_active_as_interrupted`/
  `purge_terminal_completed_before`。
- `IServiceSettingsStore`：get/upsert 单行控制面偏好（job retention）。
- `IScheduleStore`：upsert/get/list/remove。
- `IAuditEventStore`：append/list。
- `ICommandStore`：按 idempotency key 读取/插入不可变 command record。
- `IRestorePreflightStore`：插入/读取短期 Restore 安全快照；token 不可覆盖。
- `IControlPlaneUnitOfWork`：同一事务内访问上述 store；显式 `commit`，析构回滚。
- `IControlPlaneDatabase`：schema version、begin unit of work、只读查询快照（含
  `get_service_settings`）。

Job 状态机为纯函数：`queued/running/cancelling/succeeded/failed/cancelled/interrupted`。
Service 启动应对 `queued`、`running` 与 `cancelling` 调用 `mark_active_as_interrupted`；
`queued` 也必须收敛，因为 Worker 启动前已先提交 durable Job intent。

## Schema 与不变量

- `schema_meta.version` 当前为 `18`（`ports::kControlPlaneSchemaVersion`）。产品未发布：
  - 新库 `CREATE IF NOT EXISTS` 即为当前完整表结构，再写入 version=18；
  - **不提供** 历史 schema 的 `ALTER` 迁移或兼容读取；旧开发库必须删除后重建；
  - 非 0 且非当前版本 → `kUnsupportedVersion`。
- schema 18：`schedules.local_minutes_of_day`（TEXT，逗号分隔分钟，如 `120,900`）替换单值
  `local_minute_of_day`；一条 Schedule 可配置多个当日时刻。
- `service_settings`（schema 16）：单行 `id=1`；`job_retention_months` ∈ {1,3,6} 默认 3；
  `updated_utc_ms`。Service 启动与 `UpdateServiceSettings` 时按 30 天/月对终端 Job 做
  `purge_terminal_completed_before` 硬删除。
- `jobs.schedule_id`：backup Job 必填（拥有方 Schedule）；restore/verify 等为空串。ListJobs 投影到
  JobSummary.schedule_id，供 Desktop 按 schedule 绑定运行状态（同源 Schedule 互不串台）。
- `restore_preflight_entry_ids`（schema 12+）：file_set 选择性恢复 preflight 的 entry_id 列表
  （`preflight_token`、`ordinal`、`entry_id`）；volume preflight 无行。`RestorePreflightRecord.entry_ids`
  在 insert/get 时与主表同事务写入/附加。file 指纹 `chain_fingerprint` 以 `filec|` 前缀区分。
- `jobs.content_kind`：`1=volume_set`，`2=file_set`；backup/restore/verify job 必填。
- `jobs.source_ids`：volume_set 为 inventory source id；file_set 为 opaque selection UUID（**从不**写路径）。
- `jobs.exclude_page_and_hibernation_files` 可空（非 backup job）；backup job 必须写入。
- `jobs.request_fingerprint`：幂等键对应的规范化请求指纹。StartBackup 指纹覆盖
  `schedule_id`、**请求的** `backup_type`（非降级后的 effective 类型）、`content_kind`、
  volume `source_ids` 或 file `selection_id` 列表、`repository_connection_id`、exclude、encryption；
  volume 还必须覆盖 `deduplication_enabled`；
  重放时只比指纹，不从 effective Job 状态猜 demote。有 `idempotency_key` 时指纹不得为空。
- `jobs` FI7 结果投影（schema 13）：`result_requested_backup_type`、`result_effective_backup_type`、
  `result_effective_parent_uuid`、`result_incremental_downgrade_reason`。终端 transition 从 TaskResult
  写入；`backup_type`/`parent_recovery_point_id` 仍为请求侧 requested type 与 candidate parent。
- `schedules.content_kind` + `schedules.owner_sid`：创建时写入；`content_kind` 终身不可改。
- `schedule_file_selections`：file_set 专用子表（`selection_id`、`volume_identity`、相对路径 blob、
  entry/recursion/reparse/unreadable policy、display_label）；volume_set 时为空。
- `schedules.exclude_page_and_hibernation_files` 与 `schedules.deduplication_enabled` 必填；后者对 file_set
  固定 false。upsert command 的 idempotency fingerprint
  必须包含该选项与 `archive_password` 的不可逆摘要（不存明文）。
- `schedules.archive_password_protected`：加密 Schedule 为 `dpapi-lm:<schedule_id>:<base64>`
  （DPAPI `CRYPTPROTECT_LOCAL_MACHINE`，`pOptionalEntropy` = UTF-8 `schedule_id`）；未加密必须为空串。
  **不**返回给 Desktop `ScheduleSummary`。
- **Schedule 更新不变量**（`UpsertSchedule` 在已有 `schedule_id` 上强制）：
  - **创建后不可变**：`content_kind`、有序 volume `source_ids[]` 或 file selections、
    `exclude_page_and_hibernation_files`、`deduplication_enabled`、`encryption_enabled`；加密时 `archive_password_protected`
    创建后不可改、不可清空、不可关闭加密。file_set 更新不得携带新 `file_selections`。
  - **可修改**：`display_name`、`enabled`、`repository_connection_id`（可换其它 Repository connection）、
    `trigger`（频率/时间/星期等 Schedule settings）。
  - **`backup_type`**：创建与更新都写入 Incremental。不是用户策略；Run now / 定时请求增量，首次或 tip 空则降 Full。
    用户 Run full 只影响那一次 `StartBackup`。
  - **Backup options**：除未来的 “完成后关机”（shutdown）外，其它选项均为创建时固定；当前持久化选项为
    `exclude_page_and_hibernation_files`、`deduplication_enabled` 与 `encryption_enabled`，更新时不得变更。
  - 更新请求不得携带 `archive_password`；加密口令只在创建时 DPAPI 保护后写入 SQLite。
- `schedules.backup_set_uuid` 必填、canonical UUID；创建 Schedule 时分配并终身固定。同一 Schedule 的
  全量与增量共享该 set（含 Full→Inc→…→Full→Inc 序列：后继 Full 仍用同一 set，V6 下 Full 的
  `parent_uuid` 仍为 null，set 内形成森林）。
- `schedules.last_recovery_point_id`：可空；**下一次增量的唯一父候选**（`file_uuid`）。
  - 仅在该 Schedule 的备份 **Catalog 发布成功** 后更新为本次 `file_uuid`。
  - 不返回 `ScheduleSummary`；Client 不可写。
  - 更换 `repository_connection_id` 时清空。
  - 增量选父：**不以 Catalog tip 扫描回退**；空 tip / entry 缺失 / 父不合格 / 祖先链不完整 → **降级 Full**
    （仍用本 Schedule 的 `backup_set_uuid`）。校验链时只按 `parent_uuid` 逐条读 Catalog entry。
  - 详见 [personal_repository.md](personal_repository.md#service-增量选父与树完整判定)。
- Command 重放：`key` 不存在 → 执行；指纹相同 → Replayed；指纹不同 → `kConflict`。
- 打开时在 `BEGIN IMMEDIATE` 事务中 `CREATE IF NOT EXISTS` 并写入版本。
- 外键：`jobs.repository_connection_id` → `ON DELETE SET NULL`；
  `schedules.repository_connection_id` → `ON DELETE CASCADE`。
- 唯一：Repository `locator`、Job `idempotency_key`（非空）、Command `idempotency_key`、至多一个
  `is_default=1`、Restore preflight token，以及非空 `jobs.preflight_token`。
- Command record 保存请求指纹、command ID 与可选 resource ID；同键同请求可重放，同键不同请求冲突。
- `restore_preflights` 只保存 connection/recovery point/target ID、Repository UUID、链指纹、容量/链深和
  创建/过期 UTC；不保存 Secret、SecretRef、Archive path、Volume GUID、Manifest 或 Chunk Index。
- 一个非空 preflight token 最多关联一个 Job；数据库提供按 token 查询 Job，供 Start 在 Worker launch 前
  持久化并确认唯一 queued intent。
- 时间全部为非负 UTC 毫秒整数；超出有符号 64 位线范围拒绝。
- Job 与 Schedule 的 `source_ids` 使用有序字符串列表编码，必须包含 1 至 100 个稳定且无重复的 Source ID。
- 只存 `SecretRef` 形态字符串（含 Schedule 的 `dpapi-lm:<base64>` 密文）；不存明文凭据、
  Chunk Index、Manifest 或 Archive metadata。

## 并发模型

Service 单写者：`begin_unit_of_work` 以 `try_to_lock` 获取连接互斥锁，Unit of Work 在 commit/rollback
前一直持有该锁。公开只读 API 使用同一互斥锁，因此不会观察到未提交写入，也不会与
`write_transaction_open` 竞态。第二写者在锁被占用时返回 `kConflict`。SQLite `busy_timeout=3000`。
Unit of Work 共享拥有连接状态，因此 Database facade 可以先释放；连接在最后一个 UoW 结束后关闭。
commit/rollback 后该 UoW 的 Store 立即失效，后续读写返回 `kConflict`，不得落入 SQLite autocommit。

单次 Store 写操作中涉及多语句（例如清除 default 再 upsert）使用 SAVEPOINT，失败时回滚到保存点，
保证调用方即使继续 commit 也不会留下部分修改。

Continuation token 为不透明 `v1|<scope>|<filter>|<created>|<id>`，绑定 list 类型与过滤条件；跨
request kind 或不同 filter 复用 token 返回 `kInvalidArgument`。

## 验证

构建 SQLite Adapter 与 Service 生产 Target，并审查 schema 打开、状态转换、约束、事务、并发、command replay、
Restore preflight token 唯一占用和重启读取语义。涉及持久化变更时，使用隔离的非生产数据目录执行聚焦人工验证。

## Definition of Done

- 细粒度 ports 与独立 SQLite adapter target 可构建；
- 上述持久化与事务语义完成审查和必要的人工验证；
- 文档同步；顶层 CMake 与 Service composition 已接入。
