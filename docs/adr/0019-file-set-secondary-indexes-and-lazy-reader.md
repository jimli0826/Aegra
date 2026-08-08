# ADR-0019：File Index 二级索引与惰性 Reader

- 状态：Accepted
- 日期：2026-08-08
- 决策者：Aegra 项目
- 关联模块：format、adapters/personal_archive、apps/worker、apps/service
- 关联文档：[PERSONAL_BACKUP_FORMAT_V7](../format/PERSONAL_BACKUP_FORMAT_V7.md)、[L31](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md)、[adapters](../modules/adapters.md)
- 替代范围：ADR-0016 / 既有 V7 中“单棵 Namespace B+tree + Reader 首次全量扫描建内存表”的打开模型

## 背景

主 File Index 的 B+tree key 为 `(parent_entry_id, name, entry_id)`，适合 `list_children`，
却不能高效完成：

- `describe_entry(entry_id)`
- `describe_stream_owner(stream_index)`
- `scan_file_stream_chunks` / 按 `chunk_index` 定位 payload

若仅删除 Reader 中的哈希表而不补索引，查询会退化为全树扫描。Phase-1 已用**排序连续数组**
替代 `unordered_map`、合并 leaf 双扫、父图改为三色 DFS，降低了 O(N) 扫描的内存与 CPU，
但普通打开仍是 O(N)。

产品尚未发布，允许直接更新 V7 durable contract，**不**增加旧 Archive 兼容路径。

## 决策

### 1. 四棵持久化索引

每个 `file_set` Archive 在 Footer 中保存最多四棵索引的 root locator
（`page_id` + 绝对 `offset` + root digest）：

| 索引 | Key | 用途 | 目标复杂度 |
| --- | --- | --- | --- |
| Namespace（保留） | `(parent_entry_id, name, entry_id)` | `list_children` | O(log N + K) |
| Entry ID | `entry_id` | `describe_entry`、父链定位 | O(log N) |
| Stream | `stream_index` | 增量链 `describe_stream_owner` | O(log S) |
| Chunk Locator | `chunk_index` | `read_stream` 定位 chunk | O(log C) |

`index_page_count` 为**全部** Index page 之和（四棵树合计）。`entry_count` / `stream_count` /
`file_stream_chunk_count` 语义不变。

`stream_count == 0` 时 Stream root 全 0；`file_stream_chunk_count == 0` 时 Chunk root 全 0。
`entry_count > 0` 时 Namespace 与 Entry ID root 必须有效。

### 2. Internal child 携带物理 offset

Internal page 的 child 由仅 `page_id` 改为：

```text
ChildPageLocator = { page_id : u64, offset : u64 }
```

`offset` 为该 child 的 `ArchiveRecordPrefix` 绝对文件偏移，位于已认证的 internal plaintext 内。
Reader 从 Footer root locator 逐层 seek，**禁止**为定位页而先扫描全 Archive 建 `page_offsets`。

### 3. 普通打开 vs 完整 Verify

| 路径 | 行为 |
| --- | --- |
| 普通 open | 认证 Header/Footer；校验各非零 root page；初始化有界 page cache；**不**扫描全部 leaf/chunk；**不**建 O(N) 内存 locator 表 |
| browse / 选择性恢复 | 仅认证实际访问路径上的 page AEAD、page id/kind、key 范围、涉及的 Entry/父链/Stream/Chunk |
| `verify_recoverability` | 显式全量：全部 leaf、父图、stream 引用、local chunk payload |

打开时不再要求“发现任意 Index 损坏”。损坏页在**访问到**时失败；完整损坏枚举留给 Verify。

### 4. Reader 常驻内存（更新 L31）

- 有界 LRU page cache（容量固定，例如数页至数十页，单页 ≤ L12）；
- 无全量 `entry_by_id` / `stream_by_index` / `chunks_by_index` 表；
- Chain 非 tip 层仍可用 deferred：仅 Header/Footer，首次 stream 访问再按需查索引。

### 5. Writer 义务

finalize 时：

1. 写完 local file stream chunk 后写 Index pages；
2. 先写 Namespace 树（leaf 仍含完整 `FileEntryDesc`）；
3. 自 Namespace leaf 确定性导出 Entry ID / Stream 二级 leaf，再自本层 chunk 记录导出 Chunk leaf；
4. 各树 bottom-up 建 internal，child 写入 `page_id`+`offset`；
5. Footer 写入四个 root（可空规则见上）与合计 `index_page_count`。

二级 leaf 只存紧凑 locator，不复制完整 entry 明文。

Entry ID leaf 记录必须携带 Namespace leaf 的绝对 `page_offset`（与 `page_id`/`slot` 一起），
使 `describe_entry` 在 O(log N) 定位后直接 seek 全量 entry，无需全局 `page_id→offset` 表。

### 6. 无兼容

拒绝缺少二级 root（当计数要求存在时）、internal child 无 offset、或仍依赖全量扫描才能打开的开发期 Archive。
不实现 dual-read、版本探测或 fallback。

## 备选方案

1. **仅换容器（vector/map）** — 不能消除 O(N) 打开与定位根因；已作为 Phase-1 过渡，不作终点。
2. **单棵树多 key 冗余 leaf** — 膨胀 Namespace leaf，破坏列目录局部性。
3. **外部 sidecar 索引** — 破坏单文件可认证恢复与 Repository 对象边界。
4. **保留全量扫描 + 磁盘 mmap** — 仍 O(N) I/O，不满足千万级打开时延。

## 影响

- V7 Footer reserved 区用于 Entry/Stream/Chunk root；Namespace root 字段名保持兼容布局前缀。
- Internal page CBOR 与 page_kind 枚举扩展；format codec、Writer、Reader 同步修改。
- 父图全量校验迁入 Verify；browse 仅校验访问闭包。
- 文档：L31、adapters、format V7 与本 ADR 同步。

## 验证

- 构建 `aegra_adapter_personal_archive`、worker/service 受影响目标；
- `CheckSourceLimits` / architecture 检查；
- 人工：小 Archive 列目录 / describe / read_stream；大规模 Archive 对比 Phase-1 的打开内存与时延；
- Verify 路径仍能发现全量损坏。
