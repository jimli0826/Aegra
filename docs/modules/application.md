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
- 企业版由 Management Service 维护 PostgreSQL 控制面，并通过 Repository Client 调用 Gateway。
- Application 不通过数据库行判断 Recovery Point 是否已提交。

## 任务规则

- Job 和 Progress 使用版本化 `contracts`。
- Worker 可重试操作必须携带 transaction ID 与幂等键。
- 密钥字段使用 `SecretRef`，由入口或 Adapter 解析。
- 取消和 deadline 必须贯穿到 Pipeline、Connector 和 Repository Client。

## 测试

使用 Fake Port 验证权限、调用顺序、幂等、取消、超时、错误映射和 Outbox。Controller/API 测试与 Use Case 单元测试分离。
