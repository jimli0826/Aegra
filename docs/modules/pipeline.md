# `pipeline` 模块开发文档

## 目标

提供物理机、虚拟机和内存源共享的 Backup/Restore 数据面。

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

多 Volume 备份：`BackupPlan` 可携带 `progress_total_logical_bytes` 与
`progress_base_*`，进度按**整 job** 逻辑字节聚合；仅 `commit_mode=kCommit`（最后一卷）
发布 `TaskPhase::kCompleted`，中间卷 `kDefer` 只保持 `kWriting` 且不 `session.commit`。
累计使用 checked-add，且 `base_processed + volume_size ≤ total`；字节计数不得超过 Service
有符号 64 位 wire 上限（`INT64_MAX`），溢出则失败，不发布非法进度。

## 不变量

- 队列有界；内存预算可配置并可观测。
- 取消在有界时间生效；资源由 RAII 清理。
- Commit 前失败必须 Abort，且不会产生可见 Recovery Point。
- 中间 Volume（`kDefer`）不得发布 `kCompleted`，也不得单独 Commit。
- Restore 在第一次破坏性写入前完成源、目标、容量和映射验证。
- 相同逻辑输入和策略产生确定的 Manifest/Chunk 顺序。

## 阶段 2 实现范围

1. Memory Block Source/Sink。
2. 顺序 fixed-size Chunker。
3. 无压缩、无加密的 Memory Backup Session。
4. 单线程 Backup Pipeline 与错误注入能力。
5. Restore Pipeline roundtrip。
6. 有界并发、背压和取消。

阶段 2 的队列按 payload 字节数限制内存，而不是只限制元素数量。Restore 必须先读取并验证全部 descriptor、逻辑范围和目标容量，完成预检后才能启动 payload 读取和目标写入。

真实 transform、通用 Manifest 和个人 `.bkf` Session 属于阶段 3，不属于阶段 2。

## FileSet Pipeline（F3）

```text
IFileSnapshotView -> FileSetBackupPipeline -> IFileBackupSession (finalize/commit)
IFileRecoveryPointReader -> FileSetRestorePipeline -> IFileTreeSink
```

- 备份：分页枚举 → 计划 stream/hard-link → 按 block 读内容写 chunk → 写 entry spool → finalize/commit。
  Adapter 在 entry 上填充 `platform_metadata`（含 security）；Pipeline 原样写入 index。
- 恢复：选择闭包（目录 seed 展开全部可达后代 + 路径祖先）→ capability 预检 → 按深度建目录骨架 →
  文件 staging/publish → reparse → 目录 metadata。`entry_ids` 是 seed，不是最终写出集合。
  文件/目录始终应用时间戳与属性；仅当 `FileSetRestorePlan.restore_security=true` 时向 Sink
  传递 `platform_metadata` 中的 security descriptor。
  祖先遍历与 `path_for_entry` 带 visited/depth 上限，环或超深返回 `format.corrupt_index`。
  Partial `stable_error_codes` 去重且 ≤ `kMaximumPartialRestoreErrorCodes`（64）；目录 metadata
  失败会回退 `entries_restored` 并计入 `entries_failed`（仅对已成功 create 的目录）。
- 不 include personal_archive 实现类、Windows filesystem 或 VSS；只依赖 ports + format 常量。

## 当前状态

阶段 2 已实现 fixed-size raw Chunk、Memory Adapter、Backup/Restore Pipeline、按字节预算的有界队列、取消、Commit/Abort、Restore 预检和内存 roundtrip。

阶段 3 已完成第一条纵向切片：同一 Pipeline 可把 Memory Block Source 写入一个正式加密 metadata、逐块 Zstandard 压缩、Footer 完成标记的单 volume `.bkf`，再通过 `IRecoveryPointReader` 还原到 Block Sink。Pipeline 没有新增对具体 Adapter 的依赖。后续 Transform 组合接口将用于企业 Repository、payload 加密和去重，不把个人格式判断加入 Pipeline。

阶段 6 的个人增量实现仍复用同一 Backup Pipeline 读取完整源，由 Archive Session 在 Adapter 内形成稀疏变化层；恢复侧由 Chain Reader 先合并为连续视图，再交给原 Restore Pipeline。Pipeline 不知道父 UUID、Sidecar 或个人备份链。

F3 已实现 `FileSetBackupPipeline` / `FileSetRestorePipeline`。背压队列的 producer/consumer 线程模型与
百万级 multi-leaf 优化在 F5 纵向切片继续加强；当前实现为可取消的顺序数据面。F10 将 file_set 首版
Pipeline 路径纳入发布门禁（与 volume Pipeline 并存；无兼容分支）。

## 验证

构建 Pipeline 及其生产消费者，审查空源、尾部短块、零块、取消、读写失败、容量不足、Commit/Abort、
背压和数据 roundtrip 路径；必要的人工验证使用隔离的内存或非生产数据源，不访问客户磁盘或生产网络。
