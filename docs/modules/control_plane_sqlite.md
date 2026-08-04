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
    └── sqlite_audit_event_store.cpp
```

| Target | 依赖 |
| --- | --- |
| `Aegra::Ports`（接口） | `base`、`contracts` |
| `Aegra::AdapterSqlite` | PUBLIC `Ports`；PRIVATE `unofficial::sqlite3::sqlite3` |

禁止：Qt、Service 协议 codec、Windows SDK、Archive/Repository 数据面、明文密码列。

## 端口拆分

- `IRepositoryConnectionStore`：upsert/get/list/set_default/remove（仅删除控制面引用）。
- `IJobStore`：insert/get/list/CAS `transition`/`mark_active_as_interrupted`。
- `IScheduleStore`：upsert/get/list/remove。
- `IAuditEventStore`：append/list。
- `ICommandStore`：按 idempotency key 读取/插入不可变 command record。
- `IRestorePreflightStore`：插入/读取短期 Restore 安全快照；token 不可覆盖。
- `IControlPlaneUnitOfWork`：同一事务内访问上述 store；显式 `commit`，析构回滚。
- `IControlPlaneDatabase`：schema version、begin unit of work、只读查询快照。

Job 状态机为纯函数：`queued/running/cancelling/succeeded/failed/cancelled/interrupted`。
Service 启动应对 `queued`、`running` 与 `cancelling` 调用 `mark_active_as_interrupted`；
`queued` 也必须收敛，因为 Worker 启动前已先提交 durable Job intent。

## Schema 与不变量

- `schema_meta.version` 当前为 `2`（`ports::kControlPlaneSchemaVersion`）。产品未发布，schema V2 直接定义当前
  表结构，不提供 V1 迁移或兼容读取。
- 打开时在 `BEGIN IMMEDIATE` 事务中 `CREATE IF NOT EXISTS` 并写入版本；未知更高版本返回
  `kUnsupportedVersion`。
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
- 只存 `SecretRef` 字符串；不存明文凭据、Chunk Index、Manifest 或 Archive metadata。

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

## 测试

`tests/adapters/sqlite_control_plane_test.cpp` 覆盖：

- schema 打开/迁移与 SecretRef 持久化；
- Job 合法/非法状态转换与启动 interrupt 收敛；
- 外键、唯一约束、单 default；
- UTC/非法时间与事务回滚；
- 损坏数据库与缺失 open-existing；
- 单写者 + 并发只读。
- command store 持久化、唯一键、同请求 replay 与请求指纹冲突。
- Restore preflight 插入/读取、非法记录、token 冲突、事务回滚、重启读取、取消、UoW 失效，以及 Job token
  唯一占用和反查。

## Definition of Done

- 细粒度 ports 与独立 SQLite adapter target 可构建；
- 上述测试通过；
- 文档同步；顶层 CMake 与 Service composition 已接入。
