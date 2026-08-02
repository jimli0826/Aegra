# Windows Disk、Volume 与 VSS Adapter 开发文档

## 目标与非目标

本模块把 Windows Volume Inventory、稳定块读取、VSS Snapshot 和后续 Disk/Partition 操作适配到
Aegra 的平台无关核心。阶段 8A 实现 Volume Inventory 与稳定 Block Source；阶段 8B 实现多 Volume
VSS Snapshot Set Session；阶段 8F 实现 Worker 使用的系统时钟、密码学随机与凭据解析；阶段 8G
实现 Worker 本地 Named Pipe Client。

个人版卷恢复新增 `WindowsVolumeBlockSink`：生产模式只接受 canonical Volume GUID，拒绝系统卷，成功
锁卷并卸载后才允许按 offset 写入，完成时 flush，析构时 best-effort 解锁。普通文件模式仅用于确定性
契约测试。不可逆写入决策见 [ADR-0009](../adr/0009-windows-volume-restore-safety.md)。

本模块不直接读取在线 Volume、不备份 `PhysicalDrive`、不修改分区表，也不执行 BCD/WinRE 修复。
只有显式恢复用的 Block Sink 可以在完成安全检查后写入非系统目标 Volume。

## 依赖

- `aegra_adapter_windows_disk` 只依赖 `Aegra::Ports` 和 Windows SDK。
- `aegra_adapter_windows_vss` 只依赖 `Aegra::Base`、VSS API、COM 和 Windows SDK；不得依赖
  `windows_disk` 的实现。
- `aegra_adapter_windows_system` 只依赖 `Aegra::Ports`、BCrypt、Credential Manager 和虚拟内存 API；
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
- `IOCTL_DISK_GET_LENGTH_INFO` 返回的逻辑大小与 cluster size；
- disk number、physical offset 和 length 组成的 extents；
- filesystem metadata、逻辑大小和 extent mapping 是否可用的 capability。

文件系统未就绪、可移动介质无介质或 extent 查询权限不足不会删除 Volume identity；对应 capability 为
false。只有 Volume 枚举本身无法启动或异常终止时，整个调用失败。

### `WindowsBlockSource`

实现 `IBlockSource`，支持：

- `kStableFile`：普通文件或 UNC 文件，不允许 Win32 Device Namespace；
- `kVssSnapshot`：严格校验的 Shadow Copy Device Object，并要求显式逻辑大小。

Source 独占 Handle，可以并发调用 `read()`。每次读取使用独立重叠 I/O 状态；取消会中止本次 I/O，
不会关闭 Source Handle。`size_bytes()` 在对象生命周期内稳定。

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
- `WindowsCredentialResolver` 只解析 `wincred://<target>` Generic Credential；Blob 复制到锁页内存，
  析构前清零。具体安全与部署决策见
  [ADR-0007](../adr/0007-windows-worker-system-capabilities.md)。

### `WindowsNamedPipeChannel`

实现 `IMessageChannel`，作为 Client 连接父进程预创建的本地 Pipe。公开输入是最长 128 字节且只含
`[A-Za-z0-9_.-]` 的逻辑名称，Adapter 内部映射到 `\\.\pipe\aegra-worker-<name>`。传输使用 byte mode、
4 字节 little-endian 长度前缀和 UTF-8 body；零长度或超过配置上限的帧直接拒绝。

同一实例允许一个接收和一个发送并发，不保证多 Reader 或多 Writer 安全。连接轮询响应取消，挂起
Overlapped I/O 通过 `CancelIoEx` 中止。父进程负责 Server 生命周期、ACL 与 Worker SID 授权，具体协议见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

## 核心不变量

- 在线 Volume 和 `PhysicalDrive` 不能通过公开 Block Source 请求打开。
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

## 测试

- 普通单元测试只使用临时文件，不依赖管理员权限、真实 Volume 或 VSS Service。
- 覆盖正确读取、非零 offset、EOF、越界、取消、缺失文件和 Device Path 拒绝。
- Shadow Copy 路径验证使用纯字符串样本。
- VSS 单元测试通过 Adapter 私有 Backend seam 覆盖空/重复/非法请求、预取消、映射校验、幂等关闭、
  关闭失败和析构清理，不创建真实系统快照。
- 真实 Volume、跨盘 Volume、无介质设备、访问拒绝和 VSS 生命周期进入单独集成测试。
- System Adapter 普通测试不创建或修改真实 Credential；成功解析、锁页清零和账户隔离进入临时
  Credential 集成套件。
- IPC 测试创建进程内临时本地 Pipe，覆盖双向 frame、并发 Reader/Writer、无效长度、预取消、挂起接收
  取消和 Server 断线；真实 Worker 子进程测试验证 `--pipe` 拒绝路径。

## 安全与可观测性

- 错误不得输出完整源路径。
- 不请求写权限，不使用 `FILE_FLAG_NO_BUFFERING`，不要求调用方提供对齐缓冲区。
- Application 后续接入可记录 Snapshot Set ID、Snapshot ID、阶段耗时和清理结果，但不记录客户数据。
- `apps/worker` 的个人卷备份装配见
  [Windows 个人卷备份 Composition Root](windows_personal_backup.md)。

## Definition of Done

- Windows SDK 类型未泄漏到公共头或核心模块。
- Block Source 通过 `IBlockSource` 关键契约测试。
- 普通文件模式无法打开 Device Namespace，Snapshot 模式无法伪装任意设备。
- 多 Volume 只通过一个 Snapshot Set 创建，COM/VSS 对象始终在专用 MTA 线程释放。
- 显式关闭成功后不重复删除；失败和析构路径仍承担清理责任。
- Debug/Release 构建、测试、clang-tidy 与源码规模检查通过。
