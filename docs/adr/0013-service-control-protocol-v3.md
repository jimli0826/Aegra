# ADR-0013：本地 Service 控制面协议 V3

- 状态：Accepted
- 日期：2026-08-03
- 决策者：Aegra 项目
- 关联模块：contracts、application、apps/service、apps/desktop
- 替代范围：ADR-0011 与 ADR-0012 中的 Service wire schema；传输与 Repository 权威决策不变

## 背景

Service schema 2 只支持 `GetServiceInfo` 和 `ListRecoveryPoints`，根消息通过多个 nullable 字段表达 payload。
后续个人版需要 Repository 连接、Source Inventory、任务、计划、审计、Restore、Verify、Mount 和 task event，
同时必须保持 Desktop 不直接访问数据库、Repository、Worker 或 Windows 数据面。

协议需要在业务实现开始前确定 envelope、类型、幂等、分页、事件恢复和背压语义。产品尚未发布，因此直接
替换 schema 2，不维护双协议、字段别名或迁移窗口。

## 决策

### 1. 传输与版本

Named Pipe、4 字节 little-endian 长度前缀、UTF-8 JSON 和 64 KiB 最大 frame 保持 ADR-0011 的决定。
所有 V3 根消息携带 `schema_version=3` 和 `message_type`：`1=Request`、`2=Response`、`3=Event`。
每个对象严格校验字段集合、JSON 类型、整数范围、枚举和 payload 联合；未知关键字段或 kind 直接拒绝。

`GetServiceInfo` 请求携带客户端接受的 `minimum_api_version` 与 `maximum_api_version`。Service 选择唯一当前
`api_version=3`；范围不相交时返回 `UnsupportedVersion` 和 `service.api_version_unsupported`。响应同时返回
`minimum_api_version`，后续只有显式设计兼容范围时才允许一个 Service 支持多个 API 版本。

### 2. Request envelope

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "<correlation-id>",
  "kind": 1,
  "idempotency_key": null,
  "payload": {
    "minimum_api_version": 3,
    "maximum_api_version": 3
  }
}
```

查询不得携带 `idempotency_key`。所有命令必须携带不超过 128 字节的稳定幂等键；Service 控制面最终需要在
命令状态提交的同一事务中保存键、请求摘要和结果引用。相同键与相同请求返回 `Replayed` acknowledgement；
相同键与不同请求返回 Conflict。`request_id` 只做当前会话响应对账，不提供幂等或授权保证。

稳定 request kind 为：

| 数值 | Query | 数值 | Command |
| ---: | --- | ---: | --- |
| 1 | GetServiceInfo | 32 | AddRepositoryConnection |
| 2 | ListRecoveryPoints | 33 | ImportRepositoryConnection |
| 3 | ListRepositoryConnections | 34 | TestRepositoryConnection |
| 4 | ListSourceInventory | 35 | SetDefaultRepository |
| 5 | ListJobs | 36 | RemoveRepositoryConnection |
| 6 | ListSchedules | 37 | StartBackup |
| 7 | ListEvents | 38 | CancelJob |
| 8 | ListMountSessions | 39 | StartVerify |
| 9 | PrepareRestore | 40 | StartRestore |
| 10 | ResolveRecoveryPointChain | 41 | MountRecoveryPoint |
| 11 | PlanDeleteRecoveryPoints | 42 | UnmountSession |
|  |  | 43 | UpsertSchedule |
|  |  | 44 | DeleteSchedule |
|  |  | 45 | SubscribeTaskEvents |
|  |  | 46 | AcknowledgeEvents |
|  |  | 47 | ExecuteDeletePlan |

`StartVerify`（39）payload 为 `StartVerifyCommand{repository_connection_id, recovery_point_id}`，
不再使用仅含 `resource_id` 的 `ResourceRef`（产品未发布，直接替换）。

相同形状不表示相同权限。Service dispatcher 必须按 request kind 分别检查 capability、身份、资源归属和业务
前置条件，不能仅按 payload 类型授权。

### 3. Response envelope

```json
{
  "schema_version": 3,
  "message_type": 2,
  "request_id": "<correlation-id>",
  "kind": 1,
  "request_kind": 2,
  "boundary_error_code": 0,
  "message_code": "repository.catalog_ready",
  "message_arguments": [],
  "payload": {
    "repository_connection_id": null,
    "catalog": {
      "state": 2,
      "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
      "items": [],
      "continuation_token": null
    }
  }
}
```

Response kind 为 `1=QueryResult`、`2=CommandAccepted`、`3=RequestFailed`。`request_kind` 固定记录所响应的
操作，客户端同时校验 `request_id`、`request_kind` 与强类型 payload。成功响应的错误码必须为 None；失败
响应必须有非 None 稳定错误码且 `payload=null`。

命令成功只表示控制面已接受或重放，不表示数据面完成。`CommandAcknowledgement` 返回稳定 `command_id`、
`Accepted/Replayed` disposition 和可选 resource ID。Backup、Verify、Restore 等长任务的完成状态通过 Job
查询和 task event 获得。

Service 只返回稳定 `message_code` 和按名称排序的结构化 `message_arguments`。不返回本地化文本、异常文本、
日志内容、明文秘密或未脱敏客户数据。Desktop 负责把 code 映射为 Qt 翻译 ID。

### 4. 领域 DTO

V3 Contracts 定义以下传输无关 DTO，JSON 只存在于 `apps/service` 和 Desktop 私有适配层：

- Repository connection summary/input，不返回 Repository 根路径；credential 为可选 `SecretRef`。
- Source Inventory，使用 Service 生成的 opaque source ID，不接受 Desktop 伪造设备路径。
- Job、TaskProgress、TaskResult 和 queued/running/cancelling/terminal 状态。
- Daily/Weekly Schedule、IANA/Windows timezone ID、next-run 投影。
- Audit Event severity、稳定 code、参数和 correlation ID。
- Restore preflight token、Recovery Point/target ID、链深度、容量和过期时间。
- Mount Session 与 Service 生成的挂载点；Desktop 不直接调用 Dokan。
- Event subscription lease、resume token、sequence 和 acknowledgement。

每类列表拥有自己的强类型查询条件：Repository state、是否包含 unavailable source、Job operation/state、
Schedule enabled、Audit severity/time/correlation 和 Mount state。Recovery Point 查询与响应都携带可选
Repository connection ID；当前单 Repository 启动参数模式使用 null，S4 接入多连接后使用稳定 connection ID。

所有传给 Qt 的时间、容量、计数和 sequence 必须处于非负有符号 64 位范围。跨进程只传 `SecretRef`；
Repository connection 不需要凭据时使用 null，不用空字符串代替。

### 5. 分页

所有列表请求最多 100 项，使用最大 1,024 字节的不透明 continuation token。调用方不得解析、拼接或从其他
query 复用 token。响应 token 只对相同 caller、request kind、filter 和排序有效；Service 实现必须保证稳定
排序。64 KiB frame 仍是最终上限，达到上限时 Service 可以返回更少项目。

Recovery Point 继续使用 ADR-0012 的 Catalog 语义：`catalog_ready` 不表示 Archive 已认证或 Restore Ready。
Recovery Point、Manifest、Chunk Index 和 Archive metadata 不进入控制面数据库成为权威副本。

### 6. Task event、恢复与背压

`SubscribeTaskEvents` 是幂等命令。首次订阅传 `resume_token=null, after_sequence=0`；acknowledgement 必须返回
subscription ID、不可解析的 resume token、下一 sequence 和协商后的未确认窗口。重连恢复同时提交原 resume
token 和最后已处理 sequence。

Event envelope 固定包含 subscription ID、从 1 开始单调递增的 sequence、event kind、message code、参数和
强类型 payload。首批 event kind 为 TaskProgress、TaskCompleted 和 MountSessionChanged。

客户端通过 `AcknowledgeEvents` 确认连续处理到的 sequence。窗口最大 128，默认 64。Service 不允许无限缓存；
达到窗口后暂停该 subscription 的事件发送。resume token 过期、序号不可恢复或客户端长期不确认时，Service
返回稳定错误并关闭订阅，客户端必须重新查询 Job/Mount 权威状态后建立新订阅。事件不是任务权威存储。

当前串行 Host 只发送 request/response；S3 Worker Supervisor 引入受控的异步 event writer 前，Service 不声明
`task.events` capability，也不接受订阅为成功。

### 7. 当前实现能力

S0 完成时 Service 只声明 `service.info` 和 `repository.list`。当前 S1-S4 已扩展到 Inventory、Repository
connection/query、Job list、Backup start 与 Job cancel。S5 的 chain/delete/verify handler 已接线但尚未满足
完整工作包门禁，因此 runtime 不声明对应 capability；dispatcher 必须按 request kind 在 handler 调用前检查
capability，返回 Conflict 与 `service.capability_unavailable` 且不产生副作用。后续 S5-S8 只有完成各自用例和
验收门禁后才能增加 capability。

## 备选方案

- 扩展 schema 2 的多个 nullable 根字段：随着领域增加会形成巨大稀疏对象，联合关系难以验证，不采用。
- 使用一个通用 action 字符串和任意 JSON payload：无法在 Contracts 层验证权限相关结构，不采用。
- 为每个页面建立独立 Pipe 或 WebSocket：增加 ACL、生命周期和重连面，不采用。
- 事件无限 push、不要求 acknowledgement：慢客户端会导致 Service 无界缓存或丢失不可检测，不采用。
- 现在实现 schema 2/3 双栈：产品未发布，增加测试矩阵而无用户价值，不采用。

## 影响

- S2-S8 可以在不改变 envelope 的情况下逐项实现 handler、持久化和 supervisor。
- Desktop 必须先通过 ServiceInfo capability 决定页面与动作，不得依据已知 request kind 猜测能力存在。
- 严格 schema 会拒绝未知字段；后续新增字段需要新 schema 或预先定义的可选字段。
- task event 需要 S3 把当前串行 session 扩展为单写者异步发送模型，并保持 response/event framing 顺序。

## 详细 wire 说明

逐条 request/response/event 字段、枚举与示例 JSON 见权威说明文档：
[本地 Service 控制面协议 V3](../protocol/SERVICE_CONTROL_PROTOCOL_V3.md)。
本 ADR 记录决策与边界；字段级编解码以该文档与 codec 实现同步维护。

## 验证

- Contracts 测试覆盖 request/response/event envelope、kind/payload 配对、幂等键、版本范围和消息参数。
- Service codec 测试覆盖全部 request payload 和 query response payload variant、command acknowledgement、
  三类 event、golden JSON、未知字段、错误类型、超限与损坏输入。
- Host 测试验证当前两个 capability，合法但未实现的请求返回无副作用结构化失败。
- 真实 Service 进程和 Desktop Client 测试使用 V3 完成握手、Repository 分页、断线与重连。
- 架构检查保证 Contracts 不依赖 JSON、Qt、Win32 或数据库，Qt 只存在于 Desktop。
