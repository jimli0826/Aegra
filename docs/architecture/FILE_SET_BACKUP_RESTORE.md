# 文件集备份与恢复设计

| 属性 | 内容 |
| --- | --- |
| 状态 | Full 基线已完成；Incremental 由 ADR-0018 接受，FI0–FI10 已完成 |
| 版本 | 1.0 |
| 日期 | 2026-08-07 |
| 范围 | Windows 个人版、本地 NTFS/ReFS、当前 Full 文件集备份与选择性恢复 |
| 权威格式/协议 | [V7](../format/PERSONAL_BACKUP_FORMAT_V7.md)、[Catalog V2](../format/PERSONAL_REPOSITORY_FORMAT_V2.md)、[Service V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)、[上限与码](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md) |

## 1. 目标与非目标

目标是在不破坏现有 Volume 块备份边界的前提下，使一个个人版 Recovery Point 能原子包含一个或多个文件/目录
选择，并支持 Repository 发现、完整 Verify、分页浏览和选择性恢复。

交付必须满足：

- Desktop 不直接执行文件系统数据访问，也不向高权限进程提交任意路径；
- 同一 Job 的所有源来自同一个 VSS Snapshot Set；
- 文件内容、层级和关键 Windows 元数据可恢复；
- 百万级条目不要求一次性驻留内存或落入单个 64 MiB metadata envelope；
- 任一关键条目失败不会产生可见 Recovery Point；
- `.bkf` 与 Repository 是恢复权威，SQLite 不是文件树权威；
- 所有长操作有取消、背压、确定的失败原子性和结构化进度。

首版明确不包含：UNC/SMB 源、EFS raw、Cloud Files 在线召回、application-consistent VSS writer 协调、
文件级 Incremental/Differential/去重、跨平台恢复、原位置系统文件覆盖、WinPE 文件恢复、Shell Extension 和
Archive 虚拟挂载。这些能力不能以隐藏开关或 best-effort 分支进入首版。

### 1.2 ADR-0018 范围更新

[ADR-0018](../adr/0018-file-set-incremental-usn-and-chain.md) 已接受文件 Incremental 的目标设计，实施状态与
工作包见[增量开发计划](../development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md)。本期文件 Backup/Restore
只支持目录、普通文件和未命名主数据流；reparse point、hard link、sparse file、ADS 均 strict reject。
本文后续关于保存或还原这四类对象的描述属于 ADR-0016 的旧范围，已被 ADR-0018 替代，不得作为实现依据。

### 1.1 未发布产品的版本策略

Aegra 尚未发布，设计不承担开发期数据兼容责任。V7 落地时直接替换 V6；Service V4、Worker schema 4、
Catalog V2 和新版 SQLite schema 也分别直接替换现有版本。生产代码只接受当前版本，版本不匹配统一返回
unsupported/corrupt 边界错误，不识别旧版本的具体结构，也不提供迁移、转换、alias、fallback、双协议或双格式
代码。旧开发数据不作为输入验收对象，应删除并重新生成。

## 2. 术语

| 术语 | 定义 |
| --- | --- |
| `volume_set` | 现有一个或多个 Volume 的块级保护对象 |
| `file_set` | 一个 Job 中有序、去重的文件/目录选择集合 |
| selection root | 用户明确选择的一个文件或目录根 |
| entry | 当前支持 root、目录或普通文件；其它 kind 拒绝 |
| stream | 当前仅支持普通文件的未命名主数据流 |
| file index | Archive 中分页、认证、可随机访问的文件树和 stream 映射 |
| node token | Service 文件浏览接口返回的短期 opaque 选择句柄 |
| durable selection | Service 创建 Schedule 时解析并保存的稳定选择描述 |

## 3. 当前基线与缺口

当前 `SourceKind` 只有 Volume，`JobRequest.source_refs[]` 表达 Volume/Archive 引用，Manifest 只包含
`disks[]` 和 `volumes[]`。`BackupPipeline` 读取连续 `IBlockSource`，Archive Chunk 的 `source_index`
对应 Volume index。现有实现可以复用：

- Archive Group partial、分卷、首卷最后发布和 Abort 清理；
- Zstandard、XChaCha20-Poly1305、Argon2id/HKDF；
- Repository Descriptor、Catalog、扫描、任务监督和进度事件；
- VSS Snapshot Set 生命周期和取消；
- Service/Worker Named Pipe、安全凭据解析和 SQLite 控制面。

不能直接复用的是文件系统枚举、文件 metadata、文件流地址空间、File Index、路径恢复和文件冲突策略。

## 4. 总体架构

```text
Desktop FileTreeModel
  -> Service V4 BrowseFileSources (paged opaque nodes)
  -> Service UpsertSchedule(FileSetSelectionInput)
  -> SQLite durable FileSelectionSpec + owner identity
  -> Service StartBackup / Scheduler
  -> Worker Job schema 4 (trusted FileSourceRef[])
  -> one VSS Snapshot Set for all involved volumes
  -> WindowsFileTreeSource
  -> FileSetBackupPipeline
  -> PersonalArchiveV7FileSetSession
  -> Repository publish + Catalog V2

Repository + Recovery Point
  -> Service V4 ListRecoveryPointEntries
  -> PrepareFileRestore (authenticated selection + target preflight)
  -> Worker FileSetRestorePipeline
  -> WindowsFileTreeSink (handle-relative staging and publish)
```

允许的依赖方向：

```text
apps -> application, adapters, personal_repository, pipeline
application -> contracts, ports, personal_repository
pipeline -> base, contracts, ports, format
adapters/windows_filesystem -> base, contracts, ports
adapters/personal_archive -> base, format, ports, crypto/compression algorithms
format, contracts -> base
```

`pipeline` 不 include Win32；Desktop 不 include `format`、`ports`、Repository、VSS 或 Windows file Adapter。

## 5. 保护对象与控制面模型

### 5.1 Tagged protection specification

Schedule 和 Job 不再用裸 `source_ids[]` 表达所有保护对象。Contracts 使用互斥 tagged union：

```text
ProtectionSpec
  kind = volume_set | file_set
  volume_set = VolumeSetSpec{source_ids[]}
  file_set = FileSetSpec{selections[], options}
```

`FileSelectionSpec` 至少拥有：

```text
selection_id                 stable UUID inside schedule
volume_identity              trusted stable volume identity
relative_components[]        normalized path components, never an absolute path
entry_kind                   file | directory
recursion                    self_only | recursive
unreadable_policy            fail_job (V1 fixed)
exclusion_rules[]            explicit normalized rules
display_label                non-authoritative UI label
```

FI0：删除 `reparse_policy`；枚举到 reparse/hard-link/sparse/ADS 时 Backup strict fail。

选择集合按 `(volume_identity, relative_components)` 规范排序并去重。若一个递归目录包含另一个选择，Service 删除
冗余子选择；同路径使用冲突规则则拒绝，不能依赖输入顺序决定行为。

### 5.2 SQLite authority

SQLite 保存 Schedule、owner identity 和 durable selection，但不是文件树或恢复内容权威。文件选择拆到
`schedule_file_selections` 表，不把可变 JSON 塞入 `schedules`：

```text
schedule_id, ordinal, selection_id, volume_identity,
relative_path_blob, entry_kind, recursion,
unreadable_policy, display_label
```

`relative_path_blob` 保存版本化组件编码，不保存 VSS path。表与 Schedule 在同一事务创建；Schedule 更新继续
冻结保护源和备份选项。产品未发布，直接更新正式 schema，不增加旧 schema 数据迁移分支。

### 5.3 身份与授权

- 浏览会话绑定 Named Pipe 调用者 SID、logon session、Service session 和到期时间；
- token 使用密码学随机 128 bit 以上，不包含可逆路径，单次 Service 生命周期内存维护；
- 创建 Schedule 前重新打开根并校验 token 指向对象仍在同一 Volume；
- Schedule 持久化 owner SID；只有 owner 或显式管理员策略可以查询或执行；
- 后台 Worker 使用 Service 已解析引用和所需 backup privilege，不继承 Desktop 句柄；
- 路径字段禁止进入 Command acknowledgement、TaskResult、普通日志和 Catalog。

## 6. Service V4 契约

V4 保持现有 Named Pipe framing 和 64 KiB frame 上限，但根 `schema_version`、API version 和所有消费者同步
升级为 4。现有 kind 数值保持业务含义，新 kind 从未占用范围分配：

| Kind | 类型 | 名称 | 关键输入/输出 |
| ---: | --- | --- | --- |
| 13 | Query | `BrowseFileSources` | parent token、分页；返回 node token、名称、kind、选择能力 |
| 14 | Query | `ListRecoveryPointEntries` | connection、RP、parent entry、分页、credential input；返回 child summaries |
| 15 | Query | `PrepareFileRestore` | RP、entry IDs、target node token、policy；返回 preflight token/统计 |
| 48 | Command | `StartFileRestore` | preflight token、confirmed、Archive credential input |

`UpsertScheduleCommand` 改为携带 `ProtectionSpecInput`。对于 `file_set`，创建请求携带 node token 和明确规则；
更新请求不得改变解析后的 selection。查询 Schedule 只返回 `selection_id + display_label + entry_kind`，不返回
内部 Volume identity 或路径。

`ListRecoveryPointEntries` 必须先认证 Archive metadata 与 File Index；continuation token 绑定 Repository
UUID、file UUID、index root digest、parent entry ID 和调用者。每页最多 100，结果稳定排序，token 不前进、
越界或 generation 改变时拒绝。响应不返回 Archive object key、分卷路径、stream offset 或 security descriptor。

`PrepareFileRestore` 产生短期 durable preflight record，至少绑定：

```text
repository UUID, recovery point UUID, index root digest,
selected entry IDs digest, target root identity,
conflict policy, logical bytes, entry count, expiration, owner identity
```

`StartFileRestore` 重新检查 Archive generation、index digest、目标 identity 和 token 唯一占用，防止 TOCTOU
或同 token 创建多个 Job。

## 7. Worker schema 4

`JobRequest` 增加互斥 typed payload：

```text
backup.source = VolumeSourceRefs | FileSourceRefs
restore.target = BlockRestoreTarget | FileRestoreTarget
```

File Source Ref 只在受限 Service-to-Worker Pipe 中存在，包含 canonical Volume GUID path、版本化相对组件、
规则和 selection ID。校验要求：

- 当前已完成 Worker 实现中，`operation=backup`、`content_kind=file_set` 时只允许 Full；FI7 将按 ADR-0018
  同步扩展 current schema 4 consumer，不新增兼容版本；
- File Source Ref 为 1..100 个，selection ID 与规范路径唯一；
- relative component 非空且不能包含分隔符、NUL、`.` 或 `..`；
- credential 仍只用 `SecretRef`；
- deadline、job/trace 关联和 response schema 保持现有严格行为。

File Restore Target 包含 Service 解析过的目标根、entry ID 集合摘要和冲突策略，不接受 Archive 内路径作为目标
系统路径。

## 8. 文件 Source/Sink Ports

新增 `ports/file_source.h`，按能力拆分：

```cpp
class IFileTreeEnumerator;
class IFileContentReader;
class IFileSnapshotView;
```

核心数据类型表达 opaque snapshot entry handle、stable entry/parent ID、entry kind、encoded name、logical size、
allocated ranges、stream descriptors、portable timestamps/attributes 和 bounded platform metadata envelope。

接口要求：Enumerator 按确定顺序分页返回拥有数据的 batch；Reader 打开时绑定 snapshot entry identity，生命周期内
size 稳定；stream read 支持 offset、短读、取消和 capability。Port 不暴露 `HANDLE`、
`std::filesystem::path`、Qt、VSS 或 Windows 常量。Windows 精确名称使用 tagged `windows_utf16le` component
bytes，Service 展示可以替换不可显示 code unit，但恢复使用认证原始 bytes。

新增 `ports/file_sink.h`：

```cpp
class IFileTreeSink;
class IStagedFileWriter;
```

Sink 构造时绑定已验证目标根目录句柄。所有操作使用相对 component，不接受拼接后的绝对字符串。能力包括目录
创建、普通文件 staging（unnamed main stream only）、冲突策略发布、metadata/security 应用、flush 和析构清理。
FI0：无 alternate stream / sparse range / hard link / reparse 写入 API 或 capability 位。
Sink 不决定恢复顺序；Pipeline 不决定 Win32 flags 和 rename 实现。

## 9. FileSet Backup Pipeline

Pipeline 分五阶段：

1. **Enumerating**：从 snapshot view 逐批枚举，验证父子关系、名称、唯一性和上限，计算 logical byte/item
   totals，并把有界 entry records 写入 session spool。
2. **Planning**：分配确定的 entry ID 与 stream index；目录先于子项；每 regular file 仅一个 unnamed main
   stream（FI0：无 hard-link group）。
3. **Reading**：按稳定次序打开 stream，按 chunk 大小读取，通过有界队列交给 Archive Session；sparse hole 不读。
4. **Indexing**：Session 把 entry、stream、extent 和 chunk reference 编成分页 File Index。
5. **Finalizing**：写索引根和 Footer，验证内部计数，Commit Archive Group；最后发布 Catalog。

不变量：

- 同一输入快照和策略产生确定的 entry/stream/chunk 顺序；
- 队列同时受 item count 和 payload bytes 限制；
- `processed <= total <= INT64_MAX`，累计使用 checked arithmetic；
- 任一 stream 读失败、metadata 失败、取消或索引失败都 Abort；
- 只有 Footer、全部分卷和索引完整后才能发布首卷；
- progress 在枚举完成前可以没有 byte percentage，但必须报告 phase 和已发现 item count。

## 10. Personal Archive V7

权威字节布局见 [`docs/format/PERSONAL_BACKUP_FORMAT_V7.md`](../format/PERSONAL_BACKUP_FORMAT_V7.md)。本节只固定架构要求。

V7 Header 保留固定大小、小端、magic/version/length/checksum 原则，增加 `content_kind`、root metadata locator、
first record locator 和 capability flags。Header 不保存文件名、源路径、条目数或未加密客户 metadata。

Archive record 是完整分卷边界，单个 record 不跨分卷。record kind 至少有 volume data chunk、file stream data
chunk、file index leaf/internal page 和 footer。每个 Index page 有 magic、版本、kind、page ID、encoded/plain
size、保护模式和摘要；加密模式另有 nonce/tag。解密或分配前验证长度；加密模式认证后、未加密模式完成摘要
校验后再解析。页引用必须无环、无重复、位于声明 index region，并受最大深度和最大页数限制。

File Index 使用按 `(parent_entry_id, encoded_name, entry_id)` 排序的分页 B+tree。Entry value 至少包含：

```text
entry_id, parent_entry_id, kind, encoded_name,
logical_size, stream descriptors, timestamps, portable attributes,
bounded platform metadata (security only; FI0 removed hard_link_group/sparse/ADS/reparse),
content extents(chunk_index, block_entry, file_offset, logical_size)
```

根 entry 的 parent 为 null；每个 selection root 是根的直接子项并带 selection ID。所有非根 entry 必须恰有
一个可达父项。Stream index 唯一，extent 连续、不重叠、不越界，引用 Chunk 必须为 `file_stream` 且 source
index 匹配。

Footer 保存全 Archive 计数、每类 record 计数、logical/stored bytes、index root locator、page count、index
root digest 和分卷完整性字段。Reader 先验证分卷和 Footer，再读取/认证 root metadata 与 Index。

分卷发布仍为 Sidecar/续卷先、首卷最后。File V1 不生成 `.bhx`，Catalog `has_sidecar=false`；未来文件增量
不得复用 Volume Sidecar 语义。

## 11. Windows 文件语义

### 11.1 一致性和枚举

- 解析 selection 涉及的 canonical Volume；拒绝网络、FAT/RAW 和无法稳定识别的源；
- 所有 Volume 在一个 VSS Snapshot Set 中创建 snapshot；任一失败则 Job 不开始写 Archive；
- 枚举从 snapshot root 开始，使用 handle identity 防止名称重解析；
- 使用 no-follow/open-reparse-point 方式获取 link 本身，不遍历 target；
- 保持显式最大深度和 identity 循环防护；
- 文件排序基于 encoded name 的稳定二进制顺序，不使用 locale collation。

### 11.2 Metadata

首版必须支持 directory、regular file、creation/access/write/change timestamps、Windows attributes、directory
case-sensitive flag、owner/group/DACL/SACL self-relative descriptor、main stream、ADS、sparse ranges、hard link
identity 和 reparse data no-follow。

SACL 策略已冻结（ADR-0016）：启用 `SeBackupPrivilege`/`SeSecurityPrivilege` 后仍无法读取完整 security
descriptor 则 strict failure（`file_source.security_descriptor_unreadable`）。目标缺 ADS/sparse/ACL/reparse
能力时 preflight 拒绝（`file_restore.target_capability_missing`），无 lossy 策略。EFS、offline cloud
placeholder 和 unsupported reparse 返回稳定 `file_source.unsupported_*`。

## 12. FileSet Restore Pipeline

恢复先计算选择闭包：目录包含全部可达后代；hard-link 组至少选择一个 materialized source；stream extent
引用必须全部可达。随后按固定顺序执行：

1. 认证 Archive、File Index 和选择闭包；
2. 验证目标根、容量、冲突策略和目标文件系统能力；
3. 创建目录骨架，但暂缓最终目录 ACL/时间；
4. 写普通文件 staging、stream 和 sparse layout，校验 logical size；
5. 发布普通文件；
6. 创建其余 hard link；
7. 创建 reparse object；
8. 应用文件 metadata；
9. 自底向上应用目录 metadata；
10. flush、生成结果统计并清理 staging。

失败后清理未发布 staging。已发布项目可能保留，因此 TaskResult 区分 `failed_before_write` 与
`partial_restore`，列出计数但不返回客户路径。重试使用新的 preflight token；`replace` 重试仍重新检查目标。

## 13. Repository 与 Catalog V2

Catalog V2 增加 `content_kind`、`source_count`、可选认证后 `file_entry_count`、logical size 和
`format_version=7`。Catalog 不保存 selection path、文件名、目录摘要、ACL、owner SID 或 File Index locator。

扫描无凭据时从 V7 Header 重建 `content_kind` 和结构状态；认证后补全计数。File Recovery Point 不参与
Volume geometry/Sidecar 逻辑，首版只允许 `parent_uuid=null`、`has_sidecar=false`。

Verify 必须认证每个 File Index page、父子图、stream extent、所有引用 Chunk，并读取、认证和解压每个 payload。

## 14. 安全与资源上限

格式和产品配置至少定义以下上限：selection roots、目录深度、单目录 children、总 entries、name bytes、ADS
count/name、platform metadata bytes、stream logical size、总 logical bytes、sparse ranges、index page size/count/
depth、chunk stored/logical size、extent count、split parts 和恢复选择数量。

任何 `offset + size`、count multiplication 和 logical total 使用 checked helper。认证失败、超限和非法引用必须
在目标写入前失败。日志禁止输出文件内容、完整选择列表、security descriptor、SecretRef、token 或密码。

## 15. 进度与可观测性

新增稳定 message codes：

```text
file_backup.enumerating
file_backup.snapshotting
file_backup.reading
file_backup.indexing
file_backup.finalizing
file_backup.completed
file_restore.preflighting
file_restore.writing
file_restore.metadata
file_restore.completed
```

允许指标：discovered/processed entries、logical/stored bytes、stream count、sparse bytes、throughput、index pages、
elapsed time 和 warning code count。默认日志不逐文件记录路径。

## 16. 完成标准

- Full file_set 从 Desktop 创建 Schedule、启动、观察进度并生成可发现 V7 Recovery Point；
- Service 能分页浏览认证后的文件树，Desktop 不打开 Archive；
- 用户能选择文件/目录恢复到新目标，并获得明确冲突/部分恢复结果；
- 普通文件、空文件、目录、ACL、ADS、sparse、hard link、reparse no-follow 经人工核对一致；
- 百万级 metadata 使用有界内存和分页索引，不依赖单 CBOR envelope；
- Volume V7 的现有备份、Verify、恢复、分卷和增量能力完成等价人工回归；
- 所有生产 Target、架构/静态检查和文档同步完成；没有项目测试代码或兼容分支。
- 搜索确认不存在 V6/V7 dual-read、V3/V4 negotiation、旧字段 alias、旧 SQLite upgrade 或旧数据转换路径。
