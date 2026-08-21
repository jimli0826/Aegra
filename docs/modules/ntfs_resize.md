# `ntfs_resize` 模块开发文档

## 目标

实现 NTFS 小目标卷恢复（ADR-0025）中的只读分析与不可变 `ShrinkPlan` 合同：几何与绑定字段、
受保护范围、重定位记录、元数据变更、关键文件操作，以及可校验的二进制编解码。后续工作包在本模块
内扩展复合块设备、簇搬移与 Boot commit escrow。

## 非目标

- 不依赖 Windows Disk、Personal Archive、Storage Local、Qt 或 Service。
- 不写入 Target；分析阶段不得打开 Target 写句柄。
- 不把 ShrinkPlan 写入 Personal Archive 或 Repository 持久化格式（仅 Worker Scratch）。
- 不引入旧仓库兼容路径或测试 Target。

## 依赖

| 允许 | 禁止 |
| --- | --- |
| `Aegra::Base`、`Aegra::Ports`、`Aegra::NtfsCore` | Windows SDK、Qt、其它 Adapter、Archive、Service |

Target：`aegra_ntfs_resize` / `Aegra::NtfsResize`。路径：`src/ntfs_resize/`。

## 公共接口

公共头位于 `include/aegra/ntfs_resize/`：

| 头文件 | 职责 |
| --- | --- |
| `shrink_plan.h` | `kShrinkPlanVersion`、几何/范围 DTO、`RelocationRecord`、`MetadataMutation`、`CriticalFileOperation`、不可变 `ShrinkPlan`、`ShrinkPlanBuilder` |
| `shrink_plan_codec.h` | `encode_shrink_plan` / `decode_shrink_plan` |
| `ntfs_shrink_analyzer.h` | `NtfsShrinkAnalyzeRequest`、可选只读诊断 observer、`NtfsShrinkAnalyzer::analyze`（只读完整预分析） |

线程：编解码与 builder 无内部共享可变状态。错误：可预期失败返回 `Result` 与稳定 message code
（`restore.shrink_*`、`ntfs_resize.plan_*`）。取消：`analyze` 观察 `CancellationToken`。

### Analyzer 行为（SR4）

`NtfsShrinkAnalyzer::analyze` 仅通过 `IRandomAccessReader` 读取源卷视图，**不**打开 Target 写句柄：

1. 解析 Boot；按 `source_logical_size = BPB TotalSectors * sector_size + sector_size` 比对源逻辑大小，
   再验证目标扇区几何；末尾额外扇区是 Backup Boot，不得误报 sector mismatch；从 Boot MFT LCN 解析
   `$MFT` 记录 0 后，必须立即通过其 unnamed `$DATA` runlist 回读记录 0，以验证后续 MFT 映射路径；
2. 由目标设备真实扇区数计算 `new_total_sector_count = target_device_sectors - 1`，再计算
   `new_total_cluster_count = new_total_sector_count / sectors_per_cluster`；
3. 检查 `$Volume` dirty，并验证 `$LogFile` 双 restart page 的 USA、页几何、版本、LSN 与最新 clean flag；不能证明干净则写前拒绝；
4. 读取 `$MFT::$BITMAP`，只解析其中标记为 in-use 的 MFT 槽位；未初始化的全零槽位跳过，位图标记
   in-use 但记录头无效或 `FILE` flags 未置 in-use 则按损坏拒绝；随后收集跨越新边界的非稀疏 data run，
   compressed/encrypted 超界拒绝；
5. 基于 `$Bitmap` 在新边界内做确定性 first-fit 空闲簇分配（预留元数据簇预算）；
6. 生成 `RelocationRecord` / `MetadataMutation(kRunlistReplace)`；mutation 绑定 record sequence、属性位置/容量、preimage digest 与预编码替换字节；
7. 内部审计（目标不重叠、不超界、mutation 覆盖 outbound 属性）后经 `ShrinkPlanBuilder` 产出不可变计划。

可选 `INtfsShrinkAnalysisObserver` 在 Boot 解析成功后、源 reader size 校验之前报告候选源/目标几何，随后
报告计算后的扇区/簇数、分析阶段、MFT 解析失败记录的读取路径与原始头字段，以及分配预算；因此即使
source BPB size 校验或 MFT 解析早退，也能诊断 reader/NTFS size 差值、sector compatibility、runlist
映射偏移及具体失败记录。observer 非 owning，回调必须 `noexcept`，不得改变 eligibility、取消或错误结果。
Service 将高频回调汇总为候选/最终结果，Worker 可不注入。

**故意限制（首版分析）：** 稀疏 hole 不分配簇；复杂 Attribute List 边角若无法安全合并则拒绝。

### 复合块设备与 Boot 失效（SR5）

| 头文件 | 职责 |
| --- | --- |
| `composite_ntfs_block_device.h` | 源逻辑卷复合视图：Overlay → Target 前缀 → Archive 尾部 |
| `boot_sector_invalidation.h` | 锁卷后使 Primary/Backup Boot 失效并 flush/readback |

`CompositeNtfsBlockDevice`：

- `target_capacity_bytes()` 永远是 Target 真实容量；
- `source_logical_size_bytes()` 是 Archive 合并逻辑大小；
- Target 外写入只进 `IScratchStore` Overlay，并用 `SparseOverlayIndex`（按页索引）记录命中；
- Overlay 首次局部写先按 Target/Archive 基础视图水合完整 64 KiB 页面，避免边界页未写字节被零值覆盖；
- 受保护 Boot 范围**读取**始终来自 Archive/source escrow，不读已失效 Target Boot；
- 受保护范围上的 Target 写入返回 `ntfs_resize.protected_write`。
- NTFS 属性允许非扇区整数倍的字节范围；复合设备通过对齐 bounce read 与首尾扇区
  read-modify-write 访问 `IRandomAccessBlockDevice`，底层裸卷 Adapter 仍只接收扇区对齐请求。

### 普通数据重定位（SR6）

| 头文件 | 职责 |
| --- | --- |
| `ntfs_ordinary_relocation.h` | `NtfsOrdinaryRelocationExecutor::execute` |

执行顺序：按 `plan_order` 搬移普通文件（MFT record > 11）簇 → flush/readback → 更新 runlist 并
`seal_fixup` 写回 MFT → `$Bitmap` 先置位新簇再清除旧簇。不搬迁关键系统文件，不提交 Boot。
重叠源/目标 LCN 使用临时缓冲拷贝。进度以已验证迁移字节为准。
对应 record class 没有重定位记录时，Bitmap commit 为无操作，不读取或重写完整 `$Bitmap`。

### 关键元数据与提交前审计（SR7）

| 头文件 | 职责 |
| --- | --- |
| `ntfs_critical_relocation.h` | 关键文件（record 0..11）簇搬移、runlist/`$Bitmap`、`$LogFile` 失效、`$MFTMirr` 同步 |
| `ntfs_precommit_auditor.h` | Boot commit 前结构审计；唯一 pass/fail，warning 不替代失败 |

关键重定位不执行 Boot/`$Volume` BPB 提交（留给 SR8）。`$LogFile` restart 页清零以防旧 LSN replay。
Auditor 至少检查：新几何与 Target 容量、可达 run 不超界、runlist/Attribute List 合法性、`$Bitmap`
目标簇已分配、受保护 Boot escrow 可读、`$MFT`/`$MFTMirr` 首记录一致。最终 MFT、Bitmap 和 Mirror
审计只读真实 Target；Archive/Scratch 不能补齐遗漏的 Target 写入。

### Boot commit、CHKDSK 与 Finalize（SR8）

| 头文件 | 职责 |
| --- | --- |
| `ntfs_boot_commit.h` | 从 escrow Boot 打补丁 `TotalSectors`、`$MFT`/`$MFTMirr` LCN；Backup → flush/readback → Primary |
| `ntfs_chkdsk_runner.h` | 经 `IProcessLauncher` 跑 `chkdsk /x /f`；取消不 `terminate` |
| `ntfs_shrink_finalize.h` | 持锁 flush/readback/Boot commit 与关句柄后的 CHKDSK/postcheck 两阶段合同 |

结果语义：

- Primary 前失败 → `restore.shrink_target_incomplete`
- Primary readback 不确定 → `restore.shrink_commit_outcome_unknown`
- CHKDSK/复核失败 → `restore.shrink_postcheck_failed`
- exit 0/1/2 为成功候选；3/其它/强杀为失败或 unknown

`chkdsk.exe` 路径与卷 GUID 由 Worker 解析注入；脏卷/挂载复核经 `IShrinkVolumePostcheck`（Windows 实现在
Adapter/Worker，不进入 NtfsResize）。

### ShrinkPlan 字段（开发计划 §6）

`plan_version`、`source_chain_fingerprint`、`source_volume_index`、`source_logical_size_bytes`、
`source_boot_digest`、`source_ntfs_geometry`、`target_stable_id_digest`、`target_device_geometry`、
`target_capacity_bytes`、`new_total_sector_count`、`new_total_cluster_count`、`new_mft_start_lcn`、
`new_mft_mirror_start_lcn`、`minimum_target_bytes`、
`scratch_upper_bound_bytes`、`protected_ranges`、`relocation_records`、`metadata_mutations`、
`critical_file_operations`、`plan_payload_digest`（64 字符小写 hex SHA-256）。

生成后只读；通过 `ShrinkPlanBuilder::build()` 校验不变量并计算 digest。

### AGSP 二进制格式（v1，小端）

```text
Header (64 bytes)
  magic[4] = 'A','G','S','P'
  version u32 = 1
  header_size u32 = 64
  flags u32 = 0
  payload_crc32 u32          // CRC-32 (poly 0xEDB88320) over payload
  reserved u32 = 0
  payload_size u64
  payload_sha256[32]         // SHA-256 over payload
Payload
  section_count u32 = 5
  repeated:
    section_id u32           // 1 scalars, 2 protected, 3 relocations, 4 mutations, 5 critical
    section_size u32
    section_body[section_size]
```

解析时拒绝截断、未知版本、错误 CRC/digest、重复/缺失/未知 section、非法半开区间、重定位目标重叠、
以及 `cluster_count` 与源/目标 LCN 跨度不一致。

## 核心不变量

1. 容量三字段分离：`source_logical_size_bytes`、`target_capacity_bytes`、提交用的新 NTFS 大小字段不得混用。
2. `target_capacity_bytes` 必须等于 `target_device_geometry.capacity_bytes`。
3. `new_total_sector_count + 1` 必须等于 `target_capacity_bytes / bytes_per_sector`；多出的一个扇区承载
   Backup Boot。`minimum_target_bytes` 按 `new_total_cluster_count * cluster_size + sector_size` 计算。
4. 字节/簇范围为半开区间；重定位目标 LCN 范围互不重叠。
5. 计划不记录密码、密钥、token 或簇内容。
6. SHA-256 在本模块内自包含实现，不依赖 crypto adapter。

## 目录与 CMake

```text
src/ntfs_resize/
├── CMakeLists.txt
├── include/aegra/ntfs_resize/*.h
└── src/
    ├── shrink_plan_*.cpp / sha256.cpp
    ├── ntfs_volume_view.*
    ├── ntfs_mft_scanner.*
    ├── ntfs_bitmap_allocator.*
    ├── ntfs_relocation_plan.*
    ├── ntfs_shrink_audit.*
    └── ntfs_shrink_analyzer.cpp
```

根 `CMakeLists.txt` 在 `ntfs_core` 之后、adapters 之前注册本模块。

## 验证

- 构建 `aegra_ntfs_resize`（及依赖它的后续 Worker 装配）。
- 静态/架构检查：依赖仅 Base/Ports/NtfsCore；无 Windows/Qt/Archive 头；函数与文件规模符合工程规范。
- 不新增测试 Target；破坏性执行与 M01–M26 由 SR5–SR10 人工矩阵覆盖。

## Definition of Done

- Target 可独立构建；公共 ShrinkPlan/Codec API 与 AGSP v1 文档一致。
- Analyzer 可对合法较小 Target 产出可编解码 ShrinkPlan，并在写前拒绝不支持布局。
- 架构与模块索引已登记 `ntfs_resize`。
