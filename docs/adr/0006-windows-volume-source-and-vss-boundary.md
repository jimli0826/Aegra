# ADR-0006：Windows Volume Source 与 VSS 生命周期边界

- 状态：Accepted
- 日期：2026-08-02
- 决策者：Aegra 项目
- 关联模块：ports、adapters/windows_disk、adapters/windows_vss、application

## 背景

个人版下一阶段需要从真实 Windows Volume 读取块数据。在线 Volume 在备份期间可能持续变化，直接
读取 `\\.\C:`、Volume GUID Device 或 `PhysicalDrive` 不能提供文件系统一致性，也与 V6 规定的
VSS 语义冲突。另一方面，块读取、Volume Inventory 和 VSS Snapshot Set 是不同生命周期：把 COM/VSS
状态放进 `IBlockSource` 会让通用 Pipeline 隐式控制快照，并使资源清理和多 Volume 一致性无法表达。

## 决策

1. `adapters/windows_disk` 实现 Volume Inventory 和 Windows Block Source；Windows SDK 类型只存在于
   Adapter 的 `.cpp`/私有头文件，公共接口只暴露 Aegra 与标准库类型。
2. Windows Block Source 只读取已经稳定的对象。公开请求仅允许普通稳定文件和 VSS Snapshot Device
   Object；不提供在线 Volume 或 `PhysicalDrive` 枚举值。
3. VSS Snapshot Device 路径必须严格匹配
   `\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy<decimal>`，并由调用者提供 Inventory 阶段确定的
  逻辑大小。稳定文件模式拒绝 Win32 Device Namespace，避免用文件模式绕过 VSS 约束。
4. Block Source 使用重叠 I/O，每次读取拥有独立 `OVERLAPPED` 与 Event，因此逻辑 offset 不依赖共享
  文件指针，并发读取不会互相覆盖。取消时调用 `CancelIoEx` 并等待本次 I/O 完成清理。
5. Volume Enumerator 使用 Volume GUID Path 作为稳定标识，收集 mount points、文件系统、容量、
   cluster size 和 disk extents。文件系统未就绪或 extent 查询无权限时仍保留 Volume 记录，并通过
   capability 字段表达缺失信息。供 Snapshot Block Source 使用的逻辑大小来自
   `IOCTL_DISK_GET_LENGTH_INFO`，不能用受文件系统与 quota 影响的可用容量代替。
6. 后续 `adapters/windows_vss` 独立实现 Snapshot Set 生命周期：一次 Session 为所选全部 Volume 创建
  同一 Snapshot Set，成功后把每个 Snapshot Device Object 与原 Volume identity 绑定；析构、失败和
  取消路径都删除临时 Snapshot。
7. Application 负责先做 Inventory 与选择，再创建 VSS Session，最后为每个 Snapshot Device 创建
   source-scoped Block Source。Pipeline 不依赖 Windows 或 VSS。

## 备选方案

- Block Source 内部按 Volume 自动创建 VSS：无法自然表达多 Volume 同一 Snapshot Set，也隐藏昂贵且
  有副作用的生命周期，不采用。
- 直接读取在线 Volume：无法保证一致性，违反 V6 规范，不采用。
- 整盘备份直接读取 `PhysicalDrive`：绕过 VSS，且会同时读取文件系统空闲区，不采用。
- 在 `ports` 中暴露 VSS GUID、COM 接口或 Windows Handle：污染平台无关核心，不采用。

## 影响

- 阶段 8A 独立交付 Inventory 与稳定 Block Source；阶段 8B 以独立 Target 接入 VSS Session，未修改
  `IBlockSource`。VSS COM 对象在 Adapter 专用 MTA 线程中完成创建、使用和释放。
- 普通稳定文件模式可用于确定性契约测试和未来离线镜像输入，但不能打开 Win32 Device Namespace。
- Disk layout / partition table 已由 `inspect_physical_disk_layout` 写入备份 Manifest（Restore Source
  Disks 与旧 LayoutCollector 对齐）；`raw_layout`（MBR/GPT 原始扇区）在布局采集时尽量写入 Manifest
  （`PhysicalDrive` 需 `GENERIC_READ`；读取失败不阻断卷备份），供 Full 整盘还原重建分区表。

## 验证

- 单元测试使用临时普通文件覆盖随机 offset、EOF、越界、短读、取消和非法 Device Path。
- 纯路径测试覆盖合法/非法 Shadow Copy Device Object，拒绝尾随组件和非十进制编号。
- Volume Enumerator 的真实系统测试作为显式 Windows 集成套件运行，不进入普通单元测试。
- Debug、Release、clang-tidy、源码规模检查和 `git diff --check` 作为质量门禁。
