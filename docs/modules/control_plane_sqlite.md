# 个人版 SQLite 控制面 Adapter（S2）

## 目标与非目标

个人版本地 Service 的权威控制面持久化：Repository 连接、SecretRef、Job、Schedule、Event/Audit 与
schema version。`.bkf`、Recovery Point、Manifest 与 Chunk Index **不是**本库权威数据；数据库删除后
可从 Repository 与配置重新建立索引。

非目标：Worker Supervisor（S3）、Inventory/Repository API 编排（S4）、Schedule 触发引擎与完整审计查询
产品能力（S8）、Service Host 接入、Desktop 接入。

## 依赖与 Target

```text
src/ports/include/aegra/ports/control_plane.h
src/adapters/sqlite/
├── CMakeLists.txt
├── include/aegra/adapters/sqlite/sqlite_control_plane.h
└── src/
    ├── sqlite_internal.h
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
- `IControlPlaneUnitOfWork`：同一事务内访问上述 store；显式 `commit`，析构回滚。
- `IControlPlaneDatabase`：schema version、begin unit of work、只读查询快照。

Job 状态机为纯函数：`queued/running/cancelling/succeeded/failed/cancelled/interrupted`。
Service 启动应对 `running` 与 `cancelling` 调用 `mark_active_as_interrupted`。

## Schema 与不变量

- `schema_meta.version` 当前为 `1`（`ports::kControlPlaneSchemaVersion`）。
- 打开时在 `BEGIN IMMEDIATE` 事务中 `CREATE IF NOT EXISTS` 并写入版本；未知更高版本返回
  `kUnsupportedVersion`。
- 外键：`jobs.repository_connection_id` → `ON DELETE SET NULL`；
  `schedules.repository_connection_id` → `ON DELETE CASCADE`。
- 唯一：`locator`、`idempotency_key`（非空）、至多一个 `is_default=1`。
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

## Definition of Done

- 细粒度 ports 与独立 SQLite adapter target 可构建；
- 上述测试通过；
- 文档同步；顶层 CMake / Service composition 集成列入交接清单。
