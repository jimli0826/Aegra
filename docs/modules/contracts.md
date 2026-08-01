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
- Job 至少包含 job、tenant、operation、source、target、credential refs、trace 和 deadline。

## 当前状态

阶段 2 已将跨进程 `TaskProgress` DTO 放入本模块，`IProgressSink` 只引用该 DTO。`JobRequest` 仍是最小骨架；进入进程协议实现前需要引入强类型 ID。

## 测试

- 每个消息的必填字段、版本和枚举验证。
- 编码 roundtrip、未知可选字段和损坏输入。
- Secret 不出现在日志/调试输出。
- Golden message 固定字段名和数值。

## 完成标准

- 契约不泄漏传输、平台或持久化实现。
- 版本行为和拒绝规则有测试与文档。
- 所有消费者只依赖稳定 DTO，不解析另一进程的内部结构。
