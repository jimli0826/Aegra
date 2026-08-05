# `application` 模块开发文档

## 目标

按 Use Case 组织业务编排，连接控制面意图与数据面能力，不包含 HTTP、GUI、SQL 或厂商 SDK 代码。

## 推荐 Use Case

- `StartPhysicalBackup`、`StartVirtualMachineBackup`。
- `StartPhysicalRestore`、`StartVirtualMachineRestore`。
- `PreparePeRestore`、`MountRecoveryPoint`、`ExportRecoveryPoint`。
- `CreateSchedule`、`CancelTask`、`DeleteRecoveryPoint`。
- `RunRepositoryGc`、`RunRepositoryScrub`、`RebuildRepositoryIndex`。

## 依赖

允许依赖 `contracts`、`ports`、`pipeline` 和 Repository Client 抽象。不得依赖 HTTP Request、Qt 类型、PostgreSQL API 或具体 Adapter。

## Use Case 结构

每个 Use Case 定义输入、输出、权限、幂等键、依赖端口和事务边界。处理顺序为：授权前置条件、输入校验、加载必要状态、调用能力、记录结果/Outbox。

## 个人版与企业版

- 个人版可以由本地 Service 组合 SQLite 查询缓存和 `.bkf` Session。
- 个人版 Recovery Point 发现、显式链解析和删除计划通过 `personal_repository`；Application 负责解析
  每层 CredentialRef、用户确认、保留策略和任务编排。
- 企业版由 Management Service 维护 PostgreSQL 控制面，并通过 Repository Client 调用 Gateway。
- Application 不通过数据库行判断 Recovery Point 是否已提交。

## 任务规则

- Job 和 Progress 使用版本化 `contracts`。
- Worker 可重试操作必须携带 transaction ID 与幂等键。
- 密钥字段使用 `SecretRef`，由入口或 Adapter 解析。
- 取消和 deadline 必须贯穿到 Pipeline、Connector 和 Repository Client。

## 验证

构建 Application 及其直接消费者，并审查权限、调用顺序、幂等、取消、超时、错误映射和 Outbox 边界；高风险流程按需执行聚焦的人工运行验证。

## 当前状态

阶段 13B 已建立 `Aegra::Application` Target，并实现 `IPersonalRepositoryQuery` 与
`PersonalRepositoryQuery`。未配置 Repository 时返回合法空页；配置后通过 `RepositoryCatalogScanner`
分页读取并映射为 Contracts，输入、输出和取消均在 Use Case 边界校验。Application 不知道本地路径、Qt、
JSON 或具体 Storage Adapter。

S4 已增加：

- `SourceInventoryQuery`：通过 `ISourceInventory` 分页返回 opaque `source_id`，不向 Desktop 暴露设备路径；
- `RepositoryConnectionService`：add/import/test/set-default/list/remove；Add 只接受缺失或空目标，生成随机
  Repository UUID，暂存并条件发布 `aegra.repository` 后才持久化 Available 连接；Import 只接受已有有效
  Descriptor 的 Repository。控制面仅持久化 locator 与 `SecretRef`，删除只移除控制面引用；
- `ConnectedRepositoryQuery`：按 connection id 或 default 连接打开 `IRepositoryStorageFactory`，
  扫描 Catalog 并返回 `ServiceRecoveryPointPage`。

上述用例依赖 `IControlPlaneDatabase`、`IRepositoryStorageFactory`、`ISourceInventory` 与 `IClock`，现已由
Service composition root 注入。Repository command 使用持久化幂等记录；同键同请求 replay，同键不同请求
返回冲突。连接测试只把可用性写入控制面，不把 Catalog 或 Archive metadata 复制为权威数据。

S5 增加 `RecoveryPointOperations`：按 connection 打开 Repository、扫描 Catalog、构建
`RecoveryPointGraph`、返回 base-first 链摘要；生成 descendant-first 删除计划（plan token 持久化在
Repository `staging/delete-plans/`），计划持久化每个 Archive member 的 Storage generation，并按
tombstone 协议执行条件删除，防止崩溃续作误删同 key 的新对象。Verify 由 Service
`WorkerJobService::start_verify` 构造受信任 Archive 路径后提交 Supervisor。

S6 等待前置 S5：`RestorePreflightService` 已建立 Application 编排边界，按
`repository_connection_id + recovery_point_id + target_source_id` 查找受信任资源，验证目标为可用、非系统、
非只读 Volume 且容量足够，生成默认 5 分钟 opaque token，并把 Repository UUID、完整链指纹、容量和链深写入
durable control plane。`IRestoreChainInspector` 是 S5 真实逐层认证能力的待组合边界；在该实现和 Start 的 TOCTOU
重验证、Job 唯一占用及 Worker 接线完成前，Service 不构造本用例且 `restore.preflight` / `restore.start`
capability 保持关闭。
