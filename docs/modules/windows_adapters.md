# Windows Disk、Volume 与 VSS Adapter 开发文档

## 目标与非目标

本模块把 Windows Volume Inventory、稳定块读取、VSS Snapshot 和后续 Disk/Partition 操作适配到
Aegra 的平台无关核心。阶段 8A 实现 Volume Inventory 与稳定 Block Source；阶段 8B 实现多 Volume
VSS Snapshot Set Session；阶段 8F 实现 Worker 使用的系统时钟、密码学随机与凭据解析；阶段 8G
实现 Worker 本地 Named Pipe Client。

个人版卷恢复使用 `WindowsBlockSink` 的 `kVolume` 模式：只接受 canonical Volume GUID，拒绝系统卷，成功
锁卷并卸载后才允许按 offset 写入，完成时 flush，析构时 best-effort 解锁。普通文件模式仅用于隔离验证。
不可逆写入决策见 [ADR-0009](../adr/0009-windows-volume-restore-safety.md)。

个人版整盘恢复（Full 第一步）使用 `kPhysicalDisk` 模式：只接受 `\\.\PhysicalDriveN`，拒绝系统盘与
Archive 所在盘；写入前由 `prepare_target_disk_for_raw_restore` 删除现有分区布局；写入后由
`apply_disk_signature_policy`（可选随机化 MBR/GPT DiskId）、`rebuild_partition_table_from_raw_layout`
重建 MBR/GPT、`bring_target_disk_online`，以及可选的 `expand_last_data_partition_on_disk`（对齐旧 `PartitionManager::ExtendLastDataPartitionOnDisk` /
`ExtendVolumeByPath`：先解析末数据分区与卷 GUID，**保持 volume 句柄打开**，
`IOCTL_DISK_GROW_PARTITION` 后再 `FSCTL_EXTEND_VOLUME(newTotalSectors)`；GPT 末尾预留 1 MiB；
FAT/exFAT 跳过扩容）。备份侧
`inspect_physical_disk_layout` 以 `GENERIC_READ` 打开 `PhysicalDrive` 并尽量采集 `raw_layout`；
原始扇区读取失败时不阻断卷备份（空 `raw_layout`）。

本模块允许备份 Worker 以只读方式直接读取不支持 VSS 的在线 Volume；备份数据面不直接读取
`PhysicalDrive` 用户数据。分区表仅作为 Manifest 元数据与 `raw_layout` 采集；只有显式恢复路径可在
安全检查后写入非系统目标 Volume 或 PhysicalDrive。

## 依赖

- `aegra_adapter_windows_disk` 只依赖 `Aegra::Ports` 和 Windows SDK。
- `aegra_adapter_windows_vss` 只依赖 `Aegra::Base`、VSS API、COM 和 Windows SDK；不得依赖
  `windows_disk` 的实现。
- `aegra_adapter_windows_system` 只依赖 `Aegra::Ports`、BCrypt、Crypt32（DPAPI）和虚拟内存 API；
  不依赖 Disk、VSS 或 Archive Adapter。
- `aegra_adapter_windows_ipc` 只依赖 `Aegra::Ports` 与 Windows Named Pipe API；不解析 JSON，也不依赖
  Worker Host 实现。
- 公共头不得 include `Windows.h`、`winioctl.h`、COM 或 VSS 头文件。
- Windows Handle、`OVERLAPPED`、Event 和 Volume Enumeration Handle 必须由 Adapter 内 RAII 对象管理。
- Pipeline、Format、Ports 和 Contracts 不得依赖该 Target。

## 公共接口

### `WindowsVolumeEnumerator`

`enumerate()` 返回以 Volume GUID Path 标识的 Volume 列表。每条记录包含：

- mount points；
- UTF-8 label 与 filesystem；
- `IOCTL_DISK_GET_LENGTH_INFO` 返回的逻辑大小（不可用时使用受溢出检查的 extent 总长度）与 cluster size；
- disk number、physical offset 和 length 组成的 extents；
- filesystem metadata、逻辑大小和 extent mapping 是否可用的 capability。

文件系统未就绪、可移动介质无介质或 extent 查询权限不足不会删除 Volume identity；对应 capability 为
false。只有 Volume 枚举本身无法启动或异常终止时，整个调用失败。

### `WindowsSourceInventory`（磁盘优先）

`list_sources()` 对齐旧项目 `GetDisksWithVolumes`：

1. 先枚举 `PhysicalDrive0..31`（容量、`MBR`/`GPT`/`RAW` 分区样式）；
2. 再枚举 Volume，且 **仅在 extent 能解析出 disk number 时** 发布卷记录（不再把未知 extent 默认到 Disk 0）；
3. **每个**物理盘发布不可选的 `disk.N` 壳记录（`capacity_bytes=0`、`is_read_only`，`disk_capacity_bytes`
   为整盘容量）：空盘供 Desktop `disksTree` 显示 Unallocated；有卷的盘供 Restore 以 `disk.N` 作为
   `PrepareRestore.target_source_id`。`is_system` 由同盘上是否存在系统卷推导。

备份源可选性与恢复目标安全规则分离：Windows 系统卷（通常为 C:）、只读卷、EFI/FAT、RAW 和未知
文件系统卷均允许作为备份源；在线恢复仍按 ADR-0009 拒绝系统目标。具备 stable Volume GUID 和可靠
非零容量的 Volume 标记为可选。NTFS/ReFS/FAT/FAT32/exFAT 使用 VSS（整盘系统盘备份时 EFI 与 OS
卷进入同一 Snapshot Set）；RAW 与未知文件系统使用 raw block source。逻辑大小优先
`IOCTL_DISK_GET_LENGTH_INFO`，其次 extent/分区长度，最后才回退到 `GetDiskFreeSpaceEx` 容量（仅展示）。

### `WindowsBlockSource`

实现 `IBlockSource`，支持：

- `kStableFile`：普通文件或 UNC 文件，不允许 Win32 Device Namespace；
- `kVssSnapshot`：严格校验的 Shadow Copy Device Object，并要求显式逻辑大小；
- `kRawVolume`：只接受 canonical Volume GUID Path，以只读共享 Handle 打开，并要求显式非零逻辑大小。

Source 独占 Handle，可以并发调用 `read()`。每次读取使用独立重叠 I/O 状态；取消会中止本次 I/O，
不会关闭 Source Handle。`size_bytes()` 在对象生命周期内稳定。`kVssSnapshot` / `kRawVolume` 对齐
AipCopy 的源读取策略：句柄使用 `FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN`，单次 `read()`
内保持一个在途 IRP（QD1）；保留 `FILE_FLAG_OVERLAPPED` 仅用于显式 offset 和可取消等待，不增加同一
调用的队列深度。

对 `kVssSnapshot` / `kRawVolume`（对齐旧 `DiskDevice` + `BackupEngine` trailing pad）：

- `size_bytes()` 始终等于 Inventory/Manifest 的逻辑卷长度（`expected_size_bytes`）；
- 打开时用 `IOCTL_DISK_GET_LENGTH_INFO`，失败再 `GetFileSizeEx` 探测设备可读长度；
  若探测值为 0 或大于逻辑长度，则按逻辑长度视为全部可读；若 `0 < readable < logical`，
  仅 **[readable, logical)** 允许零填充（trailing），并保留可读边界；
- raw / VSS 设备读：无缓存 `ReadFile` 允许 partial IRP，**循环读满**请求长度；单次 IRP 上限 4 MiB，
  同一 `read()` 同时只有一个 IRP。Adapter 使用 `VirtualAlloc` 对齐 bounce buffer，并在返回前复制到调用方
  buffer，因此 Port 不要求调用方提供扇区对齐内存。循环后仍读不满可读区间才失败（真 EOF / 设备截断），
  **不得**把 partial 当成致命 short read；
- raw 与 VSS 均尝试 `FSCTL_ALLOW_EXTENDED_DASD_IO`（VSS 拒绝时继续，raw 失败则打开失败）；
- Free-skip / 已验证排除区间的 FREE 分类由 `FreeSkipBlockSource` 负责，不与设备 short read 混用。

`kStableFile` 保持严格语义：short/EOF 原样返回，不零填充。

### Volume Bitmap 空闲簇跳过（`FreeSkipPlan` / `FreeSkipBlockSource`）

对齐旧 `BackupEngine::BuildFreeBlockMap`：

- `build_free_skip_plan(device_path, filesystem, total_size, cluster_size)` 对 VSS 快照设备或 raw
  Volume GUID 调用 `FSCTL_GET_VOLUME_BITMAP`；
- **NTFS/ReFS**：线性 LCN；前 64 MiB 系统区永不视为空闲；
- **FAT/FAT32**：解析 BPB 得到 data area 起点，按 AipCopy 方式把 bitmap 映射到数据区；保留区与 FAT
  始终读取；
- **exFAT/RAW/未知**：不启用，整卷读取；
- bitmap 失败时 `applied=false`，调用方退回全量读取（宁可备份变大，不可漏数据）。

`FreeSkipBlockSource` 包装底层 `IBlockSource`：`describe_extent()` 将空闲区间报告为 FREE，已用区间报告为
DATA。Backup Pipeline 对 FREE 不调用 `read()`，Personal Archive 把它编码成 FREE BlockEntry（无 payload），
全量 Archive 仍覆盖完整逻辑地址空间。真实全零 DATA 才编码为 ZERO；恢复 FREE 时直接跳过目标写盘。
Composition Root 在包装前把 cluster extent 向内收缩到 Archive block 边界；同时含 DATA 与 FREE cluster 的
Archive block 按 DATA 读取和保存，防止把已用字节误标 FREE。卷末短 block 可在确认全部空闲时标 FREE。

`merge_page_and_hibernation_exclusions`（Desktop Options「Exclude pagefile / hiberfil / swapfile」）：
对齐 AipCopy `ExcludeJunkFiles` / `AddFileToExcludedClusters`。在与块读取**相同**的设备根上解析
pagefile.sys / hiberfil.sys / swapfile.sys 的 LCN，并入 free-skip plan（标记 FREE 且不读盘）：

- **raw**：canonical Volume GUID 根；
- **VSS**：`snapshot_device_path`（`\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN`）根，与
  AipCopy `swStaticVolume` 同源；直接 `CreateFile` 失败时用临时 `DefineDosDevice(DDD_RAW_TARGET_PATH)`
  映射后再取 extent（AipCopy MapDriveLetter 模式）；
- 优先 `FSCTL_GET_RETRIEVAL_POINTERS`，失败再 MFT 回退；
- **禁止**把 live Volume 上的 LCN 套到快照设备上（Composition Root 只传入当前读路径）。

VSS：`supports_vss_snapshot` 仅按文件系统筛候选；真正加入 Snapshot Set 前调用
`is_volume_snapshot_supported`（`IsVolumeSupported`）。不支持的卷走 raw，`vss_used=false`。

Worker 默认几何：`block=64KiB`、`chunk=256MiB`、`memory_budget=512MiB`（budget ≥ chunk，
约两个物理 Chunk 流水线重叠；格式单 Chunk payload 上限 512 MiB）。
`PersonalArchiveSession` 在创建时启动 **session 级** block worker pool（线程数 =
`hardware_concurrency`，至少 1），供每个物理 Chunk 的 hash/zstd 并行复用；**不得**在每个
Chunk 上重新 `std::thread` 创建/join。每个 pool worker 持有并复用一个 `ZstdCompressor`
（`ZSTD_CCtx` + 输出 scratch）。线程内异常捕获后返回 `Result`；Session 析构时 join pool。

### `WindowsVssSnapshotSession`

`create()` 接受一个或多个 canonical Volume GUID Path 及其 Inventory 阶段获得的非零逻辑大小。全部
Volume 通过一次 `StartSnapshotSet`/`DoSnapshotSet` 建立同一一致性边界。成功后，每个请求按原顺序
对应一个包含 Snapshot Device Object 的只读映射。

Session 由单一调用方拥有，但可以跨线程转移；公开方法不能并发调用。所有 COM/VSS 接口只在 Adapter
内部专用 MTA 工作线程创建、使用和释放。`close()` 执行 `BackupComplete` 并强制删除整个 Snapshot
Set，成功后幂等；未显式关闭或关闭失败时，析构路径执行 `AbortBackup` 和再次删除。

创建期间的 `GatherWriterMetadata`、`PrepareForBackup` 和 `DoSnapshotSet` 轮询 VSS Async 状态并响应取消。
在 Prepare 和 Snapshot 完成后必须收集 Writer Status，任何 Writer failure 都使创建失败并触发清理。
取消只终止创建，不能跳过已经开始的 Snapshot Set 清理。`close()` 故意不接受取消令牌，因为资源删除
必须完成或明确返回失败。

### Windows Worker 系统能力

- `WindowsSystemClock` 返回 Unix UTC 毫秒；转换前的 Windows epoch 不得伪装成有效时间。
- `WindowsCryptographicRandom` 使用系统首选 CNG RNG，预取消时不调用系统 RNG。
- `WindowsCredentialResolver` 只解析 `dpapi-lm:<entropy_id>:<base64>`（DPAPI
  `CRYPTPROTECT_LOCAL_MACHINE`，`pOptionalEntropy` = UTF-8 `entropy_id`，Schedule 为 `schedule_id`）；
  解密后的明文复制到锁页内存，析构前清零。`protect_local_machine_secret(secret, entropy_id)` 供
  Service 加密口令。不使用 Windows Credential Manager。具体决策见
  [ADR-0007](../adr/0007-windows-worker-system-capabilities.md)。

### `WindowsNamedPipeChannel` / `WindowsNamedPipeListener`

实现 `IMessageChannel` 与 Service 侧 Listener。逻辑名称最长 128 字节且只含 `[A-Za-z0-9_.-]`，映射到
`\\.\pipe\aegra-worker-<name>` 或 `\\.\pipe\aegra-service-<name>`。传输使用 byte mode、4 字节
little-endian 长度前缀和 UTF-8 body；零长度或超过配置上限的帧直接拒绝。

同一实例允许一个接收和一个发送并发，不保证多 Reader 或多 Writer 安全。连接轮询响应取消，挂起
Overlapped I/O 通过 `CancelIoEx` 中止。Worker 父进程负责 Server 生命周期与 Worker SID 授权，见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。Service 控制 Pipe 的显式 ACL、远程拒绝
与调用方 SID/session 校验见 [windows_ipc.md](windows_ipc.md) 与
[ADR-0014](../adr/0014-windows-service-ipc-security.md)。

## 核心不变量

- 只有 `kRawVolume` 可以打开在线 Volume；任意 Device Namespace 和 `PhysicalDrive` 始终拒绝。
- VSS Snapshot path 不接受目录穿越、尾随 path component、符号编号或其它 Device Object。
- offset 大于逻辑大小返回 `kInvalidArgument`；offset 等于 EOF 或空 buffer 返回 0。
- 单次 Win32 读取不超过 `DWORD`，大 buffer 通过 Port 的短读语义由 Pipeline 继续读取。
- 所有 Win32 错误在边界转换为稳定 `ErrorCode`，消息不包含客户路径或数据。
- Volume Enumerator 不把“无权限读取 extents”误报成“不存在该 Volume”。
- 同一 Session 不接受空列表、零逻辑大小或重复 Volume；Snapshot 映射数量、identity 与大小必须和请求一致。
- VSS 生命周期、COM apartment 和 Snapshot Set 删除都封装在同一个 RAII Backend 内。

## 目录与 Target

```text
src/adapters/windows_disk/
├── CMakeLists.txt
├── include/aegra/adapters/windows_disk/windows_disk.h
└── src/
    ├── windows_block_source.cpp
    ├── windows_api.h
    └── windows_volume_enumerator.cpp
```

Target：`aegra_adapter_windows_disk` / `Aegra::AdapterWindowsDisk`，仅在 Windows 构建。

```text
src/adapters/windows_vss/
├── CMakeLists.txt
├── include/aegra/adapters/windows_vss/windows_vss.h
└── src/
    ├── com_ptr.h
    ├── vss_snapshot_core.h/.cpp
    ├── windows_vss_backend.cpp
    └── windows_vss_session.cpp
```

Target：`aegra_adapter_windows_vss` / `Aegra::AdapterWindowsVss`，仅在 Windows 构建。

`aegra_adapter_windows_system` / `Aegra::AdapterWindowsSystem` 同样仅在 Windows 构建，公开头不暴露
`Windows.h`、Credential 或 BCrypt 类型。

`aegra_adapter_windows_ipc` / `Aegra::AdapterWindowsIpc` 仅在 Windows 构建，公开头使用 PImpl 隔离
`HANDLE` 与 `OVERLAPPED`。

## 验证

- 构建 Windows Disk、VSS、System 和 IPC 生产 Target。
- 审查读取、offset、EOF、越界、取消、路径拒绝、VSS 清理、Credential 生命周期和 IPC framing 边界。
- 真实 Volume、跨盘 Volume、无介质设备、访问拒绝、VSS 与临时 Credential 仅在隔离环境人工验证。
- IPC 人工验证使用临时本地 Pipe；日志可记录诊断所需的 frame 字段、路径和其他用户数据，但不得记录
  密码、密钥、SecretRef、Credential、Authorization、Cookie、令牌或其他认证材料。

## 安全与可观测性

- 错误不得输出完整源路径。
- 不请求写权限；raw / VSS 读取使用 `FILE_FLAG_NO_BUFFERING`，对齐要求完全封装在 Adapter 内，不要求
  调用方提供对齐缓冲区。普通稳定文件仍使用 Windows buffered I/O。
- Application 后续接入可记录 Snapshot Set ID、Snapshot ID、阶段耗时、清理结果及诊断所需用户数据，
  但不得记录认证信息；记录范围遵循最小必要原则。
- `apps/worker` 的个人卷备份装配见
  [Windows 个人卷备份 Composition Root](windows_personal_backup.md)。

## Definition of Done

- Windows SDK 类型未泄漏到公共头或核心模块。
- Block Source 遵循 `IBlockSource` 关键契约。
- 普通文件模式无法打开 Device Namespace，Snapshot/raw 模式无法伪装任意设备。
- 支持 VSS 的多个 Volume 只通过一个 Snapshot Set 创建；raw Volume 不加入该 Set；COM/VSS 对象始终在
  专用 MTA 线程释放。
- 显式关闭成功后不重复删除；失败和析构路径仍承担清理责任。
- Debug/Release 构建、clang-tidy 与源码规模检查通过。
