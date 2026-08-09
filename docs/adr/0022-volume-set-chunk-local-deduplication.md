# ADR-0022：Volume Set 单 Chunk 固定块去重

- 状态：Accepted
- 日期：2026-08-09
- 决策者：Aegra Maintainers
- 关联模块：contracts、format、pipeline、adapters/personal_archive、service、worker、desktop

## 背景

Personal Archive V7 已预留 `BACKUP_FLAG_DEDUP` 与 `BlockEntry::DEDUP`，但没有冻结引用域、碰撞处理、
增量协作和恢复拒绝规则，当前 Writer 也只生成 RAW、COMPRESSED 与 ZERO。Volume Set 的固定 64 KiB
逻辑块适合低成本去重，但跨 Chunk、跨分卷或跨 Recovery Point 的引用会破坏分卷独立性、顺序恢复、
有界内存和个人版单文件 Repository 边界。

Volume Incremental 已通过加密 Sidecar 比较父层块哈希并省略未变化块。该机制解决跨 Recovery Point 的
变化复用，不应与 Archive 内重复内容去重混为一体。

## 决策

1. Personal Archive 只为 `volume_set` 实现单个物理 `VolumeChunk` 内的固定块去重。Full 与
   Incremental 都可启用；`file_set` 本期禁止使用 DEDUP。
2. DEDUP 只能回指同一 `VolumeChunk` 中索引更小的 RAW 或 COMPRESSED canonical entry。禁止前向、
   链式、跨 Chunk、跨分卷、跨 Volume、跨 Archive 和跨 Recovery Point 引用。
3. Writer 按逻辑块序选择第一个相同块为 canonical。以 `(SHA-256(plaintext), logical_size)` 查找候选，
   命中后必须逐字节比较；哈希碰撞不得产生错误引用。
4. 处理顺序固定为 ZERO 检测、Incremental 父层比较、当前 Chunk 去重、canonical 压缩、Chunk AEAD。
   ZERO 保持 ZERO run；父层未变化块不进入本层去重窗口。
5. `deduplication_enabled` 是 Volume Schedule 的创建时冻结选项，默认 `true`。Service、Worker Job 与
   `BackupOptions` 必须显式传递；file_set 必须为 `false`。
6. 开启策略时 Header 设置 `BACKUP_FLAG_DEDUP`，即使本次没有发现重复块。存在 DEDUP entry 而 Header
   未置位属于损坏。该 flag 不表示一定产生了节省。
7. Footer 与 `TaskResult` 分别报告 `deduplicated_block_count` 和 `deduplicated_logical_bytes`。这些计数只含
   DEDUP entry，不把 ZERO、压缩节省或 Incremental 父层省略计入去重收益。
8. BlockEntry 表作为 Chunk AEAD AAD 保持明文，因此会泄露同一 Chunk 内哪些块相等。产品必须在界面与
   安全说明中将其作为有界元数据泄露；关闭去重时不得生成 DEDUP entry。
9. 产品尚未发布，V7、Service V4 和 Worker 当前 schema 直接原子更新，不增加旧格式解析、迁移或 fallback。

详细格式、算法和验收不变量见
[Volume Set 去重设计](../architecture/VOLUME_SET_DEDUPLICATION.md)。

## 备选方案

- **Archive 全局去重：** 需要全局索引和随机恢复访问，分卷缺失时影响范围扩大，拒绝。
- **跨 Recovery Point DEDUP 引用：** 使单个 `.bkf` 隐式依赖外部对象；现有 Incremental Sidecar 已承担该职责，拒绝。
- **仅比较 SHA-256：** 极低概率碰撞仍会造成静默数据损坏，必须进行字节确认。
- **可递归 DEDUP 链：** 增加恢复深度、循环与拒绝面，不产生额外收益，拒绝。
- **内容定义分块：** 对块级卷镜像、随机访问和当前 Sidecar 格式改动过大，留给企业 CAS 层。

## 影响

- 默认 64 MiB 物理 Chunk 形成约 64 MiB 去重窗口（格式上限 512 MiB），内存随单 Chunk 有界，不需要持久化去重数据库。
- 相距超过一个 Chunk 的重复内容不会被发现，不能把个人版能力宣传为全局去重。
- canonical payload 只压缩和加密一次；恢复可在认证后缓存 canonical 明文并复制到多个逻辑块。
- Incremental 链、Sidecar 完整基线、分卷提交和 Repository Catalog 权威边界保持不变。
- 企业版跨备份去重继续由 Repository CAS 实现，不复用 Personal Archive 的 DEDUP 引用。

## 验证

- 构建 Debug/Release 受影响生产 Targets，并运行静态与架构检查。
- 人工验证 Full/Incremental 中同 Chunk 重复、跨 Chunk 不重复、ZERO、末尾短块和 SHA-256 候选确认。
- 人工损坏 DEDUP flag、引用索引、目标类型和长度，确认 Verify/Restore 在输出前稳定拒绝。
- 人工验证分卷边界不会产生跨 part 引用，任一 Chunk 可在只持有其 record 内容时独立解析。

