# `pipeline` 模块开发文档

## 目标

提供物理机、虚拟机和内存源共享的 Backup/Restore 数据面。

## Backup Pipeline

```text
Snapshot Session -> Block Source -> Extent Enumerator -> Chunker
-> Hash/Dedup Query -> Compress -> Encrypt -> Backup Session -> Commit
```

职责：有界并发、背压、Chunk 边界、逻辑映射、转换、校验、进度、取消、重试分类和提交/中止。

Block Source 可通过 `describe_extent()` 报告 DATA/FREE。Pipeline 只为 DATA extent 调用 `read()`；FREE
区间在 chunk descriptor 中保留，逻辑进度照常推进，但不把该区间当作全零数据读取或散列。

`BackupSummary` 提供 producer 总读取耗时及 payload 分配/清零、extent 查询、实际 Block Source 读取的
分项耗时，并提供源读取字节、FREE 字节及调用次数。统计只在 Chunk、extent 和 Block Source 调用边界
采样，避免在内部固定块循环中增加高频时钟开销。

Volume Backup 按 `memory_budget_bytes / chunk_size_bytes` 建立有界 Chunk buffer pool。每个底层 storage
额外保留最多 64 KiB - 1 的对齐余量，并向 Block Source 暴露 64 KiB 对齐的有效 payload span，使 Windows
无缓存卷读取可直接填充 Pipeline buffer。Consumer 在 `IBackupSession::write_chunk` 返回后立即归还整个
storage，Producer 后续读取复用其容量；FREE 字节保持未定义旧内容，但仍由 descriptor 排除，不会被读取、
散列或持久化。最后一个短 Chunk 只调整有效 span，不释放 capacity。该 pool 对 payload 字节保持原有预算，
每个 buffer 的固定对齐余量不计入 payload budget。

## Restore Pipeline

```text
Recovery Point Reader -> Manifest Validation -> Chunk Resolver
-> Fetch -> Decrypt -> Decompress -> Logical Stream -> Block Sink -> Flush
```

职责：目标预检、顺序与随机读取、数据重建、完整性验证、可恢复检查点和错误分类。

Restore 预检验证每个 chunk 的 FREE 区间有序、不重叠且不越界。认证并展开 chunk 后，仅把 FREE 的补集
写入 `IBlockSink`；FREE 区间直接跳过，不清零、不覆盖目标盘原内容。`RestoreSummary.restored_bytes`
表示完成处理的逻辑字节，`disk_written_bytes` 表示实际提交给 Sink 的字节，`free_skipped_bytes` 和
`free_range_count` 分别表示跳过的 FREE 字节与区间数。

`RestorePlan.logical_write_limit_bytes`（ADR-0025）：

- `0`（默认）：完整恢复；要求源逻辑大小 ≤ Sink 真实容量。
- 非 0：前缀恢复；仍完整校验全部 descriptor / FREE / logical end，但只向 Sink 写入
  `[0, logical_write_limit_bytes)`，且该上限必须 ≤ Sink 容量。不得用虚报容量的 Sink 绕过此字段。

`ProtectedRangeBlockSink`：在真实 Sink 外包一层，跳过与受保护半开区间相交的写入（Primary/Backup Boot
等），供缩容前缀恢复使用。

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

- 备份：分页枚举 → change planner（FI3）→ 仅 local stream 按 block 读内容写 chunk → 写完整 tip
  entry Index → finalize/commit。Adapter 在 entry 上填充 `platform_metadata`（含 security）；
  Pipeline 原样写入 index。
  - **Full**：全部主数据流 `content_storage=local`。
  - **Incremental**（`effective_type=incremental` + parent reader + metadata baseline）：完整枚举 current
    namespace，构建有界 parent path 索引（紧凑排序向量/磁盘 spool，非全量 `FileEntryDesc` map），同路径普通
    文件按 `write_time + logical_size` 决定 local 整文件或 direct-parent stream；无法验证父 stream 时 local；
    tip Index 仍是完整当前树。
- 恢复：只依赖 `IFileRecoveryPointReader`（composition 注入 chain reader）；选择闭包（目录 seed
  展开全部可达后代 + 路径祖先）→ capability 预检 → 按深度建目录骨架 → 文件 staging/publish →
  目录 metadata。`entry_ids` 是 seed，不是最终写出集合。parent stream 由 reader 解析，Pipeline
  不感知链层。
  文件/目录始终应用时间戳与属性；仅当 `FileSetRestorePlan.restore_security=true` 时向 Sink
  传递 `platform_metadata` 中的 security descriptor。
  capability 预检同时比较每个普通文件和 `maximum_file_size_bytes`；FAT32 超过 4 GiB - 1 时在写前返回
  `file_restore.target_file_too_large`。
  祖先遍历与 `path_for_entry` 带 visited/depth 上限，环或超深返回 `format.corrupt_index`。
  Partial `stable_error_codes` 去重且 ≤ `kMaximumPartialRestoreErrorCodes`（64）；目录 metadata
  失败会回退 `entries_restored` 并计入 `entries_failed`（仅对已成功 create 的目录）。
  **进度：** 选择闭包完成后以所选普通文件逻辑字节之和作为 `TaskProgress.logical_bytes`；
  写目录骨架、每个 stream quantum 与每个文件 publish 后发布 `kWriting` 进度；`kCompleted`
  将 `processed_bytes` 对齐到 `logical_bytes`（满足 contracts，Desktop 显示 100%）。
  目录-only 选择时 `logical_bytes` 为空，UI 仍可能在成功态由 Service 投影为 100%。
- 不 include personal_archive 实现类、Windows filesystem 或 VSS；只依赖 ports + format 常量。

## 当前状态

阶段 2 已实现 fixed-size raw Chunk、Memory Adapter、Backup/Restore Pipeline、按字节预算的有界队列、取消、Commit/Abort、Restore 预检和内存 roundtrip。

阶段 3 已完成第一条纵向切片：同一 Pipeline 可把 Memory Block Source 写入一个正式加密 metadata、逐块 Zstandard 压缩、Footer 完成标记的单 volume `.bkf`，再通过 `IRecoveryPointReader` 还原到 Block Sink。Pipeline 没有新增对具体 Adapter 的依赖。ADR-0022 的个人版单 Chunk 去重由 Personal Archive Session 在 Adapter 内完成；Pipeline 只传递逻辑块与统计结果，不 include DEDUP 格式。企业 Repository 的跨备份去重仍由其 CAS/Transform 组合承担。

阶段 6 的个人增量实现仍复用同一 Backup Pipeline 读取完整源，由 Archive Session 在 Adapter 内形成稀疏变化层；恢复侧由 Chain Reader 先合并为连续视图，再交给原 Restore Pipeline。Pipeline 不知道父 UUID、Sidecar 或个人备份链。

F3 已实现 `FileSetBackupPipeline` / `FileSetRestorePipeline`。背压队列的 producer/consumer 线程模型与
file_set Backup Pipeline 向 Session 提交**逻辑** stream block；压缩（zstd）在
`IFileBackupSession` 适配器内完成，Pipeline 用返回的 stored 字节累计进度，不依赖具体压缩库。

百万级 multi-leaf 优化在 F5 纵向切片继续加强；当前实现为可取消的顺序数据面。F10 将 file_set 首版
Pipeline 路径纳入发布门禁（与 volume Pipeline 并存；无兼容分支）。

## 验证

构建 Pipeline 及其生产消费者，审查空源、尾部短块、零块、取消、读写失败、容量不足、Commit/Abort、
背压和数据 roundtrip 路径；必要的人工验证使用隔离的内存或非生产数据源，不访问客户磁盘或生产网络。
