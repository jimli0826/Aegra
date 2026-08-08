# ADR-0018：文件集增量备份的 USN 基线与链式恢复

- 状态：Accepted
- 日期：2026-08-08
- 决策者：Aegra 项目
- 关联模块：contracts、ports、format、pipeline、personal_repository、adapters/windows_filesystem、apps/worker、apps/service、apps/desktop
- 替代范围：ADR-0016 中“文件级 Incremental 暂缓”的范围，以及 reparse、hard link、sparse、ADS 属于文件集备份能力的决定
- 关联文档：[增量架构设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md)、[增量开发计划](../development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md)、[文件集基础设计](../architecture/FILE_SET_BACKUP_RESTORE.md)

## 背景

Full `file_set` 已能以 Personal Archive V7 保存完整文件树。计划任务需要在同一 backup set 中生成文件级
Incremental Recovery Point，同时保证：

- 不因时间戳精度、时钟变化或 metadata 未更新而漏掉内容变化；
- 每个增量 Recovery Point 可独立分页浏览当时的完整命名空间；
- 删除、重命名和移动不依赖恢复时重放易错的事件日志；
- 父层内容缺失、USN Journal 不连续或索引损坏时不生成未经证明的增量层；
- 恢复、Verify、删除和保留策略明确理解文件链。

本期产品范围同时收缩：不支持 reparse point、hard link、sparse file 和 alternate data stream（ADS）的备份
或还原。当前 V7 文档与部分接口曾为这些能力预留字段和分支。产品尚未发布，不需要保留这些开发期格式、字段、
能力声明或读取 fallback。

## 决策

### 1. 增量层模型

文件增量采用：

> 完整的当前命名空间 File Index，加上仅属于新增或内容变化普通文件的本层主数据流。

每个增量 Archive：

- Header `backup_type=incremental`，`parent_uuid` 指向同一 backup set 的直接父 Recovery Point；
- File Index 包含当前时点全部目录和普通文件，删除项直接缺席，重命名/移动以当前父子关系表达；
- 新文件、内容变化文件在本层保存完整主数据流；本期不做块级 delta 或跨文件内容去重；
- 未变化文件和仅 metadata 变化文件的 stream 引用直接父层的对应 stream；
- 父引用只允许指向直接父层，不允许任意祖先 UUID。Reader 递归解析父引用；
- 每层都是独立认证的 `.bkf` Archive Group，file_set 仍不生成 `.bhx`。

增量层不是事件重放日志。USN 只用于证明变化集合，tip File Index 始终是该 Recovery Point 的权威当前树。

### 2. 稳定文件身份与 USN 基线

File Index 保存加密的稳定文件身份，语义为 `(volume_identity, FILE_ID_128)`。身份仅在同一 Volume 上比较，
不能把路径、名称或时间戳当作身份。

每个参与 Volume 的父 Recovery Point metadata 保存：

```text
volume_identity
journal_id
next_usn
```

当前任务从 VSS snapshot-consistent 视图取得 journal state，并且仅在以下条件全部成立时允许 Incremental：

```text
current.journal_id == parent.journal_id
current.lowest_valid_usn <= parent.next_usn
parent.next_usn <= current.next_usn
```

Worker 读取半开区间 `[parent.next_usn, current.next_usn)`。USN create/delete 记录必须参与判断，以识别删除和
file ID 重用。任何未知或歧义状态都不能被分类为“未变化”。

### 3. Incremental 资格与自动 Full

开始写 Archive 前，父层选择与 USN 资格必须全部验证：

- 父层 `content_kind=file_set`、同一 backup set、链完整且未超过深度上限；
- 父层选择 fingerprint 与当前 Schedule 的不可变选择 fingerprint 完全相同；
- 父层 File Index 和必要 metadata 已认证；
- 所有 selected Volume 的 identity 与 journal checkpoint 可用且连续；
- 请求只包含本期支持的普通目录/普通文件主数据流。

任一条件失败时，不以 timestamp、size 或 directory enumeration 猜测增量。任务改为生成有效 Full，设置
`parent_uuid=0`，并保留原 backup set UUID。控制面分别持久化和展示 `requested_backup_type=incremental` 与
`effective_backup_type=full`，以及非敏感 downgrade reason code。

这是“同一次请求的安全 Full”，不是任务失败；只有无法完成 Full 时任务才失败。

### 4. 变化分类

USN 原因用于保守分类：

- 内容变化：data overwrite、extend、truncation，以及 file create/file ID reuse；
- 命名空间变化：create、delete、rename、move；
- metadata-only：basic information 或 security change，且没有内容变化原因；
- 无关变化：已证明不影响所选树、entry metadata 或主数据流的记录。

当前 snapshot 仍完整、有界地枚举全部选择树，以构建完整 File Index、核对身份和检测不支持对象。若 USN 记录
丢失语义、原因组合未知、身份无法关联或父 entry 不完整，则该文件在本层保存完整内容；不得引用父层。

### 5. 明确不支持的文件对象

本期 Backup 和 Restore 均不支持：

- reparse point，包括 symbolic link、junction 和其它 reparse object；
- `NumberOfLinks > 1` 的 hard-linked file；
- 设置 sparse 属性或具有 sparse allocation 语义的文件；
- 除未命名主 `$DATA` 之外的 ADS。

备份枚举必须检测上述对象。发现任一对象时整个 Job strict fail 并 Abort，不跟随、不扁平化、不展开为 dense、
不复制成独立 hard-link 内容，也不忽略 ADS。恢复 Reader/preflight 必须在第一次目标 mutation 前拒绝任何含上述
语义的 Archive entry。

由于产品未发布，FI0 直接删除当前未使用的相关格式字段、枚举值、Port 方法、Pipeline 分支和虚假 capability。
旧开发 V7 Archive 不迁移、不双读、不转换，直接删除后重新生成。

### 6. 链式读取、恢复与 Verify

File chain reader 以 tip Index 作为目录查询权威，并把 stream 读解析为：

```text
local  -> 读取当前层 stream extents
parent -> 打开直接父层的 parent_stream_index，再递归解析
```

Reader 必须拒绝父 UUID 不匹配、跨 backup set、循环、深度超限、stream identity/type/size 不一致、悬空父引用
以及任何未认证层。恢复 preflight 在目标写入前认证完整所需链和全部被选 stream 引用。Verify 校验每层格式、
父关系、Index 图、payload，并实际递归解析所有 tip stream。

### 7. Repository、Catalog 与保留

Catalog 当前版本直接修改，不引入兼容版本。file_set 允许 Full 和 Incremental，Incremental 的 `parent_uuid`
必须存在。Catalog 增加非敏感选择 fingerprint/基线可用性摘要，用于父层筛选；认证数据仍以 Archive 为权威。

删除与保留策略保持链感知：有保留后代时不得删除祖先；删除链按后代到祖先执行。本期不做 chain merge、rebase、
squash 或合成 Full。

计划任务沿用 Schedule backup set。若因 USN 不连续产生有效 Full，该 Full 成为后续 Incremental 的新基线。

## 备选方案

- **只比较 path、size、mtime：** 无法证明内容未变化，可能静默漏备，不采用。
- **只保存变化项并在恢复时重放 create/delete/rename：** 浏览和恢复依赖全链事件重放，错误面与内存上限更难控制，不采用。
- **每个 entry 引用任意祖先 UUID：** 减少引用跳数，但扩大索引信任面、删除规划和对象定位复杂度，不采用。
- **USN 不可用时做扫描式增量：** 完整扫描仍不能仅凭普通 metadata 证明内容未变，不采用；改为有效 Full。
- **变化文件做块级 delta：** 本期实现与验证成本高，先采用 whole-file replacement。
- **静默跳过或有损保存四类对象：** 会产生表面成功但不可完整恢复的 Recovery Point，不采用。

## 影响

- 每个增量层的 File Index 大小与当前 entry 数量相关，不只是变化数；换取任意时点直接分页浏览和明确删除语义。
- changed large file 会在本层完整保存，空间效率低于 block delta，但完整性和实现边界清晰。
- Restore/Verify 从单 Archive Reader 升为链 Reader；Repository 凭据和对象生命周期覆盖整条链。
- Schedule、Job、Catalog、SQLite 和 UI 需要同时表达 requested/effective type 与 downgrade reason。
- Archive/Catalog/协议的当前版本原地演进；开发期数据全部重建，无兼容实现。

## 验证

- 构建所有受影响 production targets，并运行静态、架构、格式、秘密与 `git diff --check` 检查；
- 在隔离 NTFS/ReFS 数据上人工验证 Full→Incremental、连续多层、创建/修改/metadata-only/删除/rename/move；
- 人工制造 journal reset/wrap/禁用、父层缺失、selection 改变，确认生成明确标记的有效 Full；
- 人工损坏父 UUID、Index parent stream reference、payload 和 journal checkpoint，确认 Verify/Restore 写前拒绝；
- 人工创建 reparse、hard link、sparse、ADS，确认 Backup strict fail；人工构造含相关字段的开发 Archive，确认 Reader 写前拒绝；
- 不新增测试源码、fixture、脚本、test executable、CTest 或其它项目测试资产。
