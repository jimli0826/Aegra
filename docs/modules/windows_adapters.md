# Windows Disk、Volume 与 VSS Adapter 开发文档

## 目标与非目标

本模块把 Windows Volume Inventory、稳定块读取、VSS Snapshot 和后续 Disk/Partition 操作适配到
Aegra 的平台无关核心。本阶段 8A 实现 Volume Inventory 与稳定 Block Source。

本阶段不创建 VSS Snapshot、不读取在线 Volume、不备份 `PhysicalDrive`、不修改磁盘或分区，也不
执行 BCD/WinRE 修复。

## 依赖

- `aegra_adapter_windows_disk` 只依赖 `Aegra::Ports` 和 Windows SDK。
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

## 核心不变量

- 在线 Volume 和 `PhysicalDrive` 不能通过公开 Block Source 请求打开。
- VSS Snapshot path 不接受目录穿越、尾随 path component、符号编号或其它 Device Object。
- offset 大于逻辑大小返回 `kInvalidArgument`；offset 等于 EOF 或空 buffer 返回 0。
- 单次 Win32 读取不超过 `DWORD`，大 buffer 通过 Port 的短读语义由 Pipeline 继续读取。
- 所有 Win32 错误在边界转换为稳定 `ErrorCode`，消息不包含客户路径或数据。
- Volume Enumerator 不把“无权限读取 extents”误报成“不存在该 Volume”。

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

## 测试

- 普通单元测试只使用临时文件，不依赖管理员权限、真实 Volume 或 VSS Service。
- 覆盖正确读取、非零 offset、EOF、越界、取消、缺失文件和 Device Path 拒绝。
- Shadow Copy 路径验证使用纯字符串样本。
- 真实 Volume、跨盘 Volume、无介质设备、访问拒绝和 VSS 生命周期进入单独集成测试。

## 安全与可观测性

- 错误不得输出完整源路径。
- 不请求写权限，不使用 `FILE_FLAG_NO_BUFFERING`，不要求调用方提供对齐缓冲区。
- 后续 VSS Session 记录 Snapshot Set ID、Snapshot ID、阶段耗时和清理结果，但不记录客户数据。

## Definition of Done

- Windows SDK 类型未泄漏到公共头或核心模块。
- Block Source 通过 `IBlockSource` 关键契约测试。
- 普通文件模式无法打开 Device Namespace，Snapshot 模式无法伪装任意设备。
- Debug/Release 构建、测试、clang-tidy 与源码规模检查通过。
