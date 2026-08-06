# ADR-0003：个人版分卷事务与多数据源边界

- 状态：Accepted
- 日期：2026-08-01
- 决策者：Aegra 项目
- 关联模块：ports、pipeline、format、adapters/personal_archive

## 背景

V6 允许一个逻辑备份跨多个 `.bkf` 分卷，也允许 Manifest 描述多个 volume。两者属于不同维度：
分卷是一个 Archive 的物理承载方式，多 volume 是一个 Recovery Point 包含多个独立逻辑地址空间。
现有 `IBackupSession`、`IRecoveryPointReader` 和 Pipeline 表达一个连续逻辑源，不能用全局 offset 拼接
多个 volume，否则会丢失源级容量、恢复目标和失败边界。

产品尚未发布，可以直接固定最终事务语义，不提供旧试验分卷兼容。

## 决策

1. 分卷对现有单源 Port 完全透明。Pipeline 继续产生连续 chunk，Personal Archive Adapter 只在完整
   chunk 边界切换物理文件。
2. `ArchiveCreateRequest.split_size_bytes == 0` 表示单文件；非零表示启用分卷。每个分卷至少包含完整
   Header，单个 chunk 可以使分卷超过目标值，但不能跨文件拆分。
3. 首卷保存 metadata，续卷只保存 Header 和 chunk；Footer 只写入末卷。全局 `chunk_index` 跨分卷
   连续，Footer 统计覆盖所有分卷。
4. 所有分卷永久写 `split_part_count = 0`。首卷 Header 是 metadata AEAD 的 AAD，提交时回写总数会
   破坏认证；读取器通过连续编号发现分卷，并受 `maximum_split_parts` 上限约束。
5. 发布时先发布 Sidecar 和续卷，最后发布首卷。首卷路径是可见性标记；进程内任一步失败都删除本次
   已发布文件和 partial。进程崩溃可能留下没有首卷的孤立续卷，但不会暴露看似完整的主 Archive。
6. Reader 必须验证分卷编号、Header 身份字段、算法、split size、连续 chunk index、源逻辑范围以及
   末卷 Footer；缺卷、乱序、重复 Footer 和身份不一致均拒绝。
7. 多 volume 不扩展 `ChunkDescriptor` 为含糊的全局逻辑流。后续 Port 采用两级生命周期：
   `IBackupSetSession::begin_source(SourceDescriptor)` 返回 source-scoped writer，所有 source 完成后由 set
   session 统一 Commit/Abort；读取侧由 `IRecoveryPointSetReader` 枚举 source，并为选定 source 返回
   现有语义的 `IRecoveryPointReader`。应用层负责把多个 Source Pipeline 组织为一个事务。
8. 本阶段实现透明分卷；多 source Port 在实现第二个真实 source use case 时落地，避免提前加入没有
   消费者的抽象。

## 备选方案

- 把多个 volume 按大小串成一个全局逻辑地址空间：恢复必须反向计算边界，无法自然映射多个 Sink，
  不采用。
- 给现有 `ChunkDescriptor` 直接增加 `source_index`：`logical_size_bytes()`、Commit 和 Restore Sink 仍然
  没有 source 语义，只解决字段而未解决生命周期，不采用。
- 每个 volume 生成独立 `.bkf`：破坏一个备份共享 Manifest、备份链 UUID 和一致提交的目标，不采用。
- 提交时回写所有 Header 的总分卷数：会改变 metadata AAD，除非重新加密 metadata；收益不足，不采用。
- 首卷最先 rename：短暂或崩溃后会暴露缺卷 Archive，不采用。

## 影响

- 分卷 Archive 的完整性依赖连续文件集合，但恢复仍不依赖 Sidecar。
- `BackupFooter.file_size` 只描述末卷自身，Footer 其它统计描述整个逻辑 Archive。
- 同一路径若存在孤立续卷或 Sidecar，创建操作返回冲突，不自动删除无法证明归属的文件。
- 多 volume 全量与增量均已实现：Session 按 Manifest `volumes[]` 顺序写入；增量父匹配与 Chain Reader
  均要求有序 volume 几何一致。多目标 Restore 的显式映射仍属后续工作。

## 验证

- Header golden 测试覆盖非分卷、首卷和续卷规则。
- 端到端测试强制跨多个分卷，验证跨卷 chunk 顺序、ZERO、Sidecar 和恢复字节一致。
- 损坏输入测试覆盖缺失中间卷、续卷 Header 身份不一致和末卷 Footer 缺失。
- Abort/发布失败测试验证所有本次 partial 与已发布产物被清理。
- Debug、Release、clang-tidy、clang-format、规模检查和 `git diff --check` 作为质量门禁。
