# ADR-0021：FAT32 文件集备份源与恢复目标

- 状态：Accepted
- 日期：2026-08-09
- 决策者：Aegra Maintainers
- 关联模块：contracts、ports、pipeline、windows_filesystem、service、worker、desktop

## 背景

ADR-0020 已将文件 Incremental 的变化判断改为同路径 `write_time + logical_size`，不再依赖 NTFS USN。
现有 Windows 文件 Adapter 仍把 NTFS/ReFS 当作固定前提，要求每个条目具有 `FILE_ID_128` 和完整 Windows
security descriptor，因此 FAT32 即使可以由 VSS Provider 创建快照也会在枚举阶段被拒绝。

FAT32 没有 NTFS ACL、ADS、hard link、sparse file 和同等稳定 File ID 合同，并且单文件最大长度为
`4 GiB - 1`。这些差异必须作为文件系统能力表达，不能伪造身份、ACL 或静默越过目标上限。

## 决策

1. 文件集备份源支持 NTFS、ReFS 和 FAT32。所有源仍必须进入同一个 VSS Snapshot Set；不增加 live-volume
   文件枚举 fallback。VSS Provider 不支持该 FAT32 卷时任务失败且不写出 Recovery Point。
2. Windows filesystem Adapter 在打开 snapshot/target root 时查询能力，不再使用 NTFS/ReFS 布尔门禁。
3. NTFS/ReFS 条目继续保存 `(volume_identity, FILE_ID_128)` 和 Owner/Group/DACL/SACL。FAT32 条目的
   `stable_identity` 使用合同既有 null 表示，`platform_metadata` 为空且不置 security flag；不生成路径哈希身份。
4. FAT32 不执行 hard-link、ADS 或 security descriptor 探测；reparse、sparse 和 EFS 的通用拒绝保持不变。
5. Incremental 继续仅按规范路径、`write_time` 和 `logical_size` 判断复用。FAT32 较粗的 mtime 精度是明确
   已知限制：同路径文件若内容变化但时间和大小均未变化，可能复用父层内容。
6. 恢复目标支持 NTFS、ReFS 和 FAT32。FAT32 Sink 声明 `supports_security_descriptor=false` 和
   `maximum_file_size_bytes=0xFFFFFFFF`。请求恢复 ACL 时预检失败；关闭 ACL 后恢复时间戳、属性和主数据流。
7. Service、Pipeline 与 Sink 都检查单文件上限。超限使用稳定码
   `file_restore.target_file_too_large`，并在任何目标写入前失败。
8. 不改变 Personal Archive V7 或 Service V4 wire 字段集合；既有 nullable stable identity 和
   `restore_security` 已足够表达本决策。产品未发布，不增加旧数据迁移或兼容分支。

## 备选方案

- **直接读取在线 FAT32：** 无法满足同 Job snapshot 一致性，拒绝。
- **为 FAT32 合成路径哈希 File ID：** rename 后不稳定且会伪装成文件系统身份，拒绝。
- **内容哈希作为变化判断：** 可降低粗时间戳风险，但要求读取所有文件，违背本期 metadata signature 范围。
- **恢复 ACL 时自动忽略：** 会造成未明确授权的元数据丢失；必须由用户关闭 ACL 恢复。

## 影响

- FAT32 可参与 Full/Incremental 文件集备份，但能否创建快照仍由 VSS Provider 决定。
- FAT32 Archive 条目没有稳定身份和安全描述符；路径仍是 namespace 与增量匹配权威。
- FAT32 恢复适合普通文件树，不适合要求 Windows ACL 保真的归档，也不能容纳超过 4 GiB - 1 的文件。
- exFAT、FAT12/FAT16、RAW、UNC 与 live-volume 文件源仍不在本决策范围。

## 验证

- 构建 Debug/Release 生产 Targets 并运行 source-limit/architecture 检查。
- 人工在可创建 VSS snapshot 的 FAT32 卷执行 Full 与 Incremental，确认 Archive 条目 identity/security 为空。
- 人工验证 FAT32 同路径同 mtime/size 复用及 mtime/size 改变时写入 local stream。
- 人工验证 FAT32 目标：ACL 开启时预检拒绝，关闭后恢复成功，超过单文件上限时写入前拒绝。

