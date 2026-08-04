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

`JobRequest` schema 3 是当前 Worker 的版本化任务信封，拥有 job、tenant、operation、source/target、
`SecretRef`、trace 和 deadline。Backup Job 还必须拥有 `BackupOptions`：显式 `type`、`file_uuid`、
`created_utc_ms`，全量必须拥有不同于 `file_uuid` 的 `backup_set_uuid`，增量时同时拥有
`parent_source_ref` 与 `parent_credential_ref`。Service 在提交 Worker 前分配持久化身份和创建时间，Worker
不得重新生成 Archive 身份。`SecretRef` 只保存凭据定位符，禁止保存明文 Secret。

`TaskProgress` 同时携带 `job_id` 与 `trace_id`，用于跨线程和跨进程关联。`TaskResult` 使用稳定的
`TaskOutcome`、`ErrorCode`、message code、warning code 和容量指标；不得复制 Adapter 的原始错误文本。
请求校验失败表示任务没有被接受，已接受任务的运行失败则形成合法 `TaskResult`。

`WorkerResponse` 是 Worker 的版本化根响应，互斥表达已接受任务的 `TaskResult`、请求拒绝和 Host 故障。
响应与内部结果必须保持 job/trace 关联一致。传输编码和进程退出码属于 `apps/worker`，不进入契约实现。

`WorkerCommand` 与 `WorkerEvent` 定义双向会话契约。首阶段 Command 只支持关联当前 job/trace 的 Cancel；
Event 互斥表达 `TaskProgress` 或最终 `WorkerResponse`。协议要求零到多个 Progress，随后至多一个 Result；
具体 framing、JSON 和 Named Pipe 不进入 Contracts。长期决策见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

`ServiceRequest`、`ServiceResponse` 与 `ServiceEvent` schema 3 定义本地 Desktop 控制面契约。根 envelope
显式区分 Request、Response 和 Event，request kind 与强类型 payload 必须匹配。查询不携带幂等键，命令必须
携带稳定幂等键；Response 互斥表达 QueryResult、CommandAccepted 或 RequestFailed。

V3 已定义 Repository connection、Source Inventory、Job、Schedule、Audit Event、Restore preflight、Mount
Session 和 task event DTO。列表每页最多 100 项，event 未确认窗口最多 128；所有 Qt 可见整数不超过非负
有符号 64 位范围。Catalog 状态仍不表达 Archive 已认证或 Restore Ready。完整 wire 决策见
[ADR-0013](../adr/0013-service-control-protocol-v3.md)。

Backup Start、Schedule 与 Backup Job 使用有序 `source_ids[]`，包含 1 至 100 个稳定且无重复的 Source ID。
该数组是一个 Job 的原子 Source 集合；协议层不得只保留第一个 ID，也不得拆成多个命令。Worker
`JobRequest.source_refs[]` 按相同顺序保存解析后的稳定 Volume 引用。

S6 修正了 Restore V3 DTO：Prepare 必须携带 Repository connection、Recovery Point 和 opaque target source ID；
成功 preflight 返回相同资源归属、逻辑大小、目标容量、链深、过期 UTC、eligibility 与稳定 message code；Start
只接受 opaque preflight token 且必须显式 `confirmed=true`。协议不接受 Archive path、对象 key、链数组、
SecretRef、Volume GUID 或任意设备路径。

## 验证

- 审查每个消息的必填字段、版本、枚举和组合不变量。
- 对编码 roundtrip、未知可选字段、损坏输入和 Golden message 执行聚焦的人工协议验证。
- 检查日志和调试输出不包含 Secret。

## 完成标准

- 契约不泄漏传输、平台或持久化实现。
- 版本行为和拒绝规则有完整文档，并与所有消费者保持一致。
- 所有消费者只依赖稳定 DTO，不解析另一进程的内部结构。
