# 文件集产品上限、稳定码与人工验证矩阵

| 属性 | 内容 |
| --- | --- |
| 状态 | F0 冻结权威 |
| 日期 | 2026-08-07 |
| 关联 | [ADR-0016](../adr/0016-file-set-backup-and-restore-boundary.md)、[V7 格式](../format/PERSONAL_BACKUP_FORMAT_V7.md)、[Service V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md) |

本文冻结首版 `file_set` 与相关 Volume V7 路径共用的**产品上限**、**稳定 message/error code** 与
**人工损坏/失败样本矩阵**。格式硬上限以 V7 为准；产品配置只能更严，不能更松。

按 ADR-0015：**不**把下列样本提交为仓库 fixture 或自动化测试。

---

## 1. 产品上限

| ID | 项 | 上限 | 超限稳定码 |
| --- | ---: | --- | --- |
| L01 | 单 Job selection roots | 100 | `file_backup.selection_limit` |
| L02 | 目录最大深度（自 selection root） | 64 | `file_source.depth_limit` |
| L03 | 单目录 children（枚举可见） | 1_000_000 | `file_source.directory_fanout_limit` |
| L04 | 单 Archive 总 entries | 10_000_000 | `file_backup.entry_limit`（Writer 多层 B+tree，depth≤L14；实际还受 L12 每 leaf 密度约束） |
| L05 | 名称组件 UTF-16LE 字节 | 2–512（偶数） | `file_source.name_invalid` |
| L06 | （FI0 删除）ADS | — | `file_source.unsupported_ads` |
| L07 | （FI0 删除）ADS 名 | — | `file_source.unsupported_ads` |
| L08 | platform metadata envelope | 64 KiB | `file_source.metadata_limit` |
| L09 | 单 stream logical size | 16 TiB | `file_source.stream_size_limit` |
| L10 | 单 Archive 总 logical bytes | 1 PiB（2^50） | `file_backup.logical_bytes_limit` |
| L11 | （FI0 删除）sparse ranges | — | `file_source.unsupported_sparse` |
| L12 | Index page plain size | 1 MiB | `file_backup.index_page_limit` |
| L13 | Index page count | 4_000_000 | `file_backup.index_page_limit` |
| L14 | Index 最大深度 | 8（root depth=0） | `file_backup.index_depth_limit`（仅 depth>8 或无法在深度内收敛；**不得**因 leaf>257 误报） |
| L15 | 默认 chunk 目标 | 512 MiB | — |
| L16 | 单 chunk payload | 512 MiB | `format.chunk_size_limit` |
| L17 | 分卷数（含首卷） | 1000 | `format.split_part_limit` |
| L18 | 恢复选择 entry IDs | 10_000 | `file_restore.selection_limit` |
| L19 | Index spool 磁盘预算 / Job | 8 GiB | `file_backup.index_spool_budget_exceeded` |
| L20 | Browse 页大小 | 1–100 | `service.page_limit` |
| L21 | node token TTL | 15 min | `file_browse.token_invalid` |
| L22 | active node tokens / connection | 4096 | `file_browse.token_limit` |
| L23 | preflight token TTL | 30 min | `file_restore.preflight_expired` |
| L24 | CBOR metadata plaintext | 64 MiB | `format.metadata_size_limit` |
| L25 | Service frame | 64 KiB | framing reject |
| L26 | 相对组件段数 / selection | 64 | `file_source.path_limit` |
| L27 | Pipeline entry metadata queue | 8192 items | 背压等待；取消→`job.cancelled` |
| L28 | Pipeline content queue | 256 MiB | 背压等待 |
| L29 | （FI0 删除）hard-link group | — | `file_source.unsupported_hard_link` |
| L30 | Worker Job schema | 4 only | `job.schema_unsupported` |
| L31 | Reader 常驻 Index 内存（每打开一层 Archive） | 有界 LRU page cache（固定槽位）+ 热路径 leaf 缓存；**禁止** open 时 materialize 全量 `FileEntryDesc` 或 O(N) locator 表 | 设计硬约束（OOM 风险） |

整数累加一律 checked；超过 `INT64_MAX` 的 wire 字段拒绝。

### 1.1 Reader 运行时内存模型（L31）

`PersonalFileArchiveReader::open` 与后续 browse/restore 热路径必须满足（ADR-0019 Phase-2）：

| 结构 | 允许 | 禁止 |
| --- | --- | --- |
| entry 定位 | Entry ID B+tree + Namespace leaf `page_offset`/`slot` | `unordered_map` / 全量 `vector<FileEntryDesc>` / O(N) 内存表 |
| stream 定位 | Stream B+tree → `entry_id` + `stream_slot` | 哈希表或为每个 stream 复制完整 entry |
| page 定位 | Footer root + internal `ChildPageLocator.offset` 逐层 seek | 全 Archive 扫描建 `page_offsets` |
| stream chunk 表 | Chunk B+tree locator（`record_offset`/`payload_offset`） | chunk payload 常驻内存 |
| Index page | 有界 LRU（固定容量）按需解密/解码；单页 ≤ L12 | open 时 materialize 全部 leaf 明文 entry |
| 父目录图 | **仅 Verify**：Entry ID leaf 紧凑记录 + 三色 DFS | 普通 open/browse 全量父图校验 |
| `list_children` | Namespace B+tree 按 `parent_entry_id` 有序扫描 | 全表线性扫描 `entries` |
| `describe_entry` / `describe_stream_owner` | 二级索引 O(log N) 后加载所在 leaf | 依赖常驻全量 entry 表 |
| Verify | `verify_index_and_parent_graph` + `for_each_entry_in_leaf_order` | `for entry_id = 1..entry_count` 盲扫 |
| Chain 非 tip 层 | `FileArchiveIndexLoad::kDeferred`：open 只认证 Header/Footer/密钥 | open 时每层立即加载索引 |
| Writer finalize | spool 紧凑 `IndexKey` 排序 + 流式 leaf（≤1 页 entry 常驻）+ 二级索引 | finalize 时 `vector<FileEntryDesc>` 全表 |

**Phase-2（当前）：** V7 持久化二级索引（Entry ID / Stream / Chunk）+ internal child 物理 offset + Entry ID 记录内 Namespace `page_offset` + 有界 LRU page cache；普通 open 认证 roots 为 O(1) 页 I/O，查询 O(log N)；全量父图/唯一性校验仅在显式 Verify。链 browse 仅 tip 需 root 认证；祖先层在首次 stream 访问时再 `ensure_roots`。

---

## 2. Boundary ErrorCode 映射

Contracts/base `ErrorCode` 数值保持现有枚举；业务细分靠 `message_code`。

| 场景 | ErrorCode | 说明 |
| --- | --- | --- |
| 参数/exact_keys/枚举 | `InvalidArgument` | 含混合 volume/file payload |
| 版本不匹配 | `UnsupportedVersion` | schema/api/format/job |
| 取消 | `Cancelled` | |
| I/O、VSS、磁盘 | `IoFailure` | |
| AEAD/结构损坏 | `CorruptData` | |
| 资源不存在 | `NotFound` | |
| 幂等冲突、源冻结、能力冲突 | `Conflict` | |
| 授权/ACL/token 错绑 | `Unauthorized` | |
| 内部不变量 | `Internal` | |
| 空间不足 | `InsufficientSpace` | |
| 崩溃后结果不明 | `OutcomeUnknown` | |

---

## 3. 稳定 message_code

### 3.1 进度（TaskProgress.message_code）

```text
file_backup.snapshotting
file_backup.enumerating
file_backup.reading
file_backup.indexing
file_backup.finalizing
file_backup.completed
file_restore.preflighting
file_restore.writing
file_restore.metadata
file_restore.completed
```

Volume 路径保留既有 `backup.*` / `restore.*` / `verify.*` 码，不在此重复定义。

### 3.2 备份失败 / 拒绝

```text
file_backup.selection_limit
file_backup.entry_limit
file_backup.logical_bytes_limit
file_backup.index_page_limit
file_backup.index_depth_limit
file_backup.index_spool_budget_exceeded
file_backup.incremental_downgraded_full
file_backup.metadata_baseline_invalid
file_backup.parent_chain_invalid
file_backup.selection_fingerprint_mismatch
file_backup.vss_failed
file_backup.destination_full
file_backup.aborted
file_source.unsupported_unc
file_source.unsupported_filesystem
file_source.unsupported_efs
file_source.unsupported_cloud_placeholder
file_source.unsupported_reparse
file_source.unsupported_hard_link
file_source.unsupported_sparse
file_source.unsupported_ads
file_source.security_descriptor_unreadable
file_source.unreadable
file_source.depth_limit
file_source.directory_fanout_limit
file_source.name_invalid
file_source.metadata_limit
file_source.stream_size_limit
file_source.path_limit
file_source.volume_identity_mismatch
file_source.reparse_escape
file_restore.unsupported_archive_semantics
```

### 3.3 浏览 / 控制面

```text
file_browse.token_invalid
file_browse.token_limit
file_browse.unauthorized
file_browse.parent_invalid
schedule.source_frozen
schedule.selection_conflict
service.content_kind_mismatch
service.capability_unavailable
service.api_version_unsupported
service.page_limit
```

### 3.4 恢复

```text
file_restore.preflight_ok
file_restore.preflight_expired
file_restore.preflight_consumed
file_restore.selection_limit
file_restore.target_capability_missing
file_restore.target_file_too_large
file_restore.target_not_directory
file_restore.target_reparse_escape
file_restore.target_collision
file_restore.rename_exhausted
file_restore.target_full
file_restore.partial
file_restore.failed_before_write
file_restore.original_location_unsupported
file_recover.credential_required
file_recover.credential_failed
file_recover.corrupt
file_recover.catalog_only
file_recover.parent_missing
file_recover.parent_reference_invalid
file_recover.chain_depth_limit
file_recover.token_invalid
```

### 3.5 格式 / Job

```text
format.unsupported_version
format.corrupt_header
format.corrupt_metadata
format.corrupt_chunk
format.corrupt_index
format.corrupt_footer
format.split_incomplete
format.chunk_size_limit
format.metadata_size_limit
format.split_part_limit
job.schema_unsupported
job.cancelled
job.deadline_exceeded
```

`message_arguments` 只允许非敏感标量（计数、枚举名、UUID、整数限制值）。禁止路径、descriptor、token、
SecretRef、口令。

---

## 4. 人工验证矩阵

样本与运行只位于隔离临时目录或非生产 Volume；验证后删除。

### 4.1 格式损坏（Reader）

| ID | 操作 | 期望 message / 行为 |
| --- | --- | --- |
| C01 | `format_version≠7` | `format.unsupported_version` |
| C02 | 错误 `content_kind` | corrupt / invalid |
| C03 | 翻转 metadata tag | `format.corrupt_metadata` |
| C04 | 删除 Footer | `format.corrupt_footer` / incomplete |
| C05 | 错误 index root offset | `format.corrupt_index` |
| C06 | 翻转 index page | `format.corrupt_index` |
| C07 | parent 环 | `format.corrupt_index` |
| C08 | 坏 extent 引用 | `format.corrupt_index` 或 chunk |
| C09 | 缺分卷 | `format.split_incomplete` |
| C10 | 截断 stream payload | `format.corrupt_chunk` |
| C11 | file_set 含 volume chunk | corrupt |
| C12 | 未知 critical record kind | corrupt |

### 4.2 备份失败

| ID | 场景 | 期望 |
| --- | --- | --- |
| B01 | VSS 创建失败 | `file_backup.vss_failed`；无 RP |
| B02 | 选中文件拒绝读 | `file_source.unreadable`；Abort |
| B03 | SACL 不可读 | `file_source.security_descriptor_unreadable` |
| B04 | UNC 源 | `file_source.unsupported_unc` |
| B05 | EFS | `file_source.unsupported_efs` |
| B06 | offline cloud placeholder | `file_source.unsupported_cloud_placeholder` |
| B07 | 目标 Repository 满 | `file_backup.destination_full` |
| B08 | 取消于枚举/读取 | `job.cancelled`；清理 spool/partial |
| B09 | deadline | `job.deadline_exceeded` |
| B10 | Worker crash | Job `Interrupted` / `OutcomeUnknown` 收敛 |
| B11 | Index spool > 8 GiB | `file_backup.index_spool_budget_exceeded` |
| B12 | file Incremental baseline 不合格 | `file_backup.incremental_downgraded_full` + stable downgrade reason |

### 4.3 恢复失败

| ID | 场景 | 期望 |
| --- | --- | --- |
| R01 | FAT32 目标且请求恢复 ACL | `file_restore.target_capability_missing` |
| R02 | 冲突 + Fail | `file_restore.target_collision` |
| R03 | 目标盘满 | `file_restore.target_full` / InsufficientSpace |
| R04 | 目标 reparse 替换逃逸 | `file_restore.target_reparse_escape` |
| R05 | 写前认证失败 | `file_restore.failed_before_write` |
| R06 | FAT32 目标存在超过 4 GiB - 1 的文件 | `file_restore.target_file_too_large`；写前拒绝 |
| R06 | 写中失败 | partial + `file_restore.partial` |
| R07 | 原位置恢复 | `file_restore.original_location_unsupported` |
| R08 | 过期 preflight | `file_restore.preflight_expired` |
| R09 | 重复 Start 同 token | 第二次 conflict / consumed |
| R10 | 错误口令 | `file_recover.credential_failed` |

### 4.4 成功路径（元数据）

| ID | 覆盖 |
| --- | --- |
| S01 | 普通文件 + 空文件 + 空目录 |
| S02 | Unicode / 长组件名（≤256 UTF-16） |
| S03 | 深度 32 目录 |
| S04 | DACL+SACL |
| S05 | ADS：Backup strict fail；Restore 写前拒绝 |
| S06 | sparse：Backup strict fail；Restore 写前拒绝 |
| S07 | hard link：Backup strict fail；Restore 写前拒绝 |
| S08 | reparse：Backup strict fail；Restore 写前拒绝 |
| S09 | 多 selection / 多 volume 同一 VSS set |
| S10 | 加密 + 分卷 |
| S11 | Verify 全 page/chunk |
| S12 | 分页 browse + 选择性恢复 |

### 4.5 Volume 回归（V7 等价）

Full、Incremental、分卷、加密、Verify、Volume/Disk Restore、Catalog rebuild 必须在文件功能合并后人工通过。

---

## 5. 配置默认值（可更严）

| 项 | 默认 |
| --- | --- |
| default_chunk_size | 512 MiB |
| split_size_bytes | 0（不分卷）或用户配置 ≤ 产品上限 |
| browse page default | 50 |
| event window default | 64 |
| index spool budget | 8 GiB |
| node token TTL | 900 s |
| preflight TTL | 1800 s |

---

## 6. 变更规则

- 放宽格式硬上限需要新格式版本 + ADR；
- 仅收紧产品默认值可在模块文档记录，不得破坏已冻结 wire 枚举值；
- 新增 message_code 必须同步 Desktop 翻译映射（F9）。
