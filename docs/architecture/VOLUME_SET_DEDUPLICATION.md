# Volume Set 单 Chunk 去重设计

## 1. 状态与权威

- 状态：Accepted design
- 日期：2026-08-09
- 决策：[ADR-0022](../adr/0022-volume-set-chunk-local-deduplication.md)
- 格式：[Personal Archive V7](../format/PERSONAL_BACKUP_FORMAT_V7.md)

本文定义个人版 Volume Set 的去重域、写入顺序、恢复规则、控制面选项和统计口径。格式字节布局以 V7
文档为权威；模块依赖以 `MODULAR_ARCHITECTURE.md` 为权威。

## 2. 目标与非目标

目标：

- 在不引入 Repository 数据库或跨文件引用的情况下减少重复卷块 payload；
- 保持固定块随机映射、分卷独立、顺序恢复、认证后输出和有界内存；
- 与现有 Volume Full/Incremental、ZERO run、机会性 zstd 和 XChaCha20-Poly1305 组合；
- 给 Writer、Reader、Verify、Service、Worker 和 Desktop 提供唯一可执行合同。

非目标：

- File Set、跨 Chunk、跨 Volume、跨分卷、跨 Archive 或跨 Recovery Point 去重；
- 内容定义分块、相似压缩、全局 CAS、可变长 extent 去重或网络 Repository 索引；
- 使用 DEDUP 替代 Incremental Sidecar、父链或文件变化判断；
- 隐藏同一 Chunk 内的块相等关系。

## 3. 核心模型

去重域是一个已经确定物理边界的 `VolumeChunk` record。Window 在新 Chunk 开始时为空，在 Chunk
提交、因分卷容量提前关闭或切换 source volume 时立即丢弃。

```text
snapshot block
  -> ZERO classification
  -> Incremental parent comparison (unchanged: omit from this layer)
  -> SHA-256 + current-Chunk candidate lookup
  -> byte confirmation
      duplicate: DEDUP entry
      first occurrence: opportunistic zstd -> RAW/COMPRESSED canonical entry
  -> authenticate BlockEntry table as AAD + encrypt canonical payload
```

`Incremental omission` 表示逻辑块由父 Recovery Point 提供；`DEDUP entry` 表示本 Archive、本物理 Chunk
内另一个较早 entry 提供相同明文。两个机制不得共享引用编码。

## 4. 写入算法

### 4.1 逻辑顺序与 canonical 选择

Writer 按 `(source_index, logical_block_index)` 升序组装 entry。实现分为：

1. **并行**：ZERO 检测与 SHA-256（及非 DEDUP 路径下的 zstd）；
2. **串行**：按逻辑序选择 first-canonical / DEDUP 引用（含逐字节确认），保证同样输入与 Chunk
   geometry 产生相同 canonical 选择；
3. **并行**：仅对已确定的 canonical 做机会性 zstd（DEDUP 命中块不压缩）；
4. **串行**：按序写出 entry 与 payload。

每个 Chunk 维护：

```text
key = (SHA-256 plaintext[logical_size], logical_size)
value = ordered list of canonical entry indexes
```

候选命中后，Writer 从当前 Chunk 的 plaintext backing storage 读取 canonical bytes 并逐字节确认。首个
完全相同的候选成为引用目标；均不相同时当前块成为新 canonical。不得仅凭哈希、压缩结果或密文判断。

### 4.2 分类顺序

1. 全零块先合并为 ZERO run，不进入哈希表。
2. Incremental 先与父 Sidecar 比较。未变化块不生成 entry，也不进入当前 Chunk 哈希表。
3. 变化 DATA 块进行当前 Chunk 去重。
4. 新 canonical 块在选择完成后执行机会性 zstd（可与其它 canonical 并行）；仅压缩结果严格更小时写
   COMPRESSED，否则 RAW。
5. DEDUP 不写 payload，不再次压缩或加密数据。

最后一个短块的 `logical_size` 参与 key，并且只能引用相同长度 canonical。ZERO run 仍可覆盖多个连续逻辑块；
DEDUP 永远只代表一个逻辑块。

### 4.3 Chunk 与分卷边界

Chunk 的 logical payload budget、entry 上限和分卷 record 边界沿用当前配置。Writer 在形成下一个
`VolumeChunk` 时重置去重表。即使两个 Chunk 相邻、位于同一 part，也不得互相引用；同一 Chunk record
不会跨 part，因此 DEDUP 天然不会跨分卷。

内存预算必须同时覆盖当前 Chunk 的 plaintext canonical storage、待提交 entry/payload 和哈希索引。
实现不得把所有 Archive 哈希驻留内存，也不得创建持久化 dedup index。

## 5. BlockEntry 合同

DEDUP entry 必须满足：

- `flags == DEDUP`，不能与 RAW、COMPRESSED、ZERO 组合；
- `logical_block_index` 是待恢复的目标逻辑块；
- `ptr.ref_index` 是当前 record `BlockEntry[]` 的零基索引；
- `ref_index < current_entry_index`；
- 目标 entry 必须为 RAW 或 COMPRESSED，不能为 ZERO 或 DEDUP；
- 目标解码后的逻辑长度等于当前目标块实际长度；
- `stored_size == 0` 且 `logical_size == 0`；
- entry 本身不占 payload 字节。

DEDUP 的目标长度由 canonical entry 的 `logical_size` 得到；Reader 还必须结合目标卷逻辑大小与
`logical_block_index` 验证该长度正是当前位置允许的块长度。该规则排除短尾块引用完整块。

## 6. Full 与 Incremental

Full 对所有非 ZERO DATA 块应用本设计。

Incremental 保持完整加密 `.bhx` 基线：每个当前块仍产生 Sidecar state/hash，未变化块从 `.bkf` 本层省略，
变化块才进入 DEDUP window。父层块不能成为 DEDUP canonical；DATA→ZERO 仍显式写 ZERO。去重开关不改变
父 UUID、链深、Sidecar 认证、降级 Full 或 Chain Reader overlay 规则。

因此同一 Incremental Chunk 的 entry 在逻辑索引上可以不连续；引用使用 entry index，不使用逻辑块距离。

## 7. 恢复与 Verify

Reader 对每个 Chunk 按以下顺序工作：

1. 校验 record/header/entry 数量和所有算术边界；
2. 验证 Header DEDUP 策略与 entry 类型一致；
3. 认证 BlockEntry AAD 与 payload AEAD；
4. 验证每个 DEDUP 引用为同 Chunk 后向 canonical 引用且长度合法；
5. 解码 RAW/COMPRESSED canonical，按需缓存其明文；
6. 将 canonical 明文复制到 DEDUP entry 的目标逻辑块。

认证或结构验证失败前不得向 Block Sink 返回任何当前 Chunk 数据。由于目标禁止为 DEDUP，不存在递归、环或
深度限制。Restore 可以只缓存被引用的 canonical，也可以缓存整个已认证 Chunk，但必须受 Chunk memory budget
约束。Volume Restore 与完整 Verify 在读完最后一个 Chunk 后重算两个 dedup 计数并核对 Footer。Verify 必须
执行相同结构验证、解密、解压和重复块内容展开长度检查，不要求写目标卷。

稳定损坏分类沿用 `format.corrupt_chunk`；日志可使用不含路径、密钥、哈希或块内容的内部 reason，例如
`dedup_without_header_flag`、`dedup_forward_reference`、`dedup_invalid_target`、`dedup_length_mismatch`。

## 8. 控制面与 UI

新增 `deduplication_enabled: bool`：

- Volume Schedule 创建默认 `true`，创建后冻结；
- `BackupOptions`、Service→Worker Job 和 `WindowsPersonalBackupRequest` 必须显式携带；
- `file_set` 必须为 `false`，非 Volume 请求设置为 true 时在获取凭据或创建 Snapshot 前拒绝；
- Desktop 只在 Volume Set 模式显示开关，编辑已存在 Schedule 时只读；
- Writer 仅在 true 时设置 `BACKUP_FLAG_DEDUP` 和生成 DEDUP entry。

产品未发布，因此 Service V4、Worker schema 4、SQLite current schema 与 V7 同一工作包原子更新，不保留
缺省字段解析或历史开发文件 fallback。

## 9. 指标

Writer 在 Commit 前汇总：

- `deduplicated_block_count`：DEDUP entry 数；
- `deduplicated_logical_bytes`：所有 DEDUP 目标展开后的明文长度之和。

V7 Footer 和 `TaskResult` 保存这两个值；file_set 固定为 0。`total_payload_size` 表示 chunk payload
合计；Worker 成功路径上的 `TaskResult.stored_bytes` 为已提交 Archive 各分卷 on-disk 大小之和（wire），
不得用 volume stage-2 的 `descriptor.stored_size`（常等于 logical）冒充。去重指标不包含 ZERO、zstd
节省或 Incremental 父层省略，避免把四种不同收益混成一个比率。

显示去重率时使用：

```text
dedup_ratio = deduplicated_logical_bytes /
              (canonical_data_logical_bytes + deduplicated_logical_bytes)
```

分母只含本层非 ZERO DATA；为 0 时显示不可用，不显示 0/0。Catalog 可从 Footer 重建计数，不保存逐块哈希。

## 10. 安全与隐私

- 哈希仅用于 Writer 内存索引，Archive、Sidecar、Catalog 和日志不得持久化 dedup hash。
- 不使用 convergent encryption；每个 Chunk 继续使用密码学随机 nonce 和现有 AEAD key derivation。
- BlockEntry 表是认证但未加密的 AAD。DEDUP flag 与 `ref_index` 会泄露一个 Chunk 内的块相等关系和重复位置；
  不泄露跨 Chunk/Archive 相等关系。关闭去重可移除该新增泄露面。
- 字节确认是数据完整性要求，不是可选的 debug 检查。
- DEDUP 不改变 Sidecar 加密要求，Sidecar plaintext hashes 仍不得暴露。

## 11. 模块所有权

| 模块 | 所有权 |
| --- | --- |
| `contracts` | 选项、结果计数、schema 验证 |
| `format` | DEDUP entry 编解码、Header/Footer 校验、Reader/Verify 结构规则 |
| `adapters/personal_archive` | 当前 Chunk 索引、字节确认、canonical payload 与计数 |
| `pipeline` | 继续传输块与汇报进度，不知道 Personal Archive 引用格式 |
| `service` / SQLite | Schedule 默认值、冻结、不变量与 Worker Job 投影 |
| `worker` | 请求早期验证、选项透传、结果净化 |
| `desktop` | Volume-only 开关和去重收益展示 |

`pipeline` 不得 include Personal Archive 格式头，也不得承担 Repository 级去重查询。

## 12. 验收矩阵

| 场景 | 期望 |
| --- | --- |
| 同 Chunk 两个相同 DATA 块 | 后者 DEDUP 回指首个 canonical |
| 同 Chunk 三个相同块 | 后两个均直接回指首个 canonical，无链 |
| 相同哈希候选但 bytes 不同 | 新 canonical，不产生 DEDUP |
| 相同 bytes 位于相邻 Chunk | 两个 canonical |
| 相同 bytes 位于不同 Volume/part/RP | 不引用 |
| ZERO 与 DATA 全零 | 只生成 ZERO，不进入 DEDUP |
| Incremental 未变化块 | 本层省略，不进入 DEDUP |
| Incremental 两个变化块相同 | 同 Chunk 时可 DEDUP |
| 最后短块与完整块前缀相同 | 长度不同，不 DEDUP |
| 禁用去重 | Header flag 清零且无 DEDUP entry |
| DEDUP forward/out-of-range/target DEDUP | Verify/Restore 输出前拒绝 |
| 分卷临界点关闭 Chunk | 新 part/Chunk 重置窗口 |

实现验收遵循仓库测试策略：不新增测试 Target；构建受影响 Debug/Release 生产 Targets，运行静态与架构检查，
并用受控镜像执行上述人工写入、Verify、Restore 与损坏注入。
