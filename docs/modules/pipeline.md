# `pipeline` 模块开发文档

## 目标

提供物理机、虚拟机和内存测试源共享的 Backup/Restore 数据面。

## Backup Pipeline

```text
Snapshot Session -> Block Source -> Extent Enumerator -> Chunker
-> Hash/Dedup Query -> Compress -> Encrypt -> Backup Session -> Commit
```

职责：有界并发、背压、Chunk 边界、逻辑映射、转换、校验、进度、取消、重试分类和提交/中止。

## Restore Pipeline

```text
Recovery Point Reader -> Manifest Validation -> Chunk Resolver
-> Fetch -> Decrypt -> Decompress -> Logical Stream -> Block Sink -> Flush
```

职责：目标预检、顺序与随机读取、数据重建、完整性验证、可恢复检查点和错误分类。

## 依赖

允许依赖 `base`、`contracts`、`ports`、`format`。禁止依赖 Adapter、Storage Factory、VSS、PostgreSQL、Dokan、VDDK 和 UI。

## 关键接口

- `BackupPlan`/`RestorePlan`：不可变执行输入。
- `IChunker`：只定义边界策略。
- `IChunkTransform`：压缩、加密等可组合转换。
- `IBackupSession`：写 Chunk、Commit、Abort。
- `IRecoveryPointReader`：Manifest 与批量 Chunk 解析。

## 不变量

- 队列有界；内存预算可配置并可观测。
- 取消在有界时间生效；资源由 RAII 清理。
- Commit 前失败必须 Abort，且不会产生可见 Recovery Point。
- Restore 在第一次破坏性写入前完成源、目标、容量和映射验证。
- 相同逻辑输入和策略产生确定的 Manifest/Chunk 顺序。

## 阶段 2 实现范围

1. Memory Block Source/Sink。
2. 顺序 fixed-size Chunker。
3. 无压缩、无加密的 Memory Backup Session。
4. 单线程 Backup Pipeline 与错误注入测试。
5. Restore Pipeline roundtrip。
6. 有界并发、背压和取消。

阶段 2 的队列按 payload 字节数限制内存，而不是只限制元素数量。Restore 必须先读取并验证全部 descriptor、逻辑范围和目标容量，完成预检后才能启动 payload 读取和目标写入。

真实 transform、通用 Manifest 和个人 `.bkf` Session 属于阶段 3，不属于阶段 2。

## 当前状态

阶段 2 已实现 fixed-size raw Chunk、Memory Adapter、Backup/Restore Pipeline、按字节预算的有界队列、取消、Commit/Abort、Restore 预检和内存 roundtrip。阶段 3 将在不改变 Pipeline 依赖方向的前提下接入 Transform 与格式 Session。

## 测试

覆盖空源、尾部短块、零块、取消、读写失败、容量不足、Commit/Abort、背压和数据 roundtrip。测试不得访问真实磁盘或网络。
