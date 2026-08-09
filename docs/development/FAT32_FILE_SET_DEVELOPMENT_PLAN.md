# FAT32 文件集支持开发计划（FS1-FS5）

| 属性 | 内容 |
| --- | --- |
| 状态 | 实现完成，人工 FAT32/VSS 场景待隔离环境验收 |
| 日期 | 2026-08-09 |
| 决策 | [ADR-0021](../adr/0021-fat32-file-set-source-and-restore.md) |
| 范围 | FAT32 VSS 文件源、nullable identity、能力降级恢复、Service/Desktop 语义与发布验证 |

## 不变量

- 文件备份只读 VSS snapshot，不直接扫描在线 FAT32 卷。
- 不伪造 FAT32 File ID，不保存不存在的 ACL，不静默忽略用户要求恢复的 ACL。
- Incremental 只使用同路径 `write_time + logical_size`，不引入 USN、内容哈希或 FAT 专用 baseline。
- exFAT、FAT12/FAT16、RAW 与 UNC 不随本计划进入文件集支持范围。
- 不新增测试代码；按仓库策略使用生产构建、静态检查和人工场景验证。

## FS1：文件系统能力合同

- 以 `FileSystemCapabilities` 替换 windows_filesystem 内部 NTFS/ReFS 布尔门禁。
- NTFS/ReFS：security、stable File ID、hard link 与 ADS 探测保持开启，单文件上限为 u64。
- FAT32：上述能力关闭，单文件上限为 `0xFFFFFFFF`。
- `StableFileIdentity` 的既有 null 值允许出现在普通目录/文件条目中。

验收：Adapter 可打开 NTFS/ReFS/FAT32 root，仍拒绝其它文件系统。

## FS2：FAT32 Snapshot 枚举

- `WindowsFileSnapshotView` 把每卷能力传给 Enumerator。
- FAT32 跳过 FileIdInfo、hard-link、ADS 和 security descriptor 探测，写出 null identity/空 metadata。
- NTFS/ReFS 保持原严格行为；reparse/sparse/EFS 仍统一拒绝。
- 仅在任一卷需要 ACL 时申请 `SeSecurityPrivilege`。

验收：FAT32 VSS snapshot 可枚举和读取主数据流；混合 NTFS/FAT32 snapshot set 的条目语义各自正确。

## FS3：Service 与 Desktop 语义

- Worker 继续要求每个文件源由 VSS Provider 支持，不增加 raw/live fallback。
- Desktop 增量说明披露 FAT32 粗 mtime 风险；恢复说明披露 FAT32 需关闭 ACL。
- 新增稳定错误码的 message map 与五语言资源。

验收：用户看到的变化判断、ACL 限制和错误原因与生产行为一致。

## FS4：FAT32 恢复目标

- `FileSinkCapabilities` 增加 `maximum_file_size_bytes`。
- FAT32 Sink 不要求 `SeSecurityPrivilege`，声明无 ACL 能力并限制单文件 4 GiB - 1。
- Service 选择闭包计算最大文件；Service、Pipeline、Sink 三层在写入前拒绝超限。
- `restore_security=true` 且目标无 ACL 能力时返回 `file_restore.target_capability_missing`；关闭后恢复
  时间戳、属性和主数据流。

验收：FAT32 目标的能力拒绝与普通恢复均符合 ADR-0021，且没有 partial staging 泄漏。

## FS5：发布验证

执行：

1. `scripts\\build.cmd Debug`
2. `scripts\\build.cmd Release`
3. `git diff --check`
4. Repository source-limit/architecture checks
5. 隔离 FAT32 卷人工矩阵：Full、Incremental 未变/mtime 变/size 变、混合卷 VSS、ACL on/off、
   `0xFFFFFFFF` 边界与超限拒绝、取消和 VSS 失败无可见 Recovery Point。

交付记录必须区分已自动验证项目与需要真实 FAT32/VSS 环境的人工项目。

