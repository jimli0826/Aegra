# Windows 文件集备份 Composition Root（Worker）

## 目标

在个人版 Worker 中执行 schema 4 `content_kind=file_set` 的 Full 备份 Job，生成并提交 V7 `file_set`
Recovery Point（`.bkf`）。本模块是 composition root：装配 VSS、Windows Filesystem Adapter、
FileSet Pipeline 与 Personal File Archive Session，不实现文件枚举或格式编解码算法。

## 允许依赖

- `contracts` / `ports` / `pipeline` / `format`
- `Aegra::AdapterWindowsVss`、`Aegra::AdapterWindowsDisk`、`Aegra::AdapterWindowsFilesystem`
- `Aegra::AdapterPersonalArchive`
- Worker Host / 协议 / 任务日志（`apps/worker`）

## 禁止

- Desktop、Qt、Service 控制面数据库
- 在 Pipeline 内 include Win32
- 记录路径、相对组件、SecretRef 或明文凭据到 TaskResult / 普通进度事件
- file_set Differential / raw 无 VSS 回退（Incremental 合同见 FI1；编排见 FI7）

## 执行顺序

```text
validate JobRequest (schema 4, file_set, full|incremental + selection_fingerprint)
  -> resolve SecretRef (DPAPI)
  -> unique volume_identity list
  -> one VSS Snapshot Set for all volumes (strict; no raw fallback)
  -> WindowsFileSnapshotView (snapshot root UTF-16 bindings; VSS device path must end with
     '\\' so CreateFile opens the root directory, not a raw volume handle)
  -> PersonalFileArchiveSession (index spool under data_dir/staging/job-<id>/index-spool)
  -> FileSetBackupPipeline
  -> destroy view/session -> close VSS -> remove staging tree
```

销毁顺序固定：pipeline 返回后先释放 snapshot view 与 session（session 在失败路径 Abort），再关闭
VSS，最后删除 Job 私有 staging 目录。

## 公共入口

| 符号 | 说明 |
| --- | --- |
| `execute_windows_file_set_backup_task` | Host 调用的任务入口；与 volume 任务共用 options/context 类型 |
| `detail::backup_windows_file_set` | 数据面装配（VSS + Adapter + Pipeline + Archive） |

## Wire

- Job `schema_version = 4`，`content_kind = 2`，`operation = 1`（backup）
- `file_source_refs[]`：与 Service `encode_supervisor_job_request` 对称的字段集
- `source_refs` 必须为空数组；`target_ref` 为 Archive 目标路径
- 成功 `TaskResult` 填 `entry_count` / `stream_count` / 字节计数；message code 使用
  `backup.completed` 或 `backup.completed_with_warning`（仅 VSS 清理失败）

## 日志

任务日志 `operation=backup`。允许记录 `selection_id` 与 destination 路径；禁止相对路径组件、
文件名和凭据。阶段：`resolve_credentials` → `prepare_sources` → `create_vss_set` →
`open_snapshot_view` → `create_archive` → pipeline 内部进度。

## 验证

- 构建：`aegra_app_worker_personal`、`aegra_personal_worker`
- 人工：隔离数据上 Full 文件备份、取消、deadline、VSS 失败、不可读文件、目标磁盘满；成功 Archive
  可由 `PersonalFileArchiveReader` 打开；Worker file_set Verify（F7）遍历认证全部 stream payload；
  file_set Restore（F8）经 `file_restore_target` 选择性写入目标树

## Geometry

file_set 不沿用 volume 的 64 KiB block 作为 stream write 量子。Worker 将 file_set 的
`block_size` 对齐到 `chunk_size`（上限 64 MiB），使每个 stream extent 对应一块大 payload，
避免 File Index leaf（plain ≤ 1 MiB）被 extent CBOR 撑爆。Session 按 entry 数与 plain 大小
分包 leaf，必要时写一层 internal root。

## 当前状态

F5 Full file_set 备份、F7 完整 Verify、F8 选择性恢复、F9 Desktop UX 均已接线；F10 发布门禁已通过。
**设计变更（ADR-0020）**：file_set Incremental 的变化判断从 USN contract/source 改为 metadata signature：
同路径普通文件的 `write_time + logical_size` 与直接父层相同则引用 parent stream，否则本层写完整 local stream。
代码切换已完成；Worker 数据面输入为 `effective_type` / `parent_uuid` / parent reader / selection fingerprint，
Manifest 固定写 `change_detection_method=mtime_size_v1`，不再查询或保存 journal checkpoint。

File Index Writer 自底向上构建多层 B+tree（leaf → internal…→ root，depth≤8），与产品上限 L04
（10_000_000 entries）/ L14 一致；不再在 leaf>257 时误报 `index_depth_limit`。Reader 递归收集 leaf
已支持多层；打开时仍会 materialize 全量 entry（大规模内存路径属后续优化）。

已修复：`file index page header fields are invalid`（leaf plain 超 1 MiB）——根因是小 block
量子产生过多 extent；现用大 stream quantum + 按大小分包 multi-leaf。

## 枚举失败策略（unreadable_policy=fail_job）

递归枚举目录 children 时，`FindFirstFileExW` 失败或 `FindNextFileW` 以非
`ERROR_NO_MORE_FILES` 终止，一律视为不可读源：`file_source.unreadable` → Job Failed →
Archive Abort。空目录仍通过 `.` / `..` 正常打开，不会走失败路径。禁止把枚举错误静默为
“无 children”，以免提交不完整 Recovery Point。

## 卷级系统目录排除

选择整卷（或递归路径下出现下列名称）时，`WindowsFileSnapshotView::queue_children` **不**把下列
目录入队，因此不会进入 File Index / Archive：

| 名称 | 原因 |
| --- | --- |
| `$RECYCLE.BIN` | 回收站；非用户主动选择的保护数据 |
| `System Volume Information` | VSS/系统还原元数据；备份无意义且常不可读 |

名称比较为 ASCII 大小写不敏感。这与 Browse 隐藏策略一致；已有 Archive 不受影响，需重新备份。

## 卷根 Archive 显示名

整卷 selection（`relative_components` 为空）的 root entry 名称使用
`FileSourceRef.display_label`（与 Browse 一致，如 `新加卷 (F:)`），并去掉 Windows 路径非法字符
（`:` 等）以便恢复时可作为目标下目录名；**不再**把 opaque `selection_id` UUID 写入 entry name。
子目录/文件 selection 仍使用真实路径 leaf 名。

## 不支持对象（ADR-0018）

本期只支持目录、普通文件和未命名主数据流。reparse、hard link、sparse、ADS 在 Backup 枚举时 strict fail，
在 Restore 第一次目标 mutation 前拒绝。Sink 不得宣称或预留这些能力，也不得跟随、扁平化、展开为 dense、
复制为独立文件或忽略 ADS。现有预留字段、分支和虚假 `supports_*` 由 FI0 直接删除；旧开发 Archive 不兼容。

文件 Incremental 的目标边界、metadata signature 资格和 chain reader 见
[增量设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md)；实施记录见
[FI0–FI10](../development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md)。

## 文件系统能力与 ACL（ADR-0016 / ADR-0021 / V7 §5.7）

- 备份：NTFS/ReFS 枚举启用 `SeBackupPrivilege` / `SeSecurityPrivilege`，并读取 self-relative
  Owner/Group/DACL/SACL，写入 `platform_metadata` envelope（tag 1）并置
  `ENTRY_FLAG_HAS_SECURITY`。启用特权后仍无法读完整 SD → strict failure
  `file_source.security_descriptor_unreadable`（经 TaskResult `message_code` 透出）。
- FAT32 备份仍必须来自 VSS snapshot；条目使用 null stable identity，不探测 FileId/hard-link/ADS/security，
  `platform_metadata` 为空。Incremental 仍按同路径 `write_time + logical_size`，受 FAT32 粗 mtime 精度限制。
- 恢复：NTFS/ReFS `WindowsFileTreeSink` 启用 restore/security 特权；`apply_metadata` /
  `apply_directory_metadata` 从 envelope 解出 SD 并以 `SetFileSecurityW` 写回 staging/目标路径。
  Pipeline 始终恢复时间戳与属性；仅当 `restore_security=true` 时应用 SD。
- FAT32 目标声明 `supports_security_descriptor=false` 和 `maximum_file_size_bytes=0xFFFFFFFF`；ACL 开启或
  单文件超限时在写前拒绝。关闭 ACL 后仍恢复时间戳、属性和主数据流。
- 日志/Catalog 仍禁止输出 SD 字节或 SID 明文。
