# `format` 模块开发文档

## 目标

实现纯格式模型与编解码，不感知文件路径、网络、数据库、VSS、虚拟化平台或 UI。

## 子模块

### `manifest`

定义平台无关的 `BackupManifest`、Protected Object、Stream、Extent、Consistency、Integrity 和 provider metadata envelope。物理机、虚拟机、个人 Archive 与企业 Repository 共享逻辑模型。

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

## 测试

- Golden files、roundtrip、最大/最小边界和跨分卷顺序读取。
- 截断、越界、重叠、重复 ID、非法 key 和损坏校验值。
- 缺卷、乱序、重复卷、卷间 UUID 不一致和末卷 Footer 缺失。
- Fuzz 入口覆盖每个外部解析器。
- 结构 size、offset、endianness 静态断言。

## 完成标准

格式库可在没有文件系统、数据库、Windows SDK 和网络的单元测试中运行。
