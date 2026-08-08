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
- file_set Incremental / Differential / raw 无 VSS 回退

## 执行顺序

```text
validate JobRequest (schema 4, file_set, full)
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

F5 Full file_set 备份、F7 完整 Verify、F8 选择性恢复、F9 Desktop UX 均已接线。
F10 发布门禁已通过（Debug/Release 生产构建、静态/架构/兼容搜索、文档收口）。F11 文件级
Incremental 仍暂缓，需单独 Accepted ADR。

已修复：`file index page header fields are invalid`（leaf plain 超 1 MiB）——根因是小 block
量子产生过多 extent；现用大 stream quantum + 按大小分包 multi-leaf。

## 枚举失败策略（unreadable_policy=fail_job）

递归枚举目录 children 时，`FindFirstFileExW` 失败或 `FindNextFileW` 以非
`ERROR_NO_MORE_FILES` 终止，一律视为不可读源：`file_source.unreadable` → Job Failed →
Archive Abort。空目录仍通过 `.` / `..` 正常打开，不会走失败路径。禁止把枚举错误静默为
“无 children”，以免提交不完整 Recovery Point。

## Sink 能力声明（reparse / hard link / sparse / ADS）

`WindowsFileTreeSink::capabilities` 对外宣称支持 reparse、hard link、sparse、ADS
（`supports_*=true`）。**本期不实现**这些能力的完整恢复路径；文档与 preflight 形状预留，
完整语义（reparse buffer、hard-link 组、sparse ranges、ADS 产品闭环）延后单独工作包。
本期交付范围以目录/文件主数据流、时间戳与属性、冲突策略、security descriptor（可选）为准。

## ACL / Security Descriptor（ADR-0016 / V7 §5.7）

- 备份：`WindowsFileSnapshotView::open` 启用 `SeBackupPrivilege` / `SeRestorePrivilege` /
  `SeSecurityPrivilege`；枚举每个 entry 时 `GetFileSecurityW` 读取 self-relative
  Owner/Group/DACL/SACL，写入 `platform_metadata` envelope（tag 1）并置
  `ENTRY_FLAG_HAS_SECURITY`。启用特权后仍无法读完整 SD → strict failure
  `file_source.security_descriptor_unreadable`（经 TaskResult `message_code` 透出）。
- 恢复：`WindowsFileTreeSink` 同样启用上述特权；`apply_metadata` /
  `apply_directory_metadata` 从 envelope 解出 SD 并以 `SetFileSecurityW` 写回 staging/目标路径。
  Pipeline 始终恢复时间戳与属性；仅当 `restore_security=true` 时应用 SD。
- 目标能力：`supports_security_descriptor=true`；preflight 在 `restore_security` 且 entry 含
  platform metadata 时校验。日志/Catalog 仍禁止输出 SD 字节或 SID 明文。
