# Windows 个人卷备份 Composition Root

## 目标与非目标

`apps/worker` 把 Windows Volume Inventory、VSS Snapshot/raw Volume Block Source、个人版 Archive
Session 和通用 Backup Pipeline 组装成一个真实卷全量/增量备份入口。

本阶段支持一个全量或增量 Job 将一个或多个 Volume 原子写入同一 Archive。增量要求显式父 Archive 路径与
完整 `.bhx`，并按 volume 顺序匹配父层身份与逻辑大小。不实现磁盘分区表独立采集、Service 外的自动父链
发现、差异备份、任务持久化、提权 Host 或命令行协议。

## 依赖边界

Target `aegra_app_worker_personal` / `Aegra::AppWorkerPersonal` 是 Composition Root，可以依赖：

- `Aegra::Pipeline`、`Aegra::Ports`、`Aegra::Format`；
- `Aegra::AdapterWindowsDisk`；
- `Aegra::AdapterWindowsVss`；
- `Aegra::AdapterPersonalArchive`。

这些具体依赖不得反向进入 `application`、`pipeline`、`ports` 或 `format`。私有 Runtime 接口用于隔离
具体装配细节，不是跨模块 Port，也不向产品调用方公开。

## 执行流程

```text
Validate Request
-> Enumerate and select ordered canonical Volume GUID paths
-> Require a reliable nonzero logical size for every Volume
-> Put all VSS-capable Volumes into one Snapshot Set
-> Open VSS Snapshot Devices and non-VSS canonical Volumes as ordered WindowsBlockSources
-> Classify volumes: FS candidate + IsVolumeSupported → VSS set; others raw
-> Optionally wrap each source with Volume Bitmap free-cluster skip (same device path as read)
-> pagefile/hiber/swap exclusion on the same read root (live GUID or VSS snapshot; AipCopy style)
-> Build one V6 Manifest containing all selected Volumes
-> Log worker thread count; hash/compress fan-out in PersonalArchive (exception-safe)
-> For incremental, authenticate parent Archive and Sidecar
-> Create PersonalArchiveSession
-> Run one BackupPipeline per Volume against the shared Session
   (progress aggregated over all volumes; kCompleted only after final commit)
-> Commit once after the final Volume
-> Destroy all Block Source handles
-> BackupComplete and delete VSS Snapshot Set
```

Volume 未找到、逻辑大小不可靠、VSS 创建失败、Snapshot Device 或 raw Volume 无法打开时，不创建
Archive。Pipeline 失败时 Archive Session 负责 Abort，Composition Root 仍先关闭全部 Block Source，
再清理存在的 Snapshot Set。

## 请求与结果

`WindowsPersonalBackupRequest` 要求调用方提供：

- job id、trace id、有序且无重复的 canonical Volume GUID Path 列表和 `.bkf` destination；
- 调用期间有效的 password view；
- 备份类型；增量还要求显式 parent Archive path 和调用期间有效的 parent password view；
- 密码学随机且非零的 file UUID；全量另有互不相同的 backup-set UUID，增量由父 Archive 继承；
- block/chunk/memory geometry、KDF 参数和可选分卷大小；
- created UTC、应用版本和 hostname。

成功表示 Archive 已经 Commit。此后 Snapshot 删除失败不能把已发布 Archive 伪装成未提交，因此
`WindowsPersonalBackupResult::snapshot_cleanup_error` 单独报告清理告警。调用方必须记录并告警，
但不能重复执行同一个非幂等备份请求来“修复”清理错误。

## Worker 任务入口

`execute_windows_personal_backup_task()` 是进程协议 Adapter 后面的同步任务入口：

```text
Validate JobRequest and trusted options
-> Open per-task file log under logs/backup/
-> Publish correlated Preparing progress
-> Reject pre-cancellation or expired deadline
-> Parse Service-assigned RFC 4122 IDs and creation time
-> Resolve current Archive SecretRef and optional parent SecretRef
-> Convert UTF-8 source and target refs to Windows paths
-> Log start milestones (volume, destination, geometry)
-> Execute Windows personal volume backup (VSS / pipeline milestones to active log)
-> Log complete/fail summary
-> Return validated and sanitized TaskResult
```

任务日志细节见 [Worker Host 与进程协议](worker_host.md#任务日志)。

- Job schema 3 的 Backup 必须包含 1 至 100 个有序且无重复的 volume source、显式 `backup.type`、`file_uuid`、
  `created_utc_ms`，全量还必须包含不同于 `file_uuid` 的 `backup_set_uuid`；
  加密 Archive 的 `credential_refs` 为 `dpapi-lm:<entropy_id>:<base64>`（Worker 用同一 entropy
  DPAPI 解密）；未加密时为空；
  增量还必须提供 `parent_source_ref`（`parent_credential_ref` 可选，缺省同 local 口令）；
- 差异备份当前在获取随机数、凭据或 Snapshot 前拒绝；
- schema 或 operation-specific 校验失败表示请求未被接受，返回 `Result` failure 且不获取凭据；
- 请求一旦被接受，取消、凭据、随机源、VSS、I/O 和 Archive 失败均转换为 `TaskResult`；
- TaskResult 不复制底层 Error message，只使用稳定 message/warning code；
- Snapshot 清理失败映射为 `kSucceededWithWarning`，容量与 chunk 指标仍来自已提交 Archive；
- deadline 和 Service 分配的 UUID/时间在获取凭据前校验；任务运行中的 deadline 由 Worker Host 转换为
  CancellationToken。

## Manifest 语义

- 每个 Source 对应一个 Volume，`volume_index` 按请求顺序从 0 连续递增；
- `volume_id` 和 `volume_guid` 使用 Inventory 返回的 Volume GUID Path；
- 系统卷、EFI/FAT、RAW、未知文件系统和只读卷均允许作为备份源；
- NTFS/ReFS/FAT/FAT32/exFAT Volume 设置 `vss_required=true`、`vss_used=true`，Writer Status 全部成功后
  标记 application consistency（整盘系统盘可把 EFI 与 OS 卷放进同一 Snapshot Set）；RAW/未知文件系统
  Volume 设置 `vss_required=false`、`vss_used=false` 并标记 crash consistency；
- `backup_job.backup_type` 来自受校验的 Job；增量 Session 认证父 Archive/Sidecar 后继承 backup-set UUID，
  并令 `parent_uuid` 指向父 `file_uuid`；
- total size 优先使用 `IOCTL_DISK_GET_LENGTH_INFO`，不可用时使用受溢出检查的 extent 总长度；
- mount points、filesystem、label 和 cluster size 来自 Inventory；
- Disk / Partition 来自 `inspect_physical_disk_layout`，并尽量采集 `raw_layout`（MBR/GPT）供整盘还原；
  `PhysicalDrive` 以 `GENERIC_READ` 打开以支持扇区 `ReadFile`；若原始扇区读取被拒绝则留下空
  `raw_layout` 并继续卷备份（整盘还原会在恢复时拒绝无 `raw_layout` 的 Archive）。不伪造 Extent。
  跨盘卷与完整裸机系统恢复仍属后续范围。

## 所有权、线程和取消

- Use Case 同步执行；Progress Sink、当前/父 password view 和请求必须在调用期间有效。
- 可选 Snapshot Lease 拥有 VSS Session；Block Source 分别拥有 Snapshot Device 或 raw Volume Handle；
  Archive Session 拥有 partial 文件。
- 所有 Block Source 必须在 Snapshot Lease 关闭前析构。
- 取消贯穿 VSS 创建、块读取、Archive 写入和 Pipeline；Snapshot 删除本身不可取消。
- 公开入口捕获未处理异常并转换为 `kInternal`，不得让异常越过 Worker 边界。

## 验证与完成标准

- 审查成功装配、请求前置校验、源读取失败、Archive 创建失败和 Snapshot 清理失败路径；
- 审查全量/增量 DTO 映射、Secret 生命周期、UUID、父凭据失败、deadline、取消和错误脱敏；
- 确认所有 Block Source 在 Snapshot 删除前析构，任一 Pipeline 失败会 Abort 整个 Archive，只有最后一个
  Volume 成功后 Commit，Commit 后清理失败单独告警；
- 真实 NTFS/ReFS VSS、EFI/FAT/raw Volume、空间不足和 Writer failure 仅在隔离管理员环境人工验证；
- Debug/Release、clang-tidy、源码规模与依赖检查必须通过。
