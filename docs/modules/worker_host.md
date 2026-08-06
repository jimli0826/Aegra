# Worker Host 与进程协议

## 目标与边界

`apps/worker` 的 Worker Host 每次只执行一个已接收的任务。它负责跨进程消息校验、受信任运行配置、
外部停止与 deadline 合并、任务调用、异常收口、稳定退出码和响应编码，不负责备份算法、凭据持久化、
任务调度或控制面数据库访问。

## 任务日志

每个已接受的 Backup / Restore / Verify 任务写入一份独立文本日志（对齐旧版 `logs/backup` 风格）：

| 项 | 规则 |
| --- | --- |
| 根目录 | 优先 `AEGRA_DATA_DIR`（Service 启动时注入）；否则 `%LOCALAPPDATA%\Aegra` 或 `%ProgramData%\Aegra` |
| 路径 | `<data_dir>/logs/<operation>/YYYYMMDD_HHMMSS[_job-id].log`，`operation` 为 `backup` / `restore` / `verify`；文件名附带 `job_id` 便于检索 |
| 格式 | `[YYYY-MM-DD HH:MM:SS.mmm] [level] ...`，仅文件、无控制台；章节用 `[Section]`，字段用 `  key : value` |
| 结构 | 文件头（operation/path）→ `[Job]` / `[Request]` → 若干 `[Stage: name] begin|OK|FAILED` → `[Result]` |
| 字节/时长 | 人类可读双写，例如 `3.0 GiB (3203399680 bytes)`、`14 ms` / `2.136 s` |
| 失败必填 | `step`、`error_code`（`error_code_name`）、`error_message`（含 Win32）、可选 `hint`；`[Result]` 再汇总 `message_code` / `elapsed` |
| Backup 阶段 | `resolve_credentials` → `prepare_sources`（VSS/raw、bitmap、pagefile 排除）→ `create_archive` → `backup_pipeline`（按卷） |
| Restore 阶段 | `resolve_credentials` → `open_chain_reader` → `open_volume_reader` 或 `plan_disk_volumes` → `open_volume_sink` / `prepare_target_disk`+`open_disk_sink` → `restore_pipeline` →（disk）`rebuild_partition_table` |
| Verify 阶段 | `resolve_credentials` → `open_archive` → `verify_pipeline` |
| 禁止 | 密码、SecretRef 明文、凭据材料；可记 `password=present|empty` 或层计数 |

Worker 任务日志允许记录诊断所需的源/目标路径、Volume GUID、卷标、主机名和 Archive 信息，但不得记录
密码、密钥、Secret、Credential、SecretRef、访问/刷新令牌、会话令牌、Cookie、Authorization 内容或其他
可用于恢复、派生、重放认证状态的材料。记录用户数据时遵循最小必要原则，并受日志文件 ACL、轮转和保留
策略约束。

实现入口：`WorkerTaskLog` / `ScopedStage`（`apps/worker`）。Backup / Restore / Verify 均使用同一章节与 stage 模型。

Service 在 composition root 设置 `AEGRA_DATA_DIR`，Worker 子进程通过环境继承同一数据目录，保证
task log 与 Service 分级日志（`logs/trace.log` 等）同树。

当前消息使用 UTF-8 JSON。JSON 依赖只存在于 `apps/worker`，`contracts` 保持与传输技术无关。
`aegra_personal_worker.exe` 无参数时从 stdin 读取一个最大 1 MiB 的 Job，stdout 只写最终响应；正式父进程
监督使用 `--pipe <logical-name>` 双向会话。运行时系统能力与凭据部署见
[ADR-0007](../adr/0007-windows-worker-system-capabilities.md)，会话与 framing 见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

## 请求协议

根对象是 `JobRequest`，字段名固定如下：

| 字段 | JSON 类型 | 规则 |
| --- | --- | --- |
| `schema_version` | unsigned integer | 当前固定为 `3` |
| `job_id` | string | 必填、非空 |
| `tenant_id` | string | 必填、非空 |
| `operation` | unsigned integer | 使用 `JobOperation` 的显式数值 |
| `source_refs` | string array | 至少一个非空值 |
| `target_ref` | string | Backup/Restore/Export 必填；Verify 为空 |
| `credential_refs` | string array | 只允许 `SecretRef` 定位符 |
| `backup` | object | Backup 必填；含 `type`，增量还含两个父引用 |
| `restore` | object | Restore 选项：`disk_restore`、`source_disk_number`、`source_volume_index`、`bring_target_online`、`preserve_disk_signature`、`auto_expand_last_partition`。整盘必填 `disk_restore=true`；卷还原 `disk_restore=false` + `source_volume_index`（可省略整个 `restore`，则 volume_index=0） |
| `trace_id` | string | 必填、非空 |
| `deadline_utc_ms` | signed integer | 可选；`0` 表示无 deadline |

任何字段名包含 `password` 或 `secret` 的明文凭据字段一律拒绝。解析器检查 JSON 类型和整数范围，未知 schema、无效枚举、
字段缺失或格式错误都表示请求未被接受。业务运行参数，例如 block/chunk 大小、KDF 参数、应用版本和
主机名，来自 Worker 的受信任配置，不从 Job 消息接收。

个人版 Windows Worker 的 Backup 接受 1 至 100 个有序且无重复的 Volume `source_refs`，并在一个 VSS
Snapshot Set 中为同一 Job 创建一致性快照。`credential_refs` 为 `dpapi-lm:<entropy_id>:<base64>`
密文引用；Worker 经 `ICredentialResolver` 用 DPAPI LOCAL_MACHINE 与同一 `entropy_id` 解密得到非空密码字节。
Job 和响应都不携带明文。Backup 的
`backup.type` 数值为 `1=full`、`2=incremental`、`3=differential`；当前
Worker 明确拒绝 differential。Incremental 必须提供 `parent_source_ref`；`parent_credential_ref` 可选
（缺省时复用当前 Archive 口令或空口令），并在 Backend 调用期间同时保持新 Archive 和父 Archive 的
Secret 存活。多 Volume 增量要求父 Archive 与当前 Job 的有序 Volume 集合一致。

Verify Job 的 `operation` 为 `3`，`source_refs` 恰好一个 `.bkf`，`target_ref` 为空；Worker 会完整读取并
认证每个 Chunk，不创建目标文件。成功结果使用 `verify.completed`，错误使用脱敏的 `verify.*` code。

Restore Job 的 `operation` 为 `2`，`credential_refs` 必须与 `source_refs` 同长度且逐层对应
（未加密层为空字符串）。卷恢复：`source_refs` 为 base-first 链，`target_ref` 为 canonical Volume
GUID Path，且**不**携带 `restore` 对象。整盘恢复：`source_refs` 为 base-first 完整链
（Full，以及可选 Incremental 层），`target_ref` 为 `\\.\PhysicalDriveN`，且必须带：

```json
{"restore":{"disk_restore":true,"source_disk_number":0,"source_volume_index":0,"bring_target_online":true,"preserve_disk_signature":true,"auto_expand_last_partition":true}}
{"restore":{"disk_restore":false,"source_disk_number":0,"source_volume_index":1,"bring_target_online":true,"preserve_disk_signature":true,"auto_expand_last_partition":true}}
```

Worker 用 `PersonalArchiveChainReader` 合成 tip 视图，再按卷写入 PhysicalDrive。Service→Worker
编码（`encode_supervisor_job_request`）必须序列化 `restore`；省略时 Worker 按卷恢复处理，
PhysicalDrive 目标会以 `restore.invalid_request` 失败。安全边界见
[ADR-0009](../adr/0009-windows-volume-restore-safety.md) 与
[windows_personal_restore.md](windows_personal_restore.md)。

全量和增量 Backup 的 schema 3 片段分别为：

```json
{"backup":{"type":1}}
{"backup":{"type":2,"parent_source_ref":"D:\\Backups\\base.bkf","parent_credential_ref":"dpapi-lm:…"}}
```

父 Archive 路径是显式输入，不由 Worker 扫描目录猜测；当前 Archive 口令仍由顶层
`credential_refs[0]` 指定。

## 响应协议

`WorkerResponse` 的 `schema_version` 当前固定为 `1`，包含 `job_id`、`trace_id`、`kind`、
`boundary_error_code`、`message_code` 和可空 `task_result`。枚举按显式无符号数值编码。

| `kind` | 含义 | `task_result` | Boundary Error |
| --- | --- | --- | --- |
| `1` | 已接受任务的最终结果 | 必须存在 | `kNone` |
| `2` | schema 或请求校验拒绝 | 不存在 | `kInvalidArgument` 或 `kUnsupportedVersion` |
| `3` | Host、协议或边界故障 | 不存在 | 非校验类稳定错误 |

Host 在编码前验证响应，并确保响应及内部 `TaskResult` 的 job/trace 与输入 Job 一致。Adapter 原始错误、
异常文本、路径、SecretRef 和明文凭据不得进入响应。

## 退出码

| 数值 | 名称 | 含义 |
| ---: | --- | --- |
| `0` | `kSucceeded` | 成功或带告警成功 |
| `10` | `kTaskFailed` | 已接受任务运行失败 |
| `11` | `kCancelled` | 外部停止、deadline 或任务取消 |
| `20` | `kRequestRejected` | 请求未被接受 |
| `21` | `kHostFailure` | Host 异常、时钟或无效内部响应 |

退出码只用于进程监督和快速分类；Management Service 必须以结构化 `WorkerResponse` 为详细事实，不能
解析 stderr 或日志文本充当协议。

## 双向会话协议

父进程先发送一个 `JobRequest` frame。请求未通过解析或校验时，Worker 发送一个 kind=result 的
`WorkerEvent`，其 payload 是 RequestRejected `WorkerResponse`，随后以 20 退出。任务被接受后，Worker
可以发送零到多个 Progress event；父进程可以发送一次 job/trace 完全匹配的 Cancel command。未知、损坏
或关联不匹配的 command 会停止任务并形成 `worker.command_failed` Host failure。

任务结束后 Command Listener 先通过局部 cancellation 停止并 join，再发送唯一最终 Result，因此 Progress
与 Result 不并发写。Progress 发送失败会停止任务并映射为 `worker.progress_failed`。父进程断开时传输可能
无法承载最终 Result，此时进程退出码用于标识 Host failure。

## 取消与生命周期

Host 创建局部 `CancellationSource`，通过 `stop_callback` 合并外部进程停止，并在 deadline 到达时请求
停止。deadline 监视线程由 RAII 管理；任务提前结束时唤醒并 join，不残留后台线程。系统时钟为负数时
任务不得启动。取消令牌贯穿凭据解析、VSS、块读取、Archive 写入与 Pipeline。

Host 核心收口任务执行抛出的异常；未来真正的 `main` 仍必须作为进程级最后异常边界，因为构造拥有字符串
的响应本身可能因资源耗尽失败。Host 是同步、单任务对象，不持有任务完成后的权威状态。凭据仍只在任务
入口同步调用期间以 `IResolvedSecret` 存活；恢复链的全部 Secret 会共同存活到 Chain Reader 打开完成，
协议响应不会返回凭据引用。

## 验证与完成标准

- 人工协议验证覆盖合法请求、错误类型、未知版本、数值溢出和明文凭据字段拒绝；
- 审查成功、任务失败、请求拒绝、异常、无效响应、外部取消、运行中 deadline 和无效时钟路径；
- 所有响应通过契约校验，job/trace 不得串任务；
- Host 与协议代码只位于 `apps/worker`，核心模块不依赖 JSON、线程 Host 或 Windows API；
- Debug/Release、源码限制、静态分析、依赖检查和秘密扫描通过。
- 真实 exe 对无效输入输出合法拒绝 JSON，并以 `20` 退出。
