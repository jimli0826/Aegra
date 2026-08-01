# `ports` 模块开发文档

## 目标

以最小能力接口隔离外部 I/O、系统能力和可替换策略，使核心数据面可用内存实现进行确定性测试。

## 端口集合

- 数据面：`IBlockSource`、`IBlockSink`、`ISequentialWriter`、`IRandomAccessReader`。
- 对象存储：`IObjectReader`、`IObjectWriter`、`IListableObjectStore`、`IMutableObjectStore`。
- 生命周期：`ISnapshotSession`、`IBackupSession`、`IRecoveryPointReader`。
- 系统能力：`IClock`、`IProgressSink`、`ICredentialResolver`、`IRandomSource`。

## 依赖

只依赖 `base` 和必要的 `contracts`。接口文件不得 include 具体 Adapter。

## `IBlockSource` 语义

- `size_bytes()` 在源会话生命周期内稳定。
- `read(offset, buffer)` 返回实际字节数；到达 EOF 可以短读，越界起始偏移返回错误。
- 空 buffer 成功返回 0。
- 实现必须检查 `offset + size` 溢出并响应取消。
- 并发读取能力通过显式 capability 表达，不由调用方猜测。

## `IBlockSink` 语义

- 写入前验证目标容量。
- 成功写入必须覆盖完整输入；不允许无提示短写。
- `flush()` 只保证本端口定义的持久性边界，具体 durability 写入接口文档。
- 重试安全性和幂等键由更高层 Session 定义。

## 接口设计规则

- 不创建万能 Storage Backend 或 Hypervisor Connector。
- 所有权和线程安全写在接口注释中。
- 长操作接受取消令牌。
- 返回统一 `Result<T>`，不让第三方错误或异常穿过边界。
- `ChunkWriteRequest.payload` 是调用期只读视图；Session 必须在返回前消费或复制。
- `IRecoveryPointReader` 生命周期内 logical size、chunk count 和 descriptor 必须稳定。

## 阶段 2 已实现契约

- `IBlockSource`、`IBlockSink`。
- `IBackupSession`、`IRecoveryPointReader`。
- `ChunkDescriptor`、调用期 `ChunkWriteRequest` 和拥有数据的 `ChunkData`。
- 引用 `contracts::TaskProgress` 的 `IProgressSink`。

## 测试

每个 Port 提供可复用 Contract Test Suite。所有 Adapter 必须运行同一组边界、短读、取消、并发、错误注入和资源释放测试。
