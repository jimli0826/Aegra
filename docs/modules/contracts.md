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

`JobRequest` 是当前 Worker 的版本化任务信封，拥有 job、tenant、operation、source/target、
`SecretRef`、trace 和 deadline。`SecretRef` 只保存凭据定位符，禁止保存明文 Secret。

`TaskProgress` 同时携带 `job_id` 与 `trace_id`，用于跨线程和跨进程关联。`TaskResult` 使用稳定的
`TaskOutcome`、`ErrorCode`、message code、warning code 和容量指标；不得复制 Adapter 的原始错误文本。
请求校验失败表示任务没有被接受，已接受任务的运行失败则形成合法 `TaskResult`。

`WorkerResponse` 是 Worker 的版本化根响应，互斥表达已接受任务的 `TaskResult`、请求拒绝和 Host 故障。
响应与内部结果必须保持 job/trace 关联一致。传输编码和进程退出码属于 `apps/worker`，不进入契约实现。

`WorkerCommand` 与 `WorkerEvent` 定义双向会话契约。首阶段 Command 只支持关联当前 job/trace 的 Cancel；
Event 互斥表达 `TaskProgress` 或最终 `WorkerResponse`。协议要求零到多个 Progress，随后至多一个 Result；
具体 framing、JSON 和 Named Pipe 不进入 Contracts。长期决策见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

## 测试

- 每个消息的必填字段、版本和枚举验证。
- Outcome、ErrorCode 与 warning 集合的组合不变量。
- 编码 roundtrip、未知可选字段和损坏输入。
- Secret 不出现在日志/调试输出。
- Golden message 固定字段名和数值。

## 完成标准

- 契约不泄漏传输、平台或持久化实现。
- 版本行为和拒绝规则有测试与文档。
- 所有消费者只依赖稳定 DTO，不解析另一进程的内部结构。
