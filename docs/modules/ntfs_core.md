# `ntfs_core` 模块开发文档

## 目标

提供与平台无关的 NTFS 安全解析与编码：Boot、USA fixup、MFT record、属性、Attribute List、
Runlist、`$Bitmap` 位图辅助，以及按 run 读取逻辑字节。供 `Aegra::AdapterNtfs`（只读 Explorer）与
后续 `Aegra::NtfsResize` 共用。

## 非目标

- 不实现缩容分析、ShrinkPlan、重定位或 Boot commit（见 `NtfsResize` / ADR-0025）。
- 不暴露 Explorer DTO（`NtfsEntry`、continuation token、目录分页）。
- 不依赖 Windows、Qt、Archive、Service 或其它 Adapter。

## 依赖

| 允许 | 禁止 |
| --- | --- |
| `Aegra::Base`、`Aegra::Ports` | Windows SDK、Qt、其它 Adapter、`NtfsResize` |

Target：`aegra_ntfs_core` / `Aegra::NtfsCore`。路径：`src/ntfs_core/`。

## 公共接口

公共头位于 `include/aegra/ntfs_core/`：

| 头文件 | 职责 |
| --- | --- |
| `types.h` | 语义字段类型（VCN/LCN/ClusterCount/ByteOffset）、BootGeometry、DataRun、AttributeValue、ParsedMftRecord、AttributeListEntry、属性常量 |
| `binary.h` | 溢出检查算术、LE 读写、file reference 打包 |
| `fixup.h` | USA fixup：`apply_fixup`（读路径还原）、`seal_fixup`（写路径盖戳） |
| `boot_sector.h` | Boot Sector → `BootGeometry`（含 `$MFT`/`$MFTMirr` LCN）；`patch_boot_geometry`（缩容提交） |
| `runlist.h` | Runlist 解析；`validate_data_runs`；先 `measure_runlist_encoded_size` 再 `encode_runlist` / `encode_runlist_bounded` |
| `attribute.h` | 属性头校验与属性解析 |
| `mft_record.h` | MFT record 解析（含 fixup） |
| `attribute_list.h` | Attribute List 纯解析与条目校验 |
| `bitmap.h` | `$Bitmap` 覆盖范围与按位查询 |
| `layout_read.h` | 经 `IRandomAccessReader` 按属性/run 读取逻辑字节 |

线程：编解码函数无内部共享可变状态；调用方负责缓冲生命周期。错误：可预期失败返回 `Result` 与稳定
`ntfs.*` message_code。取消：仅 `read_from_attribute` 观察 `CancellationToken`。

## 核心不变量

1. 所有 offset/size/cluster 算术经 checked helper；拒绝截断、越界与溢出 run。
2. Runlist 解析要求显式 terminator；编码前验证 VCN 连续性、非零长度、LCN delta 的 signed width、terminator 与有界缓冲；
   不使用固定 1024/4096 临时编码数组。
3. 语义量使用命名类型/字段（`VirtualClusterNumber`、`LogicalClusterNumber`、`ByteOffset` 等），
   不与裸用途混用同一字段名表达多种含义。
4. 未知/损坏输入 fail closed；不引入旧仓库兼容分支。
5. NTFS BPB `TotalSectors` 表示末尾 Backup Boot Sector 的零基扇区位置；因此
   `BootGeometry::volume_size_bytes = TotalSectors * bytes_per_sector`，而承载该 NTFS 的原始设备大小为
   `volume_size_bytes + bytes_per_sector`。簇数仍按 `TotalSectors / sectors_per_cluster` 计算。

## 目录与 CMake

```text
src/ntfs_core/
├── CMakeLists.txt
├── include/aegra/ntfs_core/*.h
└── src/*.cpp
```

根 `CMakeLists.txt` 在 adapters 之前注册 `src/ntfs_core`（非 Windows-only）。

## 验证

- 构建 `aegra_ntfs_core` 与依赖它的 `aegra_adapter_ntfs`。
- 静态/架构检查：无 Windows/Qt/Archive 头；函数与文件规模符合工程规范。
- Explorer 只读路径行为由 Adapter 人工验证；本模块不新增测试 Target。

## Definition of Done

- Target 可独立构建；依赖仅 Base/Ports。
- AdapterNtfs Explorer API 保持只读且不重导出 mutate/encode 合同。
- 动态 Runlist 编码器可在写入前判断编码长度是否放入目标缓冲。
