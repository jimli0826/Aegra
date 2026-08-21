# NTFS 小目标卷恢复开发计划（SR0-SR10）

| 属性 | 内容 |
| --- | --- |
| 状态 | SR0–SR9 已完成；SR10 构建/静态审计已完成，人工矩阵 M01–M26 仍阻塞；Debug/Release capability 已按 2026-08-21 产品决策开启 |
| 日期 | 2026-08-20 |
| 决策依据 | [ADR-0009：Windows 卷恢复安全](../adr/0009-windows-volume-restore-safety.md)；[ADR-0025：NTFS 小目标卷恢复](../adr/0025-ntfs-smaller-target-volume-restore.md) |
| 架构依据 | [模块化目标架构](../architecture/MODULAR_ARCHITECTURE.md) |
| 工程依据 | [C++ 工程开发规范](CPP_ENGINEERING_STANDARD.md) |
| 适用范围 | Personal Archive NTFS volume 到更小 Windows NTFS volume 的离线恢复 |

本文定义 Aegra 支持“目标卷小于源卷”的 NTFS volume restore 生产实施计划。方案采用
“恢复目标范围内前缀、拼接源镜像尾部、离线移动超界簇、修改 NTFS 元数据”的核心方法，
但不复制旧仓库代码，也不继承其容量混用、线性内存 Overlay、固定数组、预检不完整、并发竞态、
可选文件系统校验和失败状态不明确等行为。

本文面向接手开发的 agent，规定架构边界、开发顺序、破坏性写入门槛、失败语义、人工验证矩阵和
发布门禁。不可逆架构决策必须先写入并接受 ADR-0025。2026-08-21 的产品决策允许 Debug/Release 都宣告
`restore.ntfs_shrink.v1` 以完成标准构建验证；SR10 与 M01–M26 仍决定对外发布资格。

## 1. 最终结果

完成全部工作包后，生产代码必须满足：

1. Personal Archive 中的 NTFS volume 可以恢复到比源逻辑卷小、但足以容纳全部已分配数据和 NTFS
   元数据的目标卷。
2. 目标卷不小于源卷时继续使用现有直接恢复路径，其合同、性能和失败语义不发生变化。
3. 缩容路径在打开目标卷进行破坏性写入前，完整扫描 Boot Sector、MFT、Attribute List、Runlist、
   `$Bitmap` 和所有超界分配，生成不可变、可校验的迁移计划。
4. 源逻辑大小和目标物理容量始终是两个独立值；任何组件不得虚报目标容量或把越界写静默丢弃。
5. 前缀恢复只写入目标物理范围，并保护 Primary Boot Sector、目标 Backup Boot Sector 和计划声明的
   延迟提交区域。
6. NTFS 修改在“目标卷前缀 + 源归档尾部 + 磁盘稀疏 Overlay”组成的复合块设备上执行；Overlay
   具有硬配额、校验值和有界内存索引。
7. 所有可达 NTFS data run 在提交前都位于新边界内；内部结构审计失败时不得提交 Primary Boot Sector。
8. Backup Boot Sector 先提交并 flush/readback，Primary Boot Sector 最后提交并 flush/readback。
9. 破坏性阶段取消或崩溃后，任务不得声称回滚成功；未提交 Primary Boot Sector 时目标卷保持
   fail-closed，用户必须重新执行完整恢复。
10. 恢复成功必须包含受控执行 `chkdsk.exe /x /f`、成功退出和卷状态复核；CHKDSK 不是可选诊断。
11. 所有不支持的 NTFS 布局、几何不匹配、空间不足和 Scratch 不足都必须在目标写入前稳定拒绝。
12. 不新增旧仓库兼容逻辑、协议 fallback、字段 alias、自动化测试代码或测试专用 executable。

## 2. 范围与非目标

### 2.1 首版支持范围

- Personal Archive `volume_set` 中的单个 NTFS volume；
- volume-to-volume 恢复；
- 非系统卷、非启动卷、非当前 Archive 所在卷；
- 未启用 BitLocker 的目标卷；
- 源 BPB 与目标设备逻辑/物理扇区几何兼容；
- 目标容量能放下所有源已分配簇、重定位保留空间和最终 NTFS 元数据；
- 现有增量链合并后的只读逻辑卷视图。

### 2.2 首版明确拒绝

- whole-disk restore 中通过缩小分区实现的小盘恢复；
- FAT、FAT32、exFAT、ReFS、RAW 或未知文件系统；
- 系统卷、启动卷、BitLocker 目标、在线文件级原地缩容；
- 源卷或目标几何不能被完整确认的设备；
- 迁移边界外存在首版不支持的 compressed/encrypted extent；
- 无法完整解析或重新编码的 `$ATTRIBUTE_LIST`、未知关键属性或损坏元数据；
- 中断后的 MFT 级断点续作；
- 通过临时创建完整源大小 VHD 再调用 Windows 在线缩容的替代路径。

### 2.3 后续范围

在 volume-to-volume 达到发布门禁后，whole-disk restore 才可单独立项，在用户提供的缩小分区布局内复用
同一 NTFS Resize Engine。不得在首版同时修改现有 disk layout edit、签名保留和 auto-expand 语义。

## 3. 核心不变量

### 3.1 容量和几何

- `source_logical_size_bytes` 表示归档逻辑卷大小；
- `target_capacity_bytes` 表示目标设备真实可写容量；
- `new_ntfs_volume_size_bytes` 表示 BPB `TotalSectors * bytes_per_sector`，不含位于
  `TotalSectors` 零基扇区位置的末尾 Backup Boot；目标原始设备大小比它多一个 NTFS sector；
- 三个字段不得复用、别名化或由同一 `capacity()` 隐式表达；
- 所有字节、扇区、簇、LCN、VCN 和 offset 转换使用命名类型或语义明确的字段；
- 所有 `offset + size`、`cluster * cluster_size` 和数量转换必须使用 checked arithmetic；
- 所有范围使用半开区间 `[begin, end)`，禁止混用 inclusive end。

### 3.2 预检先于写入

精确预检必须只读取 Archive 和目标几何，不锁定、不卸载、不写目标卷。预检必须证明：

1. 源文件系统可完整解析；
2. 目标几何兼容；
3. 所有边界外已分配簇均可在边界内获得目标位置；
4. 所有受影响 MFT record 的最终属性可以合法编码；
5. 关键元数据迁移顺序可执行；
6. Scratch 配额和物理空闲空间足够；
7. 不存在未知关键布局或不支持属性；
8. 迁移计划可序列化、校验并在执行前重新验证。

任何条件不能被证明时按不支持或损坏处理，不允许“先写一部分再尝试”。

### 3.3 Fail-closed 提交

- 锁卷成功后首先使目标 Primary Boot Sector 和目标末扇区不可作为有效 NTFS Boot 使用；
- 前缀恢复不得覆盖受保护扇区；
- NTFS Engine 对 Boot 的写入先进入 commit escrow；
- 提交前内部审计必须确认无可达 LCN 超过新边界；
- Backup Boot Sector 先于 Primary Boot Sector 提交；
- Primary Boot Sector 成功 flush/readback 是“NTFS 可被挂载”的提交点；
- 提交点前失败，目标保持不可挂载；提交点结果不确定时返回 outcome unknown，而不是普通失败。

### 3.4 取消

- 精确分析阶段可立即取消，目标不发生变化；
- Boot invalidation 后只能在有界 I/O/checkpoint 边界响应取消；
- 取消不得尝试以未经验证的反向 MFT 修改伪造回滚；
- Boot commit 和 CHKDSK 阶段延迟取消，必须先达到可判断的稳定状态；
- Worker 线程必须由 `std::jthread`、CancellationToken、原子状态或有界队列协调并在退出前 join。

## 4. 目标架构

```text
Personal Archive chain
        |
        v
PersonalArchiveVolumeRandomReader
        |
        +---- read-only full NTFS analysis ----> Immutable ShrinkPlan
        |                                           |
        |                                           +-- source/target geometry
        |                                           +-- plan hash
        |                                           +-- relocation records
        |                                           +-- metadata mutations
        |                                           +-- protected sectors
        |                                           +-- scratch upper bound
        |
        v
Bounded prefix restore ------------------------> Windows target volume
        |                                         [0, target_capacity)
        |                                         protected Boot sectors skipped
        v
Composite NTFS block device
        +-- target range: Windows random-access block device
        +-- source tail: archive random-access reader
        +-- modified source tail: sparse scratch overlay
        |
        v
NTFS relocation executor
        |
        v
Composite structural auditor
        |
        v
Backup Boot -> flush/readback -> Primary Boot -> flush/readback
        |
        v
Controlled CHKDSK /x /f -> volume status verification
```

## 5. 模块与依赖方向

### 5.1 新增模块

ADR-0025 应确认以下新模块名称和层级：

| Target | 建议路径 | 职责 | 允许依赖 |
| --- | --- | --- | --- |
| `Aegra::NtfsCore` | `src/ntfs_core` | 安全解析/编码 Boot、MFT、属性、Attribute List、Runlist、Bitmap | Base、Ports |
| `Aegra::NtfsResize` | `src/ntfs_resize` | 只读分析、计划生成、复合设备、重定位、提交前审计 | Base、Ports、NtfsCore |

现有 `Aegra::AdapterNtfs` 改为依赖 `Aegra::NtfsCore`，继续只公开 Explorer 所需的只读合同。
`Aegra::NtfsResize` 不依赖 Windows Disk、Personal Archive、Storage Local、Qt 或 Service。具体实现只在
`apps/worker` composition root 装配。

### 5.2 Ports

在 `src/ports/include/aegra/ports` 增加小而专一的能力接口：

```cpp
struct BlockDeviceGeometry final {
    std::uint32_t logical_sector_size{};
    std::uint32_t physical_sector_size{};
    std::uint64_t capacity_bytes{};
};

class IRandomAccessBlockDevice : public IBlockSink {
  public:
    [[nodiscard]] virtual BlockDeviceGeometry geometry() const noexcept = 0;
    [[nodiscard]] virtual base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) = 0;
};
```

具体签名前必须审查 `IBlockSink` 的继承是否造成不必要耦合；无论采用继承还是适配 view，现有
`IBlockSink` 合同不得获得平台语义。

Scratch Port 至少表达：

- 随机读写；
- 逻辑大小；
- 已分配物理字节数；
- 最大允许分配字节数；
- flush；
- 页面校验；
- close/discard 生命周期。

### 5.3 Windows Adapters

`Aegra::AdapterWindowsDisk` 增加目标卷随机读能力、几何查询、写后 readback 和显式锁卷生命周期。
Windows 本地文件和稀疏 Overlay 必须使用 Win32 API 与 RAII handle，不得使用 iostream file stream。

Scratch 可由 `Aegra::AdapterStorageLocal` 的 Windows 实现提供，或由 ADR-0025 决定建立独立
Windows Scratch Adapter。Adapter 之间不得直接构造或引用彼此；Worker 只通过 Ports 组合它们。

### 5.4 Pipeline

`RestorePlan` 增加语义明确的逻辑写上限，建议名称为 `logical_write_limit_bytes`：

- 默认值表示完整恢复；
- 只有显式缩容策略可以设置为目标容量；
- Pipeline 仍完整验证全部 Archive descriptor；
- 对跨越写上限的最后一个 chunk 做精确裁剪；
- Pipeline 只写目标范围，不负责 NTFS 重定位；
- 受保护范围由专用 sink view 拦截，不允许“写后再修复 Boot”。

## 6. ShrinkPlan 合同

ShrinkPlan 是分析阶段与执行阶段之间的不可变合同。它可以保存在 Worker Scratch 中，但不进入 Personal
Archive 或 Repository 的持久化格式。最少包含：

```text
plan_version
source_chain_fingerprint
source_volume_index
source_logical_size_bytes
source_boot_digest
source_ntfs_geometry
target_stable_id_digest
target_device_geometry
target_capacity_bytes
new_total_sector_count
new_total_cluster_count
minimum_target_bytes
scratch_upper_bound_bytes
protected_ranges[]
relocation_records[]
metadata_mutations[]
critical_file_operations[]
plan_payload_digest
```

每个 relocation record 至少表达源 LCN 范围、目标 LCN 范围、cluster count、所属 MFT record、attribute
identity 和计划顺序。不得只用 vector 下标隐式关联 Bitmap、MFT record 和目标簇。

计划要求：

- 生成后只读；
- 大型记录可以顺序写入 Scratch spool；
- Header、每段和最终 payload 均有长度、版本和校验值；
- 解析时拒绝截断、重复、重叠、越界和未知关键版本；
- 执行前重新比对 source chain、target ID、capacity 和 geometry；
- 不记录密码、密钥、token、Archive credential 或数据内容。

## 7. Service 和 Worker 状态机

### 7.1 两级 Preflight

Service 快速筛选只决定“是否值得进行精确分析”：

- 恢复类型为 volume；
- Manifest filesystem 为 NTFS；
- Target 不是系统/启动/Archive 所在卷；
- Manifest 已用空间下界不明显超过 Target；
- Source/Target 基础几何存在。

Worker 精确预检才是写入资格的权威来源。它打开归档随机访问视图，完整扫描 NTFS，生成 ShrinkPlan，
但不打开 Target 写句柄。Service 返回给 Desktop 的确认 token 必须绑定：

- recovery point 和 chain fingerprint；
- source volume index；
- target stable ID、capacity 和 geometry；
- `VolumeSizePolicy`；
- ShrinkPlan digest；
- token expiration。

Start 时重新获取 Target inventory 并复核所有绑定字段。任何变化返回 `restore.shrink_plan_changed`，要求
重新执行 preflight。

### 7.2 执行状态机

```text
Analyzing
  -> Planned
  -> TargetLocked
  -> BootInvalidated
  -> PrefixRestored
  -> RelocatingData
  -> UpdatingMetadata
  -> Auditing
  -> CommittingBackupBoot
  -> CommittingPrimaryBoot
  -> RunningChkdsk
  -> VerifyingVolumeState
  -> Completed
```

所有状态只允许单向前进。任务日志记录状态、字节数、record count、plan digest 和稳定错误码，不记录
Archive 密码、密钥、token、明文簇内容或完整 MFT record。

## 8. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 前置 |
| --- | --- | --- | --- | --- |
| SR0 | 已完成 | Gate | ADR、NTFS 日志/Dirty/挂载技术验证 | 无 |
| SR1 | 已完成 | P0 | Contracts、稳定码和两级 Preflight 合同 | SR0 |
| SR2 | 已完成 | P0 | 随机读写块设备与稀疏 Scratch Ports/Adapters | SR0 |
| SR3 | 已完成 | P0 | NtfsCore 提取和动态 Runlist 编码 | SR0 |
| SR4 | 已完成 | P0 | 完整 NTFS 分析器与不可变 ShrinkPlan | SR1-SR3 |
| SR5 | 已完成 | P0 | 有界前缀恢复、受保护范围和复合块设备 | SR2、SR4 |
| SR6 | 已完成 | P0 | 普通数据簇重定位与 metadata 更新 | SR3-SR5 |
| SR7 | 已完成 | P0 | 关键 NTFS 文件重定位与提交前审计 | SR6 |
| SR8 | 已完成 | Gate | 崩溃/取消语义、Boot commit、CHKDSK | SR0、SR7 |
| SR9 | 已完成 | P1 | Service、Worker、协议和 Desktop 集成 | SR1、SR4、SR8 |
| SR10 | 进行中 | Gate | 生产构建、静态审计、人工矩阵和发布门禁 | SR0-SR9 |

公共合同、协议、CMake、NtfsCore 公共头和 Worker 状态机文件不得由多个 agent 同时修改。工作包状态只
允许 `等待前置`、`可开始`、`进行中`、`阻塞`、`已完成`。

## 9. SR0：ADR 和技术验证

**目标：** 在生产实现前解决无法通过普通代码审查推断的 NTFS 日志和 Windows 挂载语义。

**任务：**

1. 新增 ADR-0025，冻结模块、端口、失败原子性、取消、Scratch、Boot commit 和 CHKDSK 决策。
2. 检查 VSS 创建的备份镜像中 `$Volume` dirty flag、`$LogFile` restart area 和日志序列状态。
3. 明确离线重写 MFT/Bitmap 后，旧 `$LogFile` 如何失效或重新初始化。
4. 验证目标 Boot Sector 无效时，Windows Mount Manager 不会自动将部分恢复卷暴露给用户。
5. 验证 `FSCTL_LOCK_VOLUME`、dismount、flush、handle close 和重新打开顺序。
6. 验证通过 `GetSystemDirectoryW` 定位并由 `IProcessLauncher` 启动 `chkdsk.exe /x /f` 的生产路径。
7. 定义 CHKDSK exit code 到稳定结果的映射，并明确取消期间不得终止 CHKDSK。
8. 决定是否以及何时使用 `FSCTL_MARK_VOLUME_DIRTY`；未验证前不得把它作为正确性证明。

参考 Windows 语义：

- [FSCTL_LOCK_VOLUME](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume)
- [FSCTL_MARK_VOLUME_DIRTY](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-mark-volume-dirty)
- [FSCTL_IS_VOLUME_DIRTY](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-is-volume-dirty)
- [CHKDSK](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk)

**DoD：** ADR 接受；日志/Dirty/挂载/CHKDSK 的成功、失败和 outcome-unknown 语义有人工验证记录；没有
未决数据完整性问题被推迟到实现阶段。

## 10. SR1：Contracts、稳定码和 Preflight

**主要文件：**

- `src/contracts/include/aegra/contracts/job.h`
- `src/contracts/include/aegra/contracts/service_control.h`
- `src/contracts/src/job.cpp`
- `src/contracts/src/service_control.cpp`
- `src/ports/include/aegra/ports/control_plane.h`
- `src/application/src/restore_preflight_service.cpp`
- `src/apps/service/src/worker_job_service_restore.cpp`
- `docs/modules/contracts.md`
- `docs/modules/application.md`
- `docs/protocol/SERVICE_CONTROL_PROTOCOL_V4.md`

**任务：**

1. 增加 `VolumeSizePolicy { kRequireSourceSize, kAllowNtfsRelocation }`，禁止布尔开关。
2. Preflight DTO 增加 feasibility 状态：`ineligible`、`provisional`、`eligible`。
3. 增加最小 Target、relocation bytes、Scratch 上界、计划 digest、限制原因和 warning DTO。
4. Control Plane record 持久化 token 绑定字段；产品未发布，直接更新 current schema，不写 migration/fallback。
5. Service 快速筛选返回 provisional，不以 Manifest free size 冒充精确资格。
6. Start revalidation 识别 direct 和 shrink 两条路径；direct 继续要求 `target >= source`。
7. 定义稳定错误码和 Desktop message key。

**稳定码：**

| Code | 条件 |
| --- | --- |
| `restore.shrink_not_ntfs` | 源文件系统不是受支持 NTFS |
| `restore.shrink_sector_mismatch` | Source/Target 扇区几何不兼容 |
| `restore.shrink_below_minimum` | Target 小于精确最小容量 |
| `restore.shrink_unsupported_layout` | NTFS 布局或属性不受支持 |
| `restore.shrink_scratch_insufficient` | Scratch 配额或物理空间不足 |
| `restore.shrink_plan_changed` | Preflight 后 source/target/plan 发生变化 |
| `restore.shrink_plan_corrupt` | ShrinkPlan 截断、校验失败或内部冲突 |
| `restore.shrink_target_incomplete` | 提交前取消/失败，目标保持不可挂载 |
| `restore.shrink_postcheck_failed` | CHKDSK 或卷状态复核失败 |
| `restore.shrink_commit_outcome_unknown` | Primary Boot commit 结果无法确认 |

**DoD：** 所有消费者使用同一 enum 和字段语义；无字符串猜测、旧字段 alias 或隐式 allow-smaller 分支。

## 11. SR2：块设备和 Sparse Scratch

**主要文件：**

- `src/ports/include/aegra/ports/block_io.h`
- 新增 `src/ports/include/aegra/ports/random_access_block_device.h`
- 新增 Scratch Port 头文件
- `src/adapters/windows_disk/include/aegra/adapters/windows_disk/windows_disk.h`
- `src/adapters/windows_disk/src/windows_block_sink.cpp`
- `src/adapters/storage_local/*` 或 ADR 指定的新 Adapter
- `docs/modules/ports.md`
- `docs/modules/windows_adapters.md`
- `docs/modules/storage_local.md`

**任务：**

1. 增加设备几何和随机读能力，保持 `IBlockSink` 现有消费者兼容。
2. Windows Target 实现严格范围、扇区对齐、short-read 拒绝、flush 和 readback。
3. 将 volume lock/dismount 生命周期放入 RAII 对象，异常和取消路径不能泄漏 handle 或锁状态。
4. 实现 Windows sparse file Scratch，创建时确认路径不在 Target volume。
5. Overlay 使用 page map 或 interval index；不得对每次访问线性扫描全部写记录。
6. 内存索引受 `memory_budget_bytes` 控制；溢出时使用分段索引或有界缓存，而不是无界增长。
7. Scratch 具有 hard quota；超过上限立即返回稳定错误，不只记录 warning。
8. Scratch page 和计划段写入长度、offset、checksum；读取时 fail closed。

**DoD：** Target 永远拒绝真实容量之外的写；Scratch 不依赖目标卷、不无界占用内存、损坏可被检测。

## 12. SR3：NtfsCore

**主要文件：**

- `src/adapters/ntfs/*`
- 新增 `src/ntfs_core/*`
- 新增 CMake Target 和模块文档
- `docs/architecture/MODULAR_ARCHITECTURE.md`
- `docs/modules/adapters.md`

**任务：**

1. 从只读 Adapter 提取 Boot、USA fixup、MFT record、attribute、Attribute List、Runlist 和 Bitmap codec。
2. 保持 Explorer API 只读，不向 Shell Extension 暴露 write/mutate/commit 类型。
3. 新增动态 Runlist encoder，先计算编码长度，再在受控 buffer 中输出。
4. 编码前验证 VCN 连续性、LCN delta、signed width、terminator、record space 和 attribute length。
5. 删除固定大小临时数组；使用 `vector`、`span` 和显式上限。
6. 为 ClusterIndex、VirtualClusterNumber、ByteOffset 等建立不可混用的语义类型或强命名字段。
7. 解析未知 critical attribute、循环 Attribute List、重叠 run、溢出 run 时稳定拒绝。

**DoD：** `Aegra::AdapterNtfs` 的 Explorer 行为不变；NtfsCore 无 Windows/Qt/Archive 依赖；编码器可提前判断
最终 record 是否可容纳修改后的属性。

## 13. SR4：完整分析器与 ShrinkPlan

**主要文件建议：**

- `src/ntfs_resize/include/aegra/ntfs_resize/ntfs_shrink_analyzer.h`
- `src/ntfs_resize/src/ntfs_shrink_analyzer.cpp`
- `src/ntfs_resize/src/ntfs_mft_scanner.cpp`
- `src/ntfs_resize/src/ntfs_bitmap_allocator.cpp`
- `src/ntfs_resize/src/ntfs_relocation_plan.cpp`
- `src/ntfs_resize/src/shrink_plan_codec.cpp`

**任务：**

1. 通过 `IRandomAccessReader` 打开合并后的 Archive volume view。
2. 比对 Manifest、Boot Sector、volume size、cluster size 和 source volume index。
3. 扫描 `$Bitmap`，计算边界内可用簇和边界外已分配簇。
4. 扫描全部相关 MFT records 和 Attribute List，建立 owner/attribute/run 映射。
5. 预留 Backup Boot、MFT 扩展、Bitmap 修改和关键元数据所需空间。
6. 使用确定性策略从边界内空闲簇分配目标范围；相同输入生成相同 plan digest。
7. 预编码所有受影响 data runs，确认每个 MFT record 的最终大小和 USA 区域合法。
8. 计算 `minimum_target_bytes`、relocation bytes、record count 和 Scratch upper bound。
9. 对 compressed/encrypted tail、未知属性、损坏或未实现 critical relocation 在此阶段拒绝。
10. 完成计划后执行内部一致性审计：目标范围不重叠、不使用源已占用且未释放的簇、不超界。

**DoD：** Analyzer 对 Target 零写入；所有执行阶段可能遇到的布局拒绝都已经前移；ShrinkPlan 可重新加载并
通过 digest 和结构校验。

## 14. SR5：前缀恢复和复合块设备

**主要文件：**

- `src/pipeline/include/aegra/pipeline/restore_pipeline.h`
- `src/pipeline/src/restore_pipeline.cpp`
- `src/apps/worker/src/personal_archive_restore_task_backend.cpp`
- 新增 protected-range sink view
- `src/ntfs_resize/src/composite_ntfs_block_device.cpp`
- `src/ntfs_resize/src/sparse_overlay_index.cpp`

**任务：**

1. RestorePipeline 支持显式逻辑写上限，默认完整恢复语义不变。
2. 继续完整校验 descriptor、chunk 顺序、FREE range 和 logical end，再裁剪实际写范围。
3. 在 Target lock/dismount 后先使 Primary/Backup Boot 无效并 flush/readback。
4. 使用 protected-range sink 跳过 Primary Boot、目标末扇区和 plan 指定范围。
5. 前缀恢复只写 `[0, target_capacity)`，不得构造 fake-capacity sink。
6. 复合设备 read precedence 为 Overlay、Target prefix、Archive source tail。
7. Source Boot 读取由 Archive/escrow 提供，不能读已失效的 Target Boot。
8. 对 Target 之外的修改只进入 Scratch Overlay；任何真实 Target 越界写都是编程错误并 fail closed。

**DoD：** 直接恢复路径不变；缩容 prefix 完成后可通过复合设备看到完整源逻辑卷，同时 Target 保持不可挂载。

## 15. SR6：普通数据簇重定位

**主要文件建议：**

- `src/ntfs_resize/src/ntfs_cluster_mover.cpp`
- `src/ntfs_resize/src/ntfs_metadata_editor.cpp`
- `src/ntfs_resize/src/ntfs_bitmap_commit.cpp`
- `src/ntfs_resize/src/ntfs_record_writer.cpp`

**执行顺序：**

1. 从复合视图读取源尾部簇；
2. 写入计划分配的 Target 内新簇；
3. flush 并按块 readback 校验；
4. 更新对应 attribute data runs；
5. 应用 USA fixup 并写 MFT record；
6. 更新 `$Bitmap`，先占用新簇，再释放旧簇；
7. 更新统计信息，但不得提交 Boot Sector。

**要求：**

- 同一源范围只迁移一次；
- 重叠源/目标范围使用安全 copy 策略，不假定 memcpy 语义；
- progress 以已验证迁移字节计算，不以计划提交数猜测；
- 取消只在 record/extent checkpoint 响应；
- 任何 readback 不一致返回 Target I/O 错误并保持 fail-closed；
- 不使用裸 `new/delete`、固定数组、并行 vector 下标或跨线程裸状态字段。

**DoD：** 普通文件所有超界簇被迁移，MFT/Bitmap 对复合视图一致，无可达普通 data run 超界。

## 16. SR7：关键元数据和提交前审计

必须显式处理：

- `$MFT`；
- `$MFTMirr`；
- `$LogFile`；
- `$Volume`；
- `$AttrDef`；
- 根目录；
- `$Bitmap`；
- `$Boot`；
- `$BadClus`；
- `$Secure`、`$UpCase`、`$Extend` 下受影响的关键对象。

内部里程碑：

- M1：普通数据迁移，要求关键元数据原本位于新边界内；
- M2：关键元数据本身可以按计划迁移；
- M3：日志、Dirty 和 Boot 提交语义完成。

M1 或 M2 单独完成不代表 capability 可以开启。

提交前 Auditor 必须确认：

1. BPB 计划几何与 Target 一致；
2. MFT record 和镜像 record 满足一致性规则；
3. `$Bitmap` 与所有可达 data runs 相容；
4. 无可达 LCN 超过 `new_total_cluster_count`；
5. 新旧目标簇不存在非法共享；
6. Attribute List 无循环、悬挂或越界引用；
7. USA、record length、attribute length 和 runlist 均合法；
8. `$BadClus` 和保留范围位于新边界内；
9. Backup/Primary Boot escrow 内容一致；
10. Overlay 中不存在仍被最终文件系统引用、但无法落入 Target 的数据。

**DoD：** Auditor 可以在 Boot commit 前给出唯一的 pass/fail；任何 warning 不得替代失败。

## 17. SR8：Boot commit、取消、崩溃和 CHKDSK

**提交顺序：**

1. flush 所有数据和元数据修改；
2. readback 关键 MFT、Bitmap 和 Volume record；
3. 写 Target Backup Boot Sector；
4. flush + readback Backup Boot；
5. 写 Primary Boot Sector；
6. flush + readback Primary Boot；
7. 关闭原始句柄并保持卷不对用户暴露；
8. 通过 `IProcessLauncher` 启动系统目录中的 `chkdsk.exe /x /f <volume-guid>`；
9. 等待 CHKDSK 完成，不因普通取消请求强制终止；
10. 重新查询 volume dirty/mount 状态和关键几何；
11. 只有全部成功才进入 `Completed`。

**失败结果：**

| 阶段 | 结果 |
| --- | --- |
| 分析完成前 | 普通取消/失败，Target 未修改 |
| Boot invalidation 后、Primary commit 前 | `restore.shrink_target_incomplete`，必须完整重试 |
| Primary Boot 写入结果无法 readback 确认 | `restore.shrink_commit_outcome_unknown` |
| CHKDSK 或最终状态复核失败 | `restore.shrink_postcheck_failed`，不得报告成功 |

**DoD：** 在每个状态边界强制结束 Worker 后，结果均符合表中语义；不存在可挂载但任务报告
`target_incomplete` 的中间路径；不存在 CHKDSK 失败仍返回成功的路径。

## 18. SR9：Service、Worker、协议和 Desktop

**主要文件：**

- `src/apps/service/src/worker_job_service_restore.cpp`
- `src/apps/service/src/supervisor_worker_protocol.cpp`
- `src/apps/service/src/service_protocol_request_json.cpp`
- `src/apps/service/src/service_protocol_response_json.cpp`
- `src/apps/worker/src/worker_protocol.cpp`
- `src/apps/worker/src/personal_archive_restore_task*.cpp`
- `src/apps/desktop/qml/pages/RestorePage.qml`
- Desktop service protocol/model/translation 文件
- `docs/modules/worker_host.md`
- `docs/modules/windows_personal_restore.md`
- `docs/modules/apps.md`
- `docs/modules/desktop.md`
- `docs/protocol/SERVICE_CONTROL_PROTOCOL_V4.md`

**任务：**

1. Worker schema 显式携带 size policy、plan digest 和精确预检字段。
2. Service 当前三层“小于源即拒绝”改为策略感知；disk restore 仍拒绝小盘。
3. Worker 只在 `kAllowNtfsRelocation` 且 exact preflight 通过时进入 shrink state machine。
4. Worker 组合 Archive reader、Windows random-access target、Sparse Scratch、NtfsResize 和 ProcessLauncher。
5. Desktop 在较小 Target 被拖放时立即执行 target-bound 精确分析；只有 Target 不小于分析得到的最小容量时才接受映射，否则显示精确最小值并保持未映射。
6. Summary 不再首次触发 Analyze，只复用已签发的 eligible token，显示 Source/Target、精确最小容量、迁移字节、Scratch 上界、限制和失败后完整重试说明。
7. 取消按钮根据状态显示“可立即取消”或“正在完成安全提交/校验”。
8. 结果页区分未修改、target incomplete、postcheck failed 和 outcome unknown。
9. 更新五语言资源；稳定码必须有明确、可操作的 message。

**DoD：** 用户不会把 provisional 当作 eligible；Start token 防 TOCTOU；所有状态和风险均可从 Desktop 正确观察。

## 19. SR10：构建、审计和人工验证

本仓库不新增 unit、integration、regression、smoke、E2E、fuzz、fixture、测试脚本、测试 executable 或
CTest 注册。验证使用生产 Target 构建、静态/架构审计和可丢弃 VHD/VHDX 人工场景。

### 19.1 构建

```powershell
cmd.exe /d /c scripts\build.cmd Debug
cmd.exe /d /c scripts\build.cmd Release
git diff --check
```

至少覆盖生产 Target：

- `aegra_ports`
- `aegra_ntfs_core`
- `aegra_ntfs_resize`
- `aegra_adapter_ntfs`
- `aegra_adapter_storage_local`
- `aegra_adapter_windows_disk`
- `aegra_pipeline`
- `aegra_application`
- `aegra_app_worker_personal`
- `aegra_personal_worker`
- `aegra_app_service`
- `aegra_service`
- `aegra_desktop`
- `aegra_shell_extension`

构建使用 `C:\Program Files\Microsoft Visual Studio\18\Insiders`；Desktop 使用
`C:\Qt6\6.8.3\msvc2022_64`。`scripts\build.cmd` 完成后必须通过 source-limit 检查。

### 19.2 依赖与静态审计

- NtfsCore/NtfsResize 不包含 Windows、Qt、Archive 或 Service 头；
- Adapter 之间没有具体实现依赖；
- Shell Extension 未链接 NtfsResize；
- 所有 Windows 文件 I/O 使用 Win32 API 和 RAII handle；
- 没有拥有资源的裸指针、手写 `new/delete`、全局可变状态或未 join 线程；
- 函数最多 80 逻辑行、lambda 最多 30 行、嵌套最多四层、`.cpp` 最多 1500 物理行；
- offset/size/runlist/record arithmetic 全部经过溢出检查；
- 日志不包含密码、密钥、token、MFT 内容或用户文件数据；
- current protocol/schema 直接升级，没有 legacy fallback。

### 19.3 人工验证矩阵

所有场景使用隔离、可销毁、无生产数据的 VHD/VHDX。每项记录 plan digest、source/target geometry、状态日志、
CHKDSK exit code、最终 dirty 状态和文件哈希结果。

| ID | 场景 | 期望 |
| --- | --- | --- |
| M01 | Target 大于 Source | 走现有 direct restore，不进入 shrink |
| M02 | Target 等于 Source | 走现有 direct restore |
| M03 | 更小 Target，普通数据尾部碎片 | 成功迁移、CHKDSK 成功、文件哈希一致 |
| M04 | 已用空间接近 Target 极限 | 精确计算最小值并成功或写前拒绝 |
| M05 | Target 比最小值少一个 cluster | 精确预检拒绝，Target 零写入 |
| M06 | 大量 MFT record 和多层 Attribute List | 完整处理或写前稳定拒绝 |
| M07 | `$MFT` 超过新边界 | 关键元数据迁移成功 |
| M08 | `$MFTMirr`/`$Bitmap` 超过新边界 | 关键元数据迁移成功 |
| M09 | 稀疏文件尾部 run | 不为 sparse hole 分配簇，数据一致 |
| M10 | compressed tail extent | 首版写前拒绝 |
| M11 | encrypted tail extent | 首版写前拒绝 |
| M12 | Source dirty/log replay required | 按 ADR 规则写前拒绝或受控处理 |
| M13 | 512/512e/4Kn 几何不匹配 | 写前拒绝 |
| M14 | Target 是系统/启动/Archive 所在卷 | 写前拒绝 |
| M15 | Scratch 路径位于 Target | 写前拒绝 |
| M16 | Scratch 物理空间耗尽 | fail-closed、稳定错误 |
| M17 | prefix restore I/O 失败 | Target incomplete、不可挂载 |
| M18 | relocation read/write/readback 失败 | Target incomplete、不可挂载 |
| M19 | 每个状态边界请求取消 | 符合取消状态表，线程全部退出 |
| M20 | 每个状态边界强制结束 Worker | 重启后状态可判断，未提前暴露目标卷 |
| M21 | Backup Boot 写后异常 | Primary 仍无效，完整重试 |
| M22 | Primary Boot 写入 outcome unknown | readback 决策或返回 outcome unknown |
| M23 | CHKDSK 返回失败 | 不报告成功，保留诊断信息 |
| M24 | 完成后重启 Windows | 卷可挂载、非 dirty、文件哈希一致 |
| M25 | 增量 Archive chain | 合并视图恢复正确、文件哈希一致 |
| M26 | Direct restore 回归场景 | 性能和行为无 shrink 分支副作用 |

**DoD：** Debug/Release 全量生产构建通过；source-limit 和依赖审计通过；M01-M26 都有可复核记录；所有
破坏性失败场景使用可丢弃目标且未接触生产数据。

## 20. 已观察问题与 Aegra 防护

| 已观察问题 | Aegra 设计约束 |
| --- | --- |
| 目标设备报告源容量，真实写边界另行判断 | Source、Target、new NTFS size 三字段分离；Target 永远报告真实容量 |
| Bitmap 搜索索引和复制索引混用 | 强语义 ClusterRange 和单一 allocator API，不用并行数组隐式关联 |
| `new[]` 与 `delete` 不匹配 | RAII、Rule of Zero、禁止手写 `new/delete` |
| inclusive/exclusive 末簇混用 | 统一半开区间和 checked arithmetic |
| 固定 1024/4096 临时数组 | 动态有界容器和 Scratch spool；预先计算编码长度 |
| `$ATTRIBUTE_LIST` 支持不完整 | 完整解析/编码；任何未实现形式在写 Target 前拒绝 |
| Overlay 线性搜索且仅警告内存增长 | 磁盘稀疏 Overlay、索引查找、内存预算和硬配额 |
| 未验证扇区大小兼容 | Source/Target logical/physical geometry 明确比对 |
| 跨线程裸写返回码和进度 | 状态机、jthread、CancellationToken、原子或有界通道 |
| 边恢复边发现不支持布局 | 两遍完整预分析、预编码和不可变 ShrinkPlan |
| 直接修改且无明确崩溃边界 | Boot invalidation、延迟 Primary Boot commit、fail-closed |
| CHKDSK 可选 | CHKDSK 和最终卷状态复核是成功硬门槛 |
| 取消后目标状态不明确 | 分阶段稳定结果和完整重试语义 |

## 21. 发布门禁

`restore.ntfs_shrink.v1` 已按 2026-08-21 产品决策在 Debug/Release 中宣告。只有同时满足以下条件，才可把
该能力描述为通过发布验证并用于正式分发：

1. ADR-0025 已接受；
2. `$LogFile`、Dirty、Mount Manager 和 CHKDSK 语义已有验证结论；
3. SR0-SR10 全部标记已完成；
4. 普通数据和全部关键 NTFS 元数据均可迁移；
5. 所有 unsupported layout 均能在 Target 写入前拒绝；
6. ShrinkPlan 可重现、可校验并绑定 preflight token；
7. Boot 延迟提交和 outcome-unknown 路径完成；
8. CHKDSK 失败不会被报告为成功；
9. M01-M26 全部有通过记录；
10. Debug/Release 全量生产构建、source-limit、依赖和静态审计通过；
11. Service V4、模块文档、产品范围、稳定码和 Desktop 文案同步；
12. 未引入测试代码、旧格式兼容路径或旧仓库实现依赖。

未达到任一门禁时，构建仍可宣告 capability 用于受控验证，但不得标记为发布合格或分发为已验证产品。
Debug/Release 都必须执行完整 Analyze、eligible token、Start 重验证和 fail-closed 状态机。M01–M26
完成前应优先使用隔离、可销毁 VHD/VHDX；使用其它目标必须明确接受覆盖与失败后完整重试风险。

## 22. 完成交接格式

每个工作包完成时记录：

```text
工作包：SRx
状态：已完成 / 阻塞
基线提交：<commit>
修改文件：<paths>
生产 Targets：<built targets and configurations>
静态/边界检查：<results>
人工验证：<scenario IDs and results>
稳定码/协议/文档：<updated items>
未决风险：<none or explicit blockers>
下一工作包：<SRx>
```

不得用“编译通过”替代数据完整性验收，不得用 CHKDSK 自动修复成功掩盖内部 Auditor 已知失败，也不得
因 capability 已开启就把部分内部里程碑描述为发布验证完成。

## 23. 当前进度交接（2026-08-20）

```text
工作包：SR0
状态：已完成（决策冻结）；提权 VHD 破坏性补录挂 SR8/SR10
修改文件：docs/adr/0025-…、docs/development/NTFS_SHRINK_SR0_…、docs/adr/README.md
人工验证：chkdsk 系统路径已核；锁卷/Boot/CHKDSK 破坏性项待提权环境

工作包：SR1
状态：已完成
修改文件：contracts job/service_control、control_plane schema v20、sqlite restore_preflight、
  service prepare/start restore、protocol V4 §7.3a、Desktop prepare 请求 volume_size_policy=1、
  worker RestoreOptions 编解码
生产 Targets：Debug 构建通过（含 aegra_service / aegra_personal_worker / aegra_desktop /
  aegra_shell_extension）；source-limit 通过
行为：更小 NTFS + allow relocation → provisional；Start 拒绝非 eligible / allow-relocation；
  capability restore.ntfs_shrink.v1 未宣告

工作包：SR2
状态：已完成
修改文件：ports random_access_block_device / scratch_store；
  windows_disk WindowsRandomAccessBlockDevice；
  storage_local WindowsScratchStoreFactory（稀疏文件 + CRC 页图 + .idx spill）；
  docs/modules/windows_adapters.md、storage_local.md、ports.md
生产 Targets：aegra_adapter_windows_disk、aegra_adapter_storage_local Debug 通过；source-limit 通过

工作包：SR3
状态：已完成
修改文件：src/ntfs_core/*、AdapterNtfs 改依赖 NtfsCore、动态 runlist 编码器、模块文档
生产 Targets：aegra_ntfs_core、aegra_adapter_ntfs（及全量相关 Debug 链接）通过

工作包：SR4
状态：已完成
修改文件：src/ntfs_resize/*（ShrinkPlan AGSP codec、volume view、MFT 扫描、Bitmap 分配、
  relocation/pre-encode、audit、analyzer）；docs/modules/ntfs_resize.md
生产 Targets：aegra_ntfs_resize Debug 通过；source-limit 通过
限制：稀疏 hole 不分配；写 Target / 复合设备 / Boot commit 属 SR5+

工作包：SR5
状态：已完成
修改文件：RestorePlan.logical_write_limit_bytes、ProtectedRangeBlockSink、
  CompositeNtfsBlockDevice、SparseOverlayIndex、invalidate_ntfs_boot_sectors；
  Worker 容量预检尊重 write limit；pipeline/ntfs_resize 文档
生产 Targets：aegra_pipeline、aegra_ntfs_resize Debug 通过
限制：完整 shrink 状态机 / Boot invalidation 编排仍属 SR6–SR9 Worker 集成

工作包：SR6
状态：已完成
修改文件：ntfs_core seal_fixup；ntfs_cluster_mover / record_writer / metadata_editor /
  bitmap_commit / ordinary_relocation；composite 对受保护 Boot 读走 source escrow；
  docs/modules/ntfs_resize.md
生产 Targets：aegra_ntfs_core、aegra_ntfs_resize Debug 通过；source-limit 通过
限制：关键系统文件搬迁与提交前审计属 SR7；Boot commit/CHKDSK 属 SR8

工作包：SR7
状态：已完成
修改文件：RecordClassFilter 泛化 metadata/bitmap；critical_relocation、logfile_invalidation、
  mft_mirror_sync、precommit_auditor；docs/modules/ntfs_resize.md
生产 Targets：aegra_ntfs_resize Debug 通过；source-limit 通过
限制：Boot commit / CHKDSK / 取消崩溃语义属 SR8；Service/Desktop 集成属 SR9

工作包：SR8
状态：已完成
修改文件：ntfs_core patch_boot_geometry；ntfs_boot_commit、ntfs_chkdsk_runner、
  ntfs_shrink_finalize（含 IShrinkVolumePostcheck）；模块文档
生产 Targets：aegra_ntfs_core、aegra_ntfs_resize Debug 通过；source-limit 通过
限制：Worker 装配 GetSystemDirectoryW/CHKDSK/卷后检与 Desktop 集成属 SR9；破坏性矩阵属 SR10

工作包：SR9
状态：已完成
修改文件：contracts kind 18 AnalyzeNtfsShrink；Service sync exact analyze（NtfsResize）+
  Start 策略门禁与 Job 字段透传；Worker shrink 状态机（analyze→Boot invalidate→prefix→
  relocate→audit→finalize/CHKDSK）；windows_disk 只读几何/postcheck；Desktop capability 门控、
  拖放时精确 Analyze、容量比较、确认页与 shrink message_code；协议 V4 §7.3b；模块文档
生产 Targets：aegra_app_worker_personal、aegra_personal_worker、aegra_app_service、
  aegra_service、aegra_desktop Debug 通过；source-limit 通过
行为：自定义构建未宣告 capability 时 Analyze 返回 capability_unavailable；Desktop 无缩容入口；
  标准 Debug/Release 构建均宣告；直接恢复路径不变
限制：破坏性人工矩阵 M01–M26 与 Release 全量审计属 SR10

工作包：SR10（部分）
状态：进行中 — 构建与静态审计已完成；人工矩阵与发布资格未完成
修改文件：docs/development/NTFS_SHRINK_SR10_STATIC_AUDIT.md、
  NTFS_SHRINK_SR10_VERIFICATION_MATRIX.md、docs/README.md、本计划状态表
生产 Targets：Debug/Release 全量 `scripts\build.cmd` 通过（含 source-limit）
静态审计：NtfsCore/NtfsResize 纯净、Shell 未链 NtfsResize、capability Host 门禁存在 —
  见 SR10_STATIC_AUDIT（item 4 已复检 PASS：personal_archive/worker 改 Win32InputFile）
人工矩阵：M01–M26 见 SR10_VERIFICATION_MATRIX；破坏性项阻塞于提权隔离 VHD
capability：Debug/Release 已按 2026-08-21 产品决策宣告；门禁 9 仍未满足，不代表发布合格
下一动作：在管理员 + 可丢弃 VHDX 环境执行 M01–M26 并回填矩阵；全部通过后再标记发布合格
```
