# 本地 Management Service Host 开发文档

## 目标与非目标

`apps/service` 是个人版本地控制面的进程入口和协议 Host。阶段 13B 提供 Desktop 握手和个人版 Repository
Catalog 分页查询，建立后续任务和策略 API 的稳定入口。

Service Host 负责 framing、dispatch、会话生命周期、取消收口，以及正式 Windows Service 安全边界。
Composition root 打开个人版 SQLite 控制面、注入 Inventory/Repository Application use case，并通过
`WorkerSupervisor` 启动单任务 Worker；Service 本身不执行备份/恢复数据面，也不直接解析 `.bkf`。

## 依赖与 Target

```text
src/apps/service/
├── CMakeLists.txt
├── include/aegra/apps/service/
│   ├── service_host.h
│   ├── service_protocol.h
│   ├── service_response_fit.h
│   ├── service_security_host.h
│   ├── windows_service_control.h
│   ├── windows_service_scm_host.h
│   ├── worker_job_service.h
│   └── worker_supervisor.h
└── src/
    ├── recovery_point_layout_service.cpp  # GetRecoveryPointLayout：Catalog → Archive Manifest volumes
    ├── file_recovery_point_query.cpp      # ListRecoveryPointEntries：file_set Index 分页
    ├── service_host.cpp
    ├── service_log_formatter.cpp
    ├── service_main.cpp
    ├── service_protocol.cpp
    ├── service_protocol_request_json.cpp
    ├── service_protocol_response_json.cpp
    ├── service_response_fit.cpp           # list 响应按 1 MiB 帧预算收口
    ├── service_security_host.cpp
    ├── windows_service_control.cpp
    ├── windows_service_control_win32.cpp
    ├── windows_service_scm_host.cpp
    ├── worker_job_service.cpp
    ├── worker_job_service_file_restore.cpp  # PrepareFileRestore + StartFileRestore
    ├── worker_job_service_restore.cpp      # volume PrepareRestore + StartRestore
    └── worker_supervisor.cpp
```

- `aegra_app_service` / `Aegra::AppService`：依赖 Application、Base、Contracts、Ports、WindowsIpc；
  PRIVATE nlohmann-json 与 Advapi32。
- `aegra_service.exe`：Composition Root，依赖 AppService、SQLite、Local Storage、Windows Disk、
  Windows Filesystem、Windows Process、Windows System 与 Windows IPC Adapter。
- JSON、Win32、Qt、数据库类型不得进入 Contracts。

## 生命周期

Service 默认监听逻辑名称 `control`，实际 Pipe 名称见 [ADR-0011](../adr/0011-local-service-desktop-ipc.md)。
协议决策见 [ADR-0013](../adr/0013-service-control-protocol-v3.md)；**每一条 request/response/event 的字段与示例**
见 [SERVICE_CONTROL_PROTOCOL_V3](../protocol/SERVICE_CONTROL_PROTOCOL_V3.md)。
`--once` 只接受一个连接并处理一个请求，用于受控诊断；默认模式在会话断开后继续接受连接。

每个 session 顺序执行：

```text
Accept -> Receive Frame -> Decode/Validate -> Dispatch -> Encode -> Send -> Receive Next
```

断线结束当前 session，不结束 Service。取消必须唤醒 pending accept/receive。未知请求形成拒绝响应；只有传输
已经不可用或响应编码失败时 Host 返回边界错误。

## 协议与安全

- schema 3 使用 Request/Response/Event envelope，不兼容未发布的 schema 1/2；详见
  [ADR-0013](../adr/0013-service-control-protocol-v3.md)。
- `request_id` 用于请求响应对账，不作为权限或幂等凭据；查询不携带幂等键，命令必须携带幂等键。
- V3 已对外执行 `service.info`、Inventory、Repository connection/query、Job list、Backup start、Job
  cancel、Schedule 与 `GetRecoveryPointLayout`（kind 12：按 connection + recovery_point_id 打开 Archive
  Manifest，返回真实源卷 letter/label/filesystem/size）。Backup Start 和 Schedule 使用完整有序
  `source_ids[]`，一个命令只创建一个 Job。Recovery Point chain、delete plan/execute 与 Verify start 的
  handler 已接线，但 S5 完成门禁前不在 runtime capability 列表中；dispatcher 必须在调用 handler 前返回
  `service.capability_unavailable`。尚未接入的 Restore/Mount/Event 命令同样返回 capability unavailable。
- `service.settings`：kind 16 `GetServiceSettings` / kind 49 `UpdateServiceSettings`。控制面持久化
  `job_retention_months`（1/3/6，默认 3）；启动与更新后硬删除过期终端 Job（30 天/月）。
- Repository 响应只包含 Repository UUID 和不含客户 Metadata 的 Catalog 摘要，不包含根路径、Archive key、
  主机名、SID、SecretRef 或原始 Adapter 错误。
- frame 最大 1 MiB，JSON 根必须是 object，整数必须先检查范围。
- 含数组的 list 查询经 `service_response_fit` 按编码大小收口：接近 1 MiB 时减少本页项数并设
  `continuation_token`。`encode_service_response` 对超限帧明确失败。
- Job list 只合并 Worker 监督器缓存中的真实 progress；无缓存时 `progress` 为 null（不注入 1/1）。
- 错误响应使用稳定 `ErrorCode` 和 message code，不返回 JSON/Win32 异常文本。
- Service 控制 Pipe 在交互模式和 LocalSystem 正式模式都使用 `kLocalEveryoneControl`：允许本机
  Everyone 读写，同时拒绝远程 Client，并在连接后执行调用方 SID/session 授权。见
  [ADR-0014](../adr/0014-windows-service-ipc-security.md) 与
  [windows_ipc.md](windows_ipc.md)。
- 授权 Host：`run_authorized_service_host`；SCM stop 有界等待 STOPPED；`WindowsServiceScmHost`
  在 `stop_deadline` 内收口（非协作 worker 超时失败）。
- 正式 `--service` 模式在构造 runtime 前进入 `StartServiceCtrlDispatcherW`；两种模式的 Service 控制 Pipe
  都使用 `kLocalEveryoneControl`。Desktop 到 Service 的本地安全模型保持轻量，不承担远程零信任认证。

## Composition 配置

- `--data-dir` 接受绝对路径。缺省时 Service 模式使用 `%ProgramData%\Aegra`，交互模式使用
  `%LOCALAPPDATA%\Aegra`；控制面数据库为 `<data-dir>\control-plane.db`。
- `--worker-path` 接受绝对路径。缺省为 `aegra_service.exe` 同目录的 `aegra_personal_worker.exe`，不回退到
  当前工作目录。
- Worker session listener 使用独立 `aegra-worker-*` namespace、1 MiB frame limit 和每 session 一个
  `std::jthread`；Service control pipe 继续使用 Service namespace 与 1 MiB frame limit。
- `--repository-root` 仅保留为显式开发诊断直连查询；常规路径通过持久化 Repository connection 打开。

## 日志与交互追踪

- Service 日志按级别拆到 `<data-dir>/logs/` 下四个 rotating file（各 10 MiB、保留 5 个），不再写
  合并的 `service.log`：
  | 文件 | 级别 |
  | --- | --- |
  | `trace.log` | `trace`（含 Inbound/Outbound IPC JSON） |
  | `info.log` | `info` |
  | `warning.log` | `warning` |
  | `error.log` | `error`（含 `critical`） |
  Logger 最低级别为 `trace`；日志仍携带稳定 `event` code，便于机器筛选。
- `info/warning/error` 使用完整请求名称、自然语言结果、可读 response/error/message 名称，不输出裸
  `kind_value`、`response_kind` 或 `error_code` 数字。Job 终态同样输出 `Succeeded/Failed/Cancelled` 等名称。
- 每个 1 MiB 内的 Service IPC 请求和响应在编解码边界记录一条 `trace` JSON 到 `trace.log`，分别标记
  `Inbound` 和 `Outbound`，用于关联 `request_id` 并检查实际交互字段。
- trace JSON 在写盘前使用结构化解析递归脱敏：credential、password、secret、token、`*_key` 等认证、
  授权或会话材料的值替换为 `[REDACTED]`。路径、locator、显示名、卷标签、主机名和 message arguments
  等诊断所需用户数据允许保留。解析失败的 frame 不得原样记录，避免无法确认其中是否含认证信息。
- 任何日志级别都不得记录密码、密钥、Secret、Credential、SecretRef、访问/刷新令牌、会话令牌、Cookie、
  Authorization 内容或可用于恢复、派生、重放认证状态的材料。用户数据日志遵循最小必要原则，并受日志
  文件 ACL、轮转和保留策略约束。

## 验证与完成标准

- 审查契约、codec、dispatcher、session 和真实进程边界。
- 人工诊断覆盖授权成功、未授权拒绝、多 session、stop 取消 pending accept、
  install/recovery/restart/uninstall 与 recovery 失败 rollback。
- Service 可独立启动，Desktop 可以握手并显示 Ready 与版本。
- Service 不依赖 Qt、具体备份 Adapter 或数据库权威。
- 未知版本、损坏请求、断线和取消不会终止整个常驻 Host。
- 人工运行验证覆盖 SQLite 启动收敛、capability、数据目录、实际 Worker 启动、失败结果持久化及 session 回收。
- Debug/Release、源码规模、格式和秘密扫描通过。

## 后续控制面工作

Service 剩余工作按 [Desktop 迁移与个人版 Service 完成计划](../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md)
中的 `S5` 至 `S8` 工作包推进：Archive authenticate/Verify/删除计划、Restore、Mount、Schedule 与 Event/Audit。
现有 Worker 数据面继续复用，Service 不重复实现 Backup、Verify 或 Restore Pipeline。

Service 协议只返回稳定枚举、错误码和 message code，不返回本地化文本。个人版 SQLite 不保存 Recovery Point、
Chunk Index、Manifest 或 Archive metadata 的权威副本；Repository 仍是可重建 Recovery Point 事实来源。

## 当前状态

S0-S4 已完成；S5 进行中并已部分接入 composition：chain/delete/verify contracts、Application 用例、
delete-plan 核心和 Host dispatch 已存在，但 capability 保持关闭。完整 Verify Worker 人工进程验证、持久化
per-file Archive Credential 映射与 Local Storage 故障恢复验证仍待补齐。

`GetRecoveryPointLayout` 已实现：Catalog 定位 Archive → `PersonalArchiveReader` 读取 Manifest →
返回 hierarchical `disks[]`（分区表）+ `volumes[]`（letter/label/fs/size + extents）。无 `disks[]`
的 Archive（布局写入前产生）返回 `recovery_point.layout_failed`。个人备份加密：`encryption_enabled=false`
  时写入无加密 Archive；为 true 时创建 Schedule 要求 `archive_password`，Service 用 DPAPI
  `CRYPTPROTECT_LOCAL_MACHINE`（`pOptionalEntropy` = `schedule_id`）写入 SQLite
  `archive_password_protected`。`StartBackup` 仅接收 `schedule_id` + `backup_type`，从 Schedule
  展开其余参数与密文交给 Worker。Layout 当前优先打开无加密 Archive（空密码）。

### F6：文件浏览与 file_set 备份编排

- **API**：Service 控制协议 **V4**（见 [ADR-0017](../adr/0017-service-control-protocol-v4.md) 与
  [SERVICE_CONTROL_PROTOCOL_V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)）。
- **Browse**：`BrowseFileSources` 经 `FileBrowseService` 组合 Windows `IFileSourceBrowser`；
  Service 铸造短期 opaque token（TTL、caller SID + pipe session 绑定；**每 session 最多 4096**；
  根列表首页清空该 session 旧 token；断线 `clear_session`）。
  根节点仅包含带盘符的卷（如 `新加卷 (D:)` / `System (C:)`）；无盘符的系统隐藏分区
  （EFI/MSR/Recovery 等）不进入树。子节点枚举跳过 `HIDDEN|SYSTEM` 与
  `System Volume Information` / `$RECYCLE.BIN` 等系统保护项。
  **Backup 对齐**：`WindowsFileSnapshotView` 递归枚举同样跳过
  `System Volume Information` 与 `$RECYCLE.BIN`（整卷 file_set 选择时不写入 Archive）。
  `display_name` 为 UTF-8（含中文等非 ASCII 文件名），不再做 ASCII `?` 投影。
- **Session**：每个 Named Pipe 连接携带 `ServiceSessionContext`（peer SID + 唯一 session id）；
  `UpsertSchedule` 用其解析 file_set selection 并写入 `owner_sid`。
- **Schedule / Job**：控制面 schema **12**（含 F8 `restore_preflight_entry_ids`）；file_set selections
  存 `schedule_file_selections`；`StartBackup` 按 `content_kind` 构造 schema 4 Worker Job
  （file 路径走 `file_source_refs`，Job `source_ids` 仅为 selection UUID）。
- **Capabilities**（在 volume 根可用时）：`file.browse`、`schedule.file_set`；F8 另声明
  `file.restore`。
- **Catalog 发布**：`BackupCatalogRegistrar` 按 `content_kind` 写 Catalog V2（file_set 无 sidecar /
  source_volume_ids，写入 entry/stream 计数）。

### F7 / FI8：Recovery Point 文件查询与 Verify

- **API**：kind 14 `ListRecoveryPointEntries`（capability `file.recover_browse`）。
- **实现**：`file_recovery_chain` 经 Catalog V2 解析 tip→Full 链 →
  `PersonalFileArchiveChainReader` 认证全链 → 仅 tip Index 分页 `list_children`；
  响应不含 Archive key / stream offset。
- **Continuation**：绑定 `chain_generation`（各层 index digest 的 `+` 拼接）+ tip
  `index_generation` + `parent_entry_id` + reader offset；任一层 generation 或 parent 不匹配时拒绝。
- **错误码**：`file_recover.credential_required|failed|corrupt|catalog_only|parent_missing|
  parent_reference_invalid|chain_depth_limit`、`service.content_kind_mismatch`。
- **凭证**：先空口令；失败且 connection 声明 `archive.default_credential` 时使用 connection
  SecretRef；请求可带 `archive_secret_ref`。
- **Verify**：file_set `prepare_verify` 注入 base-first `source_refs`（全链）与匹配
  `credential_refs`；Worker 经 chain reader 做可恢复性 Verify。

### F8 / FI8：文件选择性恢复

- **API**：kind 15 `PrepareFileRestore` + kind 48 `StartFileRestore`（capability `file.restore`）。
- **Prepare**：解析 target 目录 token → inventory/capability/free space → 打开认证完整链 →
  tip 选择闭包累加 logical size，并对每个普通文件 `resolve_stream_reference`（写前解析 parent）→
  durable preflight（TTL 30 min）：
  - `chain_fingerprint` =
    `filec|archive_key|file_uuid|tip_digest|chain_gen|depth|conflict|sec|count|logical|target`
  - `chain_depth` = Catalog 链层数；`entry_ids` 存 companion 表
  - 不存 Archive/target 绝对路径或明文 Secret
- **Start**：校验 token 未过期/`filec|`/唯一占用；重开全链比对 tip digest 与 chain generation；
  Worker Job `source_refs` 为 base-first 全链路径 + 同密文凭据（每层一份）。
- **目标卷**：允许系统卷上的用户目录（Documents/Desktop 等）；`inventory.is_system` 仅用于
  volume restore 的 PE 门禁，不得拦截 file restore。只读/不可用卷仍拒绝。
- **错误码**：`file_restore.preflight_ok|expired|consumed|target_full|target_capability_missing|target_file_too_large`
  等；volume preflight token 不得启动 file restore。

### F10：发布门禁

- Debug/Release 生产构建通过；控制协议仅 V4、Job schema 仅 4、Catalog 仅 V2、控制面 schema 12；
  无 V6 dual-read / V3 negotiation / SQLite migration 分支。
- Capabilities 在 volume 根可用时声明：`file.browse`、`file.recover_browse`、`file.restore`、
  `schedule.file_set`（与 volume 能力并存，不互相替换）。
