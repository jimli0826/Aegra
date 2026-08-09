# `format` 模块开发文档

## 目标

实现纯格式模型与编解码，不感知文件路径、网络、数据库、VSS、虚拟化平台或 UI。

## 子模块

### `manifest`

定义可扩展的 `Manifest`、Disk、Partition、Volume、Extent、SystemInfo、BackupJob 和 provider extension envelope。物理机、虚拟机、个人 Archive 与企业 Repository 共享逻辑模型；虚拟化专有字段进入命名 extension，不进入个人格式的固定二进制头。

### `personal_archive`

实现个人版 `.bkf` Header、Archive Record、加密 CBOR Metadata、Volume/File Chunk、File Index page、
BlockEntry、Footer 和 `.bhx` Sidecar。唯一权威规范是[个人版 V7 格式](../format/PERSONAL_BACKUP_FORMAT_V7.md)。

### `enterprise_repository`

实现 Pack、Pack Local Index、Index Segment/Root、Recovery Point Manifest/Commit、Catalog、Tombstone 和 Maintenance Checkpoint 的编码。该子模块不依赖 PostgreSQL。

## 依赖

只依赖 `base`，必要时依赖纯数据 `contracts`。压缩、加密和 I/O 由调用者或独立 transform/port 提供。

## 不变量

- 所有格式有 magic、版本、固定字节序、长度和完整性校验。
- 解析前检查所有范围和整数溢出。
- `.bkf` CBOR Map key 只允许固定 `snake_case` text string。
- 分卷 `.bkf` 只在完整 chunk 边界切换；只有首卷保存 CBOR，只有末卷保存 Footer。
- 未识别关键版本拒绝；未识别可选扩展按 schema 规则跳过。
- Repository Pack 足以通过 Footer 重建 Local Index。
- Commit Object 是 Recovery Point 可见性的唯一提交标记。

## 验证

- 格式审查覆盖 Golden bytes、roundtrip、最大/最小边界和跨分卷顺序读取规则。
- 人工构造的非生产样本验证截断、越界、重叠、重复 ID、非法 key、损坏校验值、缺卷和乱序拒绝。
- 固定结构大小常量、字段 offset 和 endianness 必须与权威格式文档逐项核对。

## 当前状态

F2（Personal Archive V7 + Catalog V2）已实现：

- `format_version=7` / `header_version=2` Header：`content_kind`、`capability_flags`、
  `first_record_offset`；拒绝非 V7 与未知 capability bit；
- 统一 `ArchiveRecordPrefix`（`MYBKREC`）包装 volume chunk、file stream chunk、index page 与 Footer；
- Volume chunk 仍为 96 字节 kind 头；AAD = Header ‖ RecordPrefix ‖ ChunkHeader(tag=0) ‖ BlockEntry[]；
- Volume BlockEntry 区分 ZERO 与 FREE：ZERO 恢复写零；FREE 表示空闲簇或显式系统文件排除区，
  不含 payload，恢复端跳过写盘；Sidecar 使用 DATA/ZERO/FREE 精确状态；
- ADR-0022 冻结 volume_set 单 `VolumeChunk` DEDUP：只后向引用同 record 的 RAW/COMPRESSED canonical，
  禁止跨 chunk/part/source/archive；Footer 记录 DEDUP block/bytes；
- Footer 为 512 字节完整 record（prefix + body），含 file/index 统计与 index root 定位；
- File Index page header codec 与 leaf/internal CBOR body codec（`file_index.h`）；
- Manifest CBOR schema 1，根字段 `content_kind`；file_set 禁止 disks/volumes；
- AEAD HKDF info 升级为 `MYBACKUP-V7-*`；
- Catalog Entry schema 2：`content_kind`、文件统计、volume dedup 统计与 `format_version=7`。

Adapter 侧 volume session/reader 已按 V7 record 边界写读；`PersonalFileArchiveSession` 支持
多层 B+tree Index 写入（Namespace + Entry ID / Stream / Chunk 二级索引，ADR-0019）；
`PersonalFileArchiveReader` 按 L31/ADR-0019 以 Footer root 认证 + 有界 LRU page cache 打开
（O(1) 页 I/O），二级 B+tree 分页 `list_children` / `describe_entry` / stream 范围读取。
DEDUP 的格式与拒绝规则已冻结，Writer/Reader 实施状态以
[Volume Set 去重设计](../architecture/VOLUME_SET_DEDUPLICATION.md) 的工作范围为准；多目标 Restore 仍是后续工作。

## 完成标准

格式库可在没有文件系统、数据库、Windows SDK 和网络的生产 Target 中独立构建。
