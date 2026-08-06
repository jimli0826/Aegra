# `format` 模块开发文档

## 目标

实现纯格式模型与编解码，不感知文件路径、网络、数据库、VSS、虚拟化平台或 UI。

## 子模块

### `manifest`

定义可扩展的 `Manifest`、Disk、Partition、Volume、Extent、SystemInfo、BackupJob 和 provider extension envelope。物理机、虚拟机、个人 Archive 与企业 Repository 共享逻辑模型；虚拟化专有字段进入命名 extension，不进入个人格式的固定二进制头。

### `personal_archive`

实现个人版 `.bkf` Header、加密 CBOR Metadata、Chunk、BlockEntry、Footer 和 `.bhx` Sidecar。唯一权威规范是[个人版 V6 格式](../format/PERSONAL_BACKUP_FORMAT_V6.md)。

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

阶段 3 已实现：

- `Manifest` 的字符串键 CBOR 编解码和引用/唯一性校验；
- `BackupHeader`、`CborMetadataEnvelopeHeader`、`ChunkHeader`、`BlockEntry`、`BackupFooter` 的显式小端编解码；
- 整数 CBOR Map key、非法 magic/版本/尺寸、未加密 metadata envelope 和非法 BlockEntry 的拒绝路径。

当前实现不使用 packed C++ struct 直接映射外部字节。`.bhx` Sidecar 采用固定 96 字节头和显式小端 payload codec；DATA 使用 SHA-256，ZERO/SKIP hash 全零。V6 Header codec 区分非分卷、首卷和续卷规则，并校验全量/增量 `parent_uuid`。ChunkHeader 固定为 96 字节，保存独立 XChaCha20-Poly1305 nonce/tag；tag 清零的 Header 和全部 BlockEntry 作为 AAD。Adapter 已实现 Chunk Payload 认证加密、完整 chunk 边界分卷、末卷 Footer、多 Volume 全量/增量 Archive（父层有序 volume 几何与 Sidecar 块表匹配）、稀疏增量层和多 Volume 链式覆盖读取。DEDUP 写入、差异备份和多目标（多 volume 显式映射）Restore 仍是后续工作。

## 完成标准

格式库可在没有文件系统、数据库、Windows SDK 和网络的生产 Target 中独立构建。
