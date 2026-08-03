# `ports` 模块开发文档

## 目标

以最小能力接口隔离外部 I/O、系统能力和可替换策略，使核心数据面可用内存实现进行确定性测试。

## 端口集合

- 数据面：`IBlockSource`、`IBlockSink`、`ISequentialWriter`、`IRandomAccessReader`。
- Repository Storage：`IObjectReader`、`IStagedObjectWriter`、`IPrefixEnumerator`、
  `IObjectPublisher`、`IObjectDeleter`。
- 生命周期：`ISnapshotSession`、`IBackupSession`、`IRecoveryPointReader`。
- 系统能力：`IClock`、`IProgressSink`、`ICredentialResolver`、`IRandomSource`。
- 进程传输：`IMessageChannel`。

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

## Worker 系统能力

- `ICredentialResolver::resolve()` 接受 `SecretRef` 和取消令牌，返回独占的 `IResolvedSecret`；Secret
  view 只在对象生命周期内有效，Resolver 实现负责受控内存与析构清零。
- `IRandomSource::fill()` 填充调用方提供的缓冲区并支持取消，不暴露随机库或操作系统类型。
- `IClock::now_utc_ms()` 提供 UTC 毫秒时间；测试必须注入确定性时钟。
- `IProgressSink::publish()` 不得抛出异常；事件拥有 job/trace 关联字段且不得包含 Secret 或客户数据。
- `IMessageChannel` 传递拥有所有权的 UTF-8 消息；一个 Reader 和一个 Writer可以并发，挂起 I/O 必须
  响应取消，Adapter 必须执行帧大小限制。消息 schema 与状态机不属于 Port。

## 多数据源演进方向

现有 `IBackupSession` 和 `IRecoveryPointReader` 表达一个连续逻辑 source，不把多个 volume 拼接成一个
全局 offset 空间。按 [ADR-0003](../adr/0003-personal-split-archive-and-multi-source-boundary.md)，
后续真实多 volume use case 使用 Backup Set/Source 两级生命周期：Set Session 统一提交，Source Writer
保持现有连续 chunk 语义；读取侧由 Recovery Point Set Reader 枚举并打开 source-scoped Reader。
在第二个真实 source 消费者出现前不加入空接口。

## 个人版 Repository Storage 能力

个人版受管理 Archive Store 的对象能力定义在 `object_storage.h`，按真实用例拆分为 `IObjectReader`、
`IStagedObjectWriter`、`IPrefixEnumerator`、`IObjectPublisher`、`IObjectDeleter` 和独立 capability Port，
不提供万能 Storage Backend。

- `IObjectReader` 返回 key、对象大小和 opaque generation；范围读取允许短读，到达对象末尾返回 `0`。
- `IStagedObjectWriter` 返回单调用方 RAII Session；`write()` 在返回前消费或复制输入，未成功 `complete()`
  的 Session 析构时中止。
- `IPrefixEnumerator` 使用 opaque continuation token 分页；token 只可与产生它的 prefix 一起使用。
- `IObjectPublisher` 只发布已经完成的 staging 对象，支持 create-only 或 generation 匹配替换。成功返回新的
  generation；`kOutcomeUnknown` 表示 mutation 可能已完成，必须先通过 Reader 对账。
- `IObjectDeleter` 以 operation ID 和 key 形成幂等重试身份；对象不存在视为成功，同一身份改变 expected
  generation 返回冲突。
- capability 明确报告原子 rename publish、条件创建、强 read-after-write 和强 list consistency，消费者
  不得从 URI 或 Adapter 名称推断。

所有 key 都是 Repository 相对 key。Capability 对象允许并发调用；单个 Staged Write Session 不是线程
安全对象。所有操作支持取消，已进入外部单对象 mutation 后不能伪报取消，必须成功或返回
`kOutcomeUnknown`。详细语义见 [个人版 Repository 模块](personal_repository.md)。

## 测试

每个 Port 提供可复用 Contract Test Suite。所有 Adapter 必须运行同一组边界、短读、取消、并发、错误注入和资源释放测试。

阶段 12B 已提供 `tests/ports/object_storage_contract.h`，覆盖 staging 可见性、条件发布、generation、分页、
取消和幂等删除。新增 Storage Adapter 必须复用该套件。
