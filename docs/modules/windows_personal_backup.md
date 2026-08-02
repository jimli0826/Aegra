# Windows 个人卷备份 Composition Root

## 目标与非目标

`apps/worker` 把 Windows Volume Inventory、VSS Snapshot Session、Snapshot Block Source、个人版
Archive Session 和通用 Backup Pipeline 组装成一个真实卷全量备份入口。

本阶段只支持单 Volume 全量备份，不实现磁盘分区表采集、多 Volume 原子 Archive Set、增量父链发现、
任务持久化、提权 Host 或命令行协议。

## 依赖边界

Target `aegra_app_worker_personal` / `Aegra::AppWorkerPersonal` 是 Composition Root，可以依赖：

- `Aegra::Pipeline`、`Aegra::Ports`、`Aegra::Format`；
- `Aegra::AdapterWindowsDisk`；
- `Aegra::AdapterWindowsVss`；
- `Aegra::AdapterPersonalArchive`。

这些具体依赖不得反向进入 `application`、`pipeline`、`ports` 或 `format`。私有 Runtime 接口仅用于
确定性装配测试，不是跨模块 Port，也不向产品调用方公开。

## 执行流程

```text
Validate Request
-> Enumerate and select canonical Volume GUID
-> Require IOCTL-backed logical size
-> Create one-volume VSS Snapshot Set
-> Open Snapshot Device as WindowsBlockSource
-> Build one-volume V6 Manifest
-> Create PersonalArchiveSession
-> Run BackupPipeline and commit Archive
-> Destroy Block Source handle
-> BackupComplete and delete VSS Snapshot Set
```

Volume 未找到、逻辑大小不可靠、VSS 创建失败或 Snapshot Device 无法打开时，不创建 Archive。Pipeline
失败时 Archive Session 负责 Abort，Composition Root 仍先关闭 Block Source，再清理 Snapshot。

## 请求与结果

`WindowsPersonalVolumeBackupRequest` 要求调用方提供：

- job id、trace id、canonical Volume GUID Path 和 `.bkf` destination；
- 调用期间有效的 password view；
- 密码学随机且非零、互不相同的 file UUID 与 backup-set UUID；
- block/chunk/memory geometry、KDF 参数和可选分卷大小；
- created UTC、应用版本和 hostname。

成功表示 Archive 已经 Commit。此后 Snapshot 删除失败不能把已发布 Archive 伪装成未提交，因此
`WindowsPersonalVolumeBackupResult::snapshot_cleanup_error` 单独报告清理告警。调用方必须记录并告警，
但不能重复执行同一个非幂等备份请求来“修复”清理错误。

## Worker 任务入口

`execute_windows_personal_backup_task()` 是进程协议 Adapter 后面的同步任务入口：

```text
Validate JobRequest and trusted options
-> Publish correlated Preparing progress
-> Reject pre-cancellation or expired deadline
-> Generate distinct RFC 4122 v4 file/backup-set UUIDs
-> Resolve exactly one SecretRef
-> Convert UTF-8 source and target refs to Windows paths
-> Execute Windows personal volume backup
-> Return validated and sanitized TaskResult
```

- 通用 Job 必须是 Backup operation，并且恰好包含一个 source 和一个 credential ref；
- schema 或 operation-specific 校验失败表示请求未被接受，返回 `Result` failure 且不获取凭据；
- 请求一旦被接受，取消、凭据、随机源、VSS、I/O 和 Archive 失败均转换为 `TaskResult`；
- TaskResult 不复制底层 Error message，只使用稳定 message/warning code；
- Snapshot 清理失败映射为 `kSucceededWithWarning`，容量与 chunk 指标仍来自已提交 Archive；
- deadline 和 UUID 生成在获取凭据前完成；任务运行中的 deadline 由 Worker Host 转换为 CancellationToken。

## Manifest 语义

- 只包含一个 `volume_index=0` 的 Volume；
- `volume_id` 和 `volume_guid` 使用 Inventory 返回的 Volume GUID Path；
- `vss_required=true`、`vss_used=true`，Writer Status 全部成功后标记 application consistency；
- total size 使用 `IOCTL_DISK_GET_LENGTH_INFO` 的结果；
- mount points、filesystem、label 和 cluster size 来自 Inventory；
- 本阶段不伪造 Disk、Partition 或 Volume Extent。Disk Inspector 完成后再补齐裸机恢复布局。

## 所有权、线程和取消

- Use Case 同步执行；Progress Sink、password view 和请求必须在调用期间有效。
- Snapshot Lease 拥有 VSS Session，Block Source 拥有 Snapshot Device Handle，Archive Session 拥有
  partial 文件。
- Block Source 必须在 Snapshot Lease 关闭前析构。
- 取消贯穿 VSS 创建、块读取、Archive 写入和 Pipeline；Snapshot 删除本身不可取消。
- 公开入口捕获未处理异常并转换为 `kInternal`，不得让异常越过 Worker 边界。

## 测试与完成标准

- 私有 Runtime seam 覆盖成功装配、请求前置校验、源读取失败、Archive 创建失败和 Snapshot 清理失败；
- 私有 Task Backend seam 覆盖 DTO 映射、Secret 生命周期、UUID、deadline、取消和错误脱敏；
- 测试断言 Block Source 在 Snapshot 删除前析构；
- 测试断言 Pipeline 失败会 Abort Archive，已 Commit Archive 的清理失败作为独立告警返回；
- 普通测试不访问真实 Volume、VSS 或文件系统 Archive；
- 管理员环境集成测试后续覆盖真实 NTFS/ReFS Volume、VSS Service、空间不足和 Writer failure；
- Debug/Release、clang-tidy、源码规模与依赖检查必须通过。
