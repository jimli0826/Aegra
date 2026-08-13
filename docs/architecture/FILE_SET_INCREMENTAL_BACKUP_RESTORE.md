# 文件集增量备份与链式恢复设计

| 属性 | 内容 |
| --- | --- |
| 状态 | Implemented；metadata signature 已完成 current 生产代码切换，隔离环境人工矩阵待执行 |
| 版本 | 2.0 |
| 日期 | 2026-08-09 |
| 范围 | Windows 个人版、file_set Full/Incremental、计划任务、链式浏览/验证/恢复 |
| 决策 | [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md)（变化判断）、[ADR-0021](../adr/0021-fat32-file-set-source-and-restore.md)（FAT32 能力） |

## 1. 目标与非目标

本设计在现有 Full `file_set` 基础上增加可由 Schedule 周期执行的 Incremental。核心语义是：每层拥有当前时点
完整、分页且认证的 File Index，但只有新建或内容变化的普通文件在本层保存 payload；其它普通文件引用直接父层
内容。

目标：

- 使用 `write_time + logical_size` 判断同一路径普通文件内容是否变化，不依赖 USN Journal；
- tip Recovery Point 可直接分页浏览当前树，不重放目录事件；
- 支持创建、修改、metadata-only、删除、rename 和 move；
- Restore/Verify 通过统一 chain reader 解析本层及父层内容；
- Archive 发布、Catalog 可重建、链感知删除、取消和失败原子性保持明确；
- 百万级 entry 仍使用分页 Index、磁盘 spool、有界 batch 和背压。

本期非目标：Differential、block delta、content-defined chunking、跨文件去重、chain merge/rebase/squash、合成
Full、跨 Schedule 复用内容、UNC、无 VSS fallback、跨平台恢复。

本期明确不支持 Backup 或 Restore：reparse point、hard link、sparse file、ADS。遇到即 strict fail；不存在
follow、flatten、dense materialization、duplicate-as-independent 或 ignore-stream 模式。
源文件系统支持 NTFS/ReFS/FAT32 且必须由 VSS Provider 创建 snapshot；FAT32 的 coarse mtime 是
metadata signature 的已知漏检边界。

## 2. 术语

| 术语 | 定义 |
| --- | --- |
| requested type | 用户或 Schedule 请求的备份类型 |
| effective type | 实际发布 Recovery Point 的类型；Incremental 资格失败时为 Full |
| metadata signature | 普通文件的 `(write_time, logical_size)` |
| baseline | 父 Recovery Point 中用于判定下次增量资格的 selection fingerprint 与 change detection method |
| tip | 用户选择用于浏览、Verify 或 Restore 的链末 Recovery Point |
| local stream | 内容 extents 位于当前 Archive 的主数据流 |
| parent stream | 当前 Index 不含 payload，引用直接父层 stream 的主数据流 |
| selection fingerprint | 规范化不可变 file selection spec 的认证摘要 |
| current namespace index | 当前快照中全部受保护目录和普通文件的 File Index |

“分页 File Index”表示文件树不会作为一个巨型 metadata object 一次加载。Entry 按稳定 key 分布在认证 B+tree
page 中，Reader 可按目录和 continuation token 读取需要的页。分页是物理存储与查询机制，不是部分备份；每个
Recovery Point 的 Index 仍描述完整当前树。

## 3. 总体数据流

```text
Schedule(requested=incremental)
  -> Service selects parent candidate
  -> Worker opens parent chain + one VSS Snapshot Set
  -> validate selection fingerprint and metadata baseline
       eligible   -> effective=incremental
       ineligible -> effective=full, parent_uuid=0
  -> enumerate complete current namespace
  -> reject unsupported objects
  -> classify entries with parent path Index + write_time/logical_size
  -> write changed/new main streams
  -> write complete current File Index
  -> commit Archive Group, then Catalog

Tip Recovery Point
  -> FileRecoveryChainReader
  -> tip Index for namespace/query
  -> local stream or recursive direct-parent stream resolution
  -> Verify or FileSetRestorePipeline
```

依赖方向不变：

```text
apps -> application, adapters, personal_repository, pipeline
application -> contracts, ports, personal_repository
pipeline -> base, contracts, ports, format
personal_repository -> base, contracts, ports, format
adapters/windows_filesystem -> base, contracts, ports
adapters/personal_archive -> base, contracts, ports, format
format, ports, contracts -> base
```

Windows Adapter 负责 VSS snapshot 枚举、metadata/security 读取和不支持对象检测；`pipeline` 使用平台无关
parent path index 与 metadata signature 比较。Archive Adapter 解析物理 page/record；chain reader 暴露
`IFileRecoveryPoint` 语义。只有 composition root 选择具体 Adapter。

## 4. 计划任务语义

### 4.1 Schedule

`file_set` Schedule 的请求类型固定为 Incremental（不是用户选项）。首次运行或没有合格父时降为 Full。
用户可对单次 Job 请求 Full。Schedule 创建时冻结：

- `backup_set_uuid`；
- 规范化 selection roots、recursion、exclusion、metadata/security 选项；
- selection fingerprint 算法及其版本；
- change detection method（当前固定 `mtime_size_v1`）；
- chain depth policy；
- credential reference。

Repository connection 可改；改后清空 tip，下次增量降 Full。

更新任何进入 fingerprint 的字段必须创建新的 backup set 或明确重置为 Full 基线，不能继续旧链。显示名称、运行
时间、保留窗口等不影响内容选择的字段不进入 fingerprint。

### 4.2 父层选择

Service/Repository graph 为同一 backup set 选择最新可用 Recovery Point。候选必须：

1. `content_kind=file_set`；
2. structural state complete，Catalog 与 Header identity 一致；
3. backup set 与 selection fingerprint 相同；
4. 从候选到 Full root 的链完整、无环、深度不超过 128；
5. Credential 可认证候选 metadata baseline 和 File Index；
6. 不是处于删除计划、缺卷、冲突或 credential-failed 状态。

没有候选不是错误：effective type 为 Full。父候选存在但损坏/缺失时也不沿链向后“找一个看起来可用的旧父层”；
生成新 Full，避免分叉和遗漏中间变化。

### 4.3 请求与结果

Job 固定携带：

```text
requested_backup_type
candidate_parent_uuid (nullable)
backup_set_uuid
selection_fingerprint
file_source_refs[]
```

结果与持久化状态分别记录：

```text
requested_backup_type
effective_backup_type
effective_parent_uuid
incremental_downgrade_reason (nullable stable enum)
```

建议 downgrade reason：`no_parent`、`selection_changed`、`chain_incomplete`、`baseline_invalid`、
`parent_unavailable`、`volume_identity_changed`。旧 `journal_*` reason 属 ADR-0018 历史路径，不再由
metadata baseline 增量产生。这些码可进入日志和 UI；路径、文件名和 security descriptor 不能进入普通消息。

## 5. Metadata Baseline

ADR-0018 的 USN checkpoint 与 journal 连续性设计已被 ADR-0020 替代；现行 baseline 只包含 selection
fingerprint 与 change detection method。

每个 file_set Full/Incremental 的认证 metadata 保存：

```text
file_set_baseline {
  fingerprint_algorithm = 1
  selection_fingerprint = 32-byte digest
  change_detection_method = 1   // mtime_size_v1
}
```

baseline 只证明“本 RP 可作为同一 selection 与同一变化判断方法的父层”。它不证明源端所有内容变化都能被发现；
内容变化判断由当前枚举结果和父 Index 中的 metadata signature 比较完成。

### 5.1 当前命名空间仍是权威

metadata signature 只构成 payload 复用判断，不能替代当前树枚举。完整枚举负责：

- 生成当前父子图、名称、metadata 和删除后的最终状态；
- 发现排除规则结果；
- 检测不支持对象；
- 构建 parent path index 并校验 parent stream 引用；
- 防止 rename/move、删除、新路径或类型变化被误表达为旧路径内容。

## 6. Entry 身份与变化规划

### 6.1 匹配 key

Incremental planner 为直接父层构建 path index：

```text
key = (selection_id, relative_path_components, entry_kind)
```

路径组件使用 Archive File Index 中的规范化 encoded name；比较按编码字节执行，不使用 locale。rename/move 后路径
不同，因此按新 entry 处理并写 local stream。`StableFileIdentity` 可继续作为加密 File Index 中的诊断/校验
metadata，但不再作为增量匹配 key 或变化证明。

### 6.2 分类表

普通文件的变化签名为 `write_time + logical_size`。分类表：

| 当前 entry | 父 entry | metadata signature 结论 | 本层动作 |
| --- | --- | --- | --- |
| 普通文件 | 无 | 新路径或新文件 | 写完整 local stream |
| 普通文件 | 同路径普通文件 | `write_time` 和 `logical_size` 均相同 | 写当前 metadata，引用 parent stream |
| 普通文件 | 同路径普通文件 | `write_time` 或 `logical_size` 不同 | 写完整 local stream |
| 普通文件 | 同路径非文件/父 stream 不可验证 | 类型或引用不合格 | 写完整 local stream |
| 目录 | 无/有 | 当前枚举结果 | 写当前 directory entry，无 stream |
| 父中存在 | 当前缺失 | delete/out of selection | tip Index 不写该 entry |

只要决定 local，本层保存整个普通文件主数据流，不读取父 payload 做 delta。Full 仍把全部普通文件写为 local。

### 6.3 规划资源上限

Parent lookup 与 current enumeration 使用磁盘 spool/有界索引，不把全部 entry 放入 `unordered_map`。建议按 path key
外排或使用有界 page lookup。所有 count、offset、logical bytes 使用 checked arithmetic。取消需贯穿
enumeration、parent lookup、content read、index write 和 finalization。

## 7. Archive 与 File Index 目标合同

FI0/FI1 直接修订当前 Personal Archive V7，不引入 V8 或旧 V7 fallback；产品未发布，旧 Archive 重建。

### 7.1 Header 与 metadata

- `file_set` 允许 FULL 或 INCREMENTAL；禁止 DIFFERENTIAL；
- FULL：`parent_uuid=0`；INCREMENTAL：`parent_uuid!=0`；
- 增加 critical capability `CAP_FILE_METADATA_BASELINE`，file Incremental 必须置位；
- file_set 始终 `CAP_HAS_FILE_INDEX`，始终禁止 volume Sidecar capability；
- 加密 metadata 增加 `selection_fingerprint`、`fingerprint_algorithm`、`change_detection_method`；
- Header/Catalog 记录 effective type；requested type 只属于控制面和 Job result，不伪装成 Archive 类型。

### 7.2 Entry 与 stream

目标 File Index 只允许：

```text
entry kind = root | directory | regular_file
stream kind = unnamed_main_data
content storage = local | parent
```

每个 regular file 恰有一个 main stream descriptor：

```text
stream_index
logical_size
content_storage

local:
  extent_count
  extents[]

parent:
  parent_stream_index
```

`parent` 只能用于 Incremental，且父 stream 必须属于 `Header.parent_uuid`。引用时当前/父 stream 的 path key、
stream kind 和 logical size 必须一致。禁止 parent 引用与 local extents 同时出现。

FI0 删除 `kReparse`、alternate stream、hard-link group、allocated/sparse ranges、sparse flags 和对应 platform
metadata tag。Reader 对超出当前 exact schema 的字段或枚举统一报 corrupt/unsupported current format，不解析旧语义。

### 7.3 Index 不变量

- tip Index 是完整当前 namespace；所有非 root entry 恰有一个可达父 directory；
- key 仍按 `(parent_entry_id, encoded_name, entry_id)` 规范排序；
- 非 null stable identity 在当前 Index 中唯一；FAT32 null identity 可重复；
- local extent 连续、不重叠、不越界，引用当前 Archive file stream chunk；
- parent stream index 在直接父 Index 中唯一存在且通过 stream kind/大小校验；
- 引用深度不超过 Repository chain limit；任何环或跨链引用拒绝；
- page、root digest、Footer count 和 Archive Group 完整性规则保持。

## 8. Chain Reader

新增面向 pipeline/application 的 `IFileRecoveryChain` 或等价组合，不让 pipeline 自行打开 Repository object。

### 8.1 打开

1. 从 tip Catalog/Archive identity 构造父 UUID 链；
2. 验证同 repository、同 backup set、content kind、父关系、Full root、无环和深度；
3. 打开每层 Archive Group，验证 Header/Footer/metadata；
4. 认证 selection fingerprint 相同；
5. 建立 layer UUID 到 reader 的只读映射；
6. 将 tip File Index 暴露为 browse namespace。

任何一步失败，不返回部分可用 chain。

### 8.2 查询与读取

目录分页始终查询 tip Index。读取 regular file 时：

```text
resolve(tip_layer, stream_index):
  local  -> validate extents and read current layer
  parent -> validate direct parent and recurse(parent_layer, parent_stream_index)
```

实现使用迭代或有界递归，并维护 `(layer_uuid, stream_index)` visited set。解密/解压/摘要失败不得向 Sink 返回未认证
内容。Range read 可以按最终 local extents 执行，但不得跳过中间父引用验证。

### 8.3 Verify

File chain Verify 至少执行：

- 每层 Archive Group 与所有 Index page 认证；
- 每层完整 parent/entry/stream graph validation；
- 全部 local payload 读取、认证和解压；
- 对 tip 每个普通文件递归解析到 local owner，核对最终 logical size；
- 检查无孤立、跨 backup set、悬空或循环内容引用。

单层 Verify 可用于诊断物理 Archive；面向用户的 Recovery Point Verify 必须验证可恢复的整条链。

## 9. Restore

恢复仍按 tip Index 计算选择闭包。preflight 在目标 mutation 前：

1. 打开并认证完整链；
2. 认证 tip Index、选择 entry 与全部后代；
3. 解析每个 selected file 的 parent stream 链到 local payload；
4. 确认 Archive 不含 reparse/hard-link/sparse/ADS 语义；
5. 校验目标 NTFS/ReFS/FAT32、安全路径、空间、冲突策略、security metadata 能力和单文件上限；
6. 绑定目标根 identity 并生成 durable preflight token。

写入阶段只通过 chain reader 读取逻辑主数据流。目录/普通文件 staging、flush、发布、metadata 和 partial restore
语义沿用基础设计。链中任何父 Archive 在 preflight 后发生 generation 变化，Start 时必须拒绝并重新 preflight。

## 10. Repository、Catalog 与删除

Catalog V2 当前 schema 直接演进：

- file_set `backup_type` 允许 `full|incremental`；
- file Incremental `parent_uuid` 必填，Full 为 null；
- `has_sidecar=false`、`source_volume_ids=[]` 保持；
- 增加固定长度 `file_selection_fingerprint`（可用编码摘要）和 `file_baseline_available`；
- exact key 集合、Reader、Writer、scanner 与所有消费者同步修改；不双读旧 V2。

Catalog fingerprint 是选父投影，Archive 认证 metadata 是权威。无 credential 扫描可重建 Header 身份/链；认证后
补全 fingerprint/baseline。若无法认证，不能选作新的 file Incremental 父层。

Deletion planner 使用现有 recovery graph：有后代时拒绝单删祖先；删除整段时后代先于祖先。Retention 必须保留
每个 retained tip 到 Full root 的闭包。本期不重写链，因此“只保留最近 N 个 RP”需要扩展为保留最近 N 个 tip
及其所需祖先，或按策略生成新 Full 后再淘汰旧链。

## 11. 不支持对象的检测边界

### 11.1 Backup

Windows snapshot enumerator 对每个当前 entry 执行：

- attributes/tag 检测 reparse；不打开 target，不递归；返回 `file_source.unsupported_reparse`；
- handle info 检测 `NumberOfLinks > 1`；返回 `file_source.unsupported_hard_link`；
- attributes/allocated semantics 检测 sparse；返回 `file_source.unsupported_sparse`；
- stream enumeration 确认只有 unnamed main `$DATA`；发现其它 `$DATA` stream 返回 `file_source.unsupported_ads`。

检测覆盖 Full 与 Incremental 的完整枚举，即使 metadata signature 判断该 entry 未变化也不能跳过。发生错误后
Abort Archive staging，不发布 Catalog。

### 11.2 Restore/Reader

Current-format Parser exact-schema 拒绝相关旧字段/枚举。Restore preflight 另做 defense-in-depth capability scan，确保
没有 alternate stream、sparse extent、duplicate non-null stable identity 或 reparse entry。拒绝发生在创建目录/staging 前。

### 11.3 不允许的捷径

- 不把 reparse target 当普通目录继续遍历；
- 不把 hard link 的每个名称备份为独立普通文件；
- 不读取 sparse logical zeros 并写成 dense file；
- 不只备份 unnamed stream 而忽略 ADS；
- 不用 warning、计数或 UI 提示把数据缺失包装为成功。

## 12. 安全、资源与可观测性

- 路径、名称、security descriptor 位于认证/加密边界，不进入 Catalog 或普通日志；
- 日志只记录 RP UUID、layer depth、entry/byte count、requested/effective type 和稳定 reason code；
- entry batch、parent lookup cache、content queue 和 index page 都有 byte/count 上限；
- parent Archive handle 数最多 128；实现可按层惰性打开，但 preflight 必须完成链身份认证；
- 所有 `offset+size`、count multiplication 和 byte total 使用 checked arithmetic；
- cancellation 在有界时间内中断 enumeration、stream read、page read、queue wait 和 finalize；
- partial/staging 文件、spool 与 Archive Group 由 RAII 清理；首卷与 Catalog 仍最后发布。

新增/调整的稳定码至少包括：

```text
file_backup.incremental_downgraded_full
file_backup.parent_chain_invalid
file_backup.selection_fingerprint_mismatch
file_backup.metadata_baseline_invalid
file_source.unsupported_reparse
file_source.unsupported_hard_link
file_source.unsupported_sparse
file_source.unsupported_ads
file_recover.parent_missing
file_recover.parent_reference_invalid
file_recover.chain_depth_limit
```

## 13. 故障与原子性

| 阶段 | 故障 | 行为 |
| --- | --- | --- |
| 选父/认证 | 父缺失、损坏、凭据失败 | 凭据失败使 Job 失败；结构/资格问题转 Full，记录 reason |
| baseline 资格 | fingerprint/method 不一致 | 转 Full，不接旧链 |
| 枚举 | 不可读/不支持对象/超限 | strict fail，Abort，无可见 RP |
| 内容读取 | short read/identity changed | fail，Abort；VSS 视图不允许 live retry |
| Index/finalize | spool 满/引用无效 | fail，Abort |
| Archive 发布 | 任一 member 失败 | 首卷不可见或进入现有收敛流程，不发布 Catalog |
| Catalog 发布 | 条件创建失败 | Archive 可由 scanner 重建；报告明确 outcome |
| Restore preflight | 链/引用/能力失败 | failed_before_write |
| Restore write | 目标盘满/取消 | 清 staging，已发布项计 partial_restore |

credential 无法打开父层不是 baseline 不足，不能静默转 Full 后绕过父认证；任务应返回 credential failure。这样避免
“错误口令仍产生新基线”的意外行为。

## 14. 完成标准

- Schedule 可请求 file Full/Incremental，UI 展示 requested/effective type 和 downgrade reason；
- metadata signature 场景生成父链正确的 Incremental；tip Index 包含完整当前树；
- no-change、mtime change、size change、新文件、删除、rename/move 的 browse、Verify、restore 人工核对符合本设计；
- 同大小同 mtime 修改被明确记录为本模式风险；
- selection 改变、父缺失会生成明确的新 Full，凭据错误会失败；
- reparse、hard link、sparse、ADS 在 Backup strict fail，在 Restore 写前拒绝；
- Chain Reader、Verify、Restore、Catalog rebuild、retention/delete 均理解 file chain；
- Debug/Release 受影响 production targets、静态/架构/格式/秘密检查通过；
- 没有项目测试资产，没有旧 Archive/Catalog/protocol/schema 兼容或 migration 路径。
