# Windows 个人版卷 / 整盘恢复

## 目标与非目标

### 卷恢复（已有）

把显式、完整的个人版 `.bkf` 恢复链还原到一个已存在的非系统 Windows Volume。它不创建分区、
不修改分区表、不恢复系统卷、不自动选择目标，也不扫描目录猜测恢复链。

### 整盘恢复（Full 与 Incremental tip）

把**完整 base-first 链**（Full，以及可选的后续 Incremental 层）中某个源磁盘上的全部已备份
Volume，按 extent 物理偏移写入目标 `\\.\PhysicalDriveN`，并用 tip Archive Manifest 中的
`raw_layout` 重建分区表。

本阶段**不支持**：

- 在线系统盘目标（需 PE）；
- 跨盘卷 / 多 extent 卷；
- 自动盘符分配（后续可加）；
- Differential tip（Worker 链 Reader 仅接受 Full + Incremental）。

Service / Desktop 控制面：PrepareRestore（kind 9）+ StartRestore（kind 40）支持非系统盘
`disk.N` 目标；tip 可为 Full 或 Incremental。Desktop Restore 页提供源→目标映射、
**Preserve disk signature**、**Auto expand last partition** 与启动。

## 依赖边界

`apps/worker` Composition Root 可以依赖 `PersonalArchiveChainReader`、
`PersonalArchiveVolumeReader`、`WindowsBlockSink`（Volume / PhysicalDisk）、整盘
prepare/rebuild/online API 和通用 `RestorePipeline`。Pipeline 只依赖 `IRecoveryPointReader` 与
`IBlockSink`。

## 卷恢复流程

```text
Validate Restore Job and trusted chain-depth limit
-> Resolve every SecretRef while retaining all Secret lifetimes
-> Open PersonalArchiveChainReader (Full base + Incremental overlays)
-> PersonalArchiveVolumeReader(tip Manifest, source_volume_index)
   (rewrites descriptors to source_index=0 for RestorePipeline)
-> Open canonical target Volume GUID path (WindowsVolumeBlockSink)
-> Reject system volume
-> Reject any chain Archive located on target volume
-> Lock and dismount target volume
-> Preflight descriptors, capacity and memory budget
-> Read/authenticate/decompress each Chunk for that volume only
   (persistent Archive part inputs + one-Chunk sequential base-layer payload prefetch)
-> Write DATA/ZERO by logical offset using one overlapped request at a time;
   skip authenticated FREE ranges without target writes
-> Flush target
-> Unlock and close target
```

Worker 数据面（Phase A）、Service Prepare/Start（Phase B）与 Desktop 卷映射 UI
（Phase C）均已支持卷路径：`target_source_id=vol.…` + `source_volume_index`，
指纹 `volc|…`，Start 解析 Inventory `stable_key` 为 Volume GUID Path。

Desktop Restore 页提供 **Disk / Volume** 模式切换：
- Disk：源磁盘 → 目标 `disk.N`（Preserve signature / Auto expand 可用）；
- Volume：源 Manifest 卷 → 目标 Inventory `vol.*`（拖放或 “Restore to”；多卷可排队
  逐个 StartRestore）。

## 整盘恢复流程

```text
Validate restore.disk_restore + base-first source_refs + PhysicalDrive target
-> Resolve credentials (one SecretRef per layer; empty = unencrypted)
-> Open PersonalArchiveChainReader (Full base + Incremental overlays)
-> Locate Manifest.disks[source_disk_number] on tip and require raw_layout
-> Plan volumes with extents on that disk (single extent, same disk only)
-> Reject system target disk; reject any chain Archive living on target disk
-> prepare_target_disk_for_raw_restore (delete layout, capacity/sector check)
-> Open PhysicalDrive BlockSink
-> For each volume (offset order):
     Volume-scoped reader over the chain view (source_index rewritten to 0)
     -> OffsetBlockSink(physical_offset)
     -> RestorePipeline (FREE ranges skipped, no disk write)
-> Flush disk sink and close handle
-> apply_disk_signature_policy (preserve=true keeps source MBR/GPT DiskId; false randomizes)
-> rebuild_partition_table_from_raw_layout
-> Optional bring_target_disk_online
-> Optional expand_last_data_partition_on_disk (target larger + NTFS/ReFS only)
```

Service Prepare/Start：

```text
// 整盘 disk.N
resolve_chain(tip) → base-first Catalog entries
-> Open tip with password for disk size / capacity check
-> Preflight fingerprint diskc|… binds source_disk, size, and every layer key/uuid
-> Start re-resolves chain, rejects if keys/uuids/depth/size changed
-> Job: disk_restore=true, target_ref=\\.\PhysicalDriveN

// 卷 vol.…
resolve_chain(tip) → base-first Catalog entries
-> Open tip for volume total_size at source_volume_index
-> Preflight fingerprint volc|… binds volume_index, size, and every layer key/uuid
-> Start re-resolves chain; resolve_source → stable_key (Volume GUID Path)
-> Job: disk_restore=false, source_volume_index, target_ref=GUID path
```

对齐旧项目 `RestoreEngine` 全盘路径：先清布局写 raw 数据，再写 MBR/GPT，联机，再按需扩容。
增量数据面复用 `PersonalArchiveChainReader`（ADR-0004）。

## Job 与结果

### 卷恢复

- `operation = kRestore`；
- `source_refs` 按 base-first 顺序包含完整 `.bkf` 链（至少 1 个 Full；tip 可为 Incremental）；
- `target_ref` 是 canonical Volume GUID Path：
  `\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\`（UTF-8）；
- `restore` 推荐显式给出（Service 贯通后必填）：

```json
{
  "disk_restore": false,
  "source_disk_number": 0,
  "source_volume_index": 0,
  "bring_target_online": true,
  "preserve_disk_signature": true,
  "auto_expand_last_partition": true
}
```

- `source_volume_index`：tip Manifest `volumes[].volume_index`；省略 `restore` 时 Worker 按 `0` 处理。
- `bring_target_online` / `preserve_disk_signature` / `auto_expand_last_partition` 在卷模式由
  Worker **忽略**（仅整盘路径使用）。
- 多卷 Archive 必须用 `PersonalArchiveVolumeReader` 限定单卷；不得把整链多 `source_index`
  直接交给 `RestorePipeline`。

### 整盘恢复

- `operation = kRestore`；
- `source_refs` 为 base-first 完整链（至少 1 个 Full；tip 可为 Incremental）；
- `target_ref` 为 `\\.\PhysicalDrive{N}`（UTF-8）；
- `restore` 对象必填：

```json
{
  "disk_restore": true,
  "source_disk_number": 0,
  "bring_target_online": true,
  "preserve_disk_signature": true,
  "auto_expand_last_partition": true
}
```

- `preserve_disk_signature`：默认 true，保留源盘 MBR signature / GPT DiskId；false 时随机化（克隆数据盘且源盘仍在线时取消勾选）。
- `auto_expand_last_partition`：默认 true，目标大于源时扩展最后一个数据分区并在线扩展 NTFS/ReFS；FAT/FAT32/exFAT 不扩（剩余空间保持未分配）。

- `credential_refs` 与 `source_refs` 一一对应：加密归档为 DPAPI SecretRef；未加密为
  **空** `SecretRef`（`value` 为空表示无密码，合法，不得被 Job 校验拒绝）。Desktop 当前对整条链
  使用同一 Archive 口令；
- 成功 `restore.completed`；稳定 `restore.*` message code；
- `logical_bytes` / `stored_bytes` 为各卷完成处理的逻辑字节之和，`chunk_count` 为完成处理的 Chunk 总数。
- Worker 日志额外记录 `disk_written_bytes`、`free_skipped_bytes` 和 `free_ranges`，用于区分实际写盘量
  与直接跳过的 FREE 区域；`restored_bytes = disk_written_bytes + free_skipped_bytes`。

## 不变量与失败语义

- 打开目标写句柄前完成凭据解析与整条链 Archive 认证；
- 整盘路径要求链以 Full 为基、UUID/set/几何合法，且 tip Manifest 含可用 `raw_layout`
  （需备份采集；旧 Archive 无 raw_layout 时拒绝并提示重新备份）；
- Catalog 断链 / 环 / 超深度在 Prepare 与 Start 重验证时拒绝；
- 在线禁止写系统卷 / 系统物理盘；
- 链上任一 Archive 不得位于目标卷或目标物理盘；
- 写入开始后取消或 I/O 失败会留下部分恢复的目标；
- 整盘成功以分区表重建完成（以及可选 online）为界；数据 flush 在关 Sink 前完成；
- Worker 结果不输出口令、SecretRef 或底层 Win32 文本。

## 验证与完成标准

- 审查 Volume Sink 与 PhysicalDisk Sink 的安全边界；
- 审查断链、raw_layout 缺失、容量不足、系统盘、源在目标盘等拒绝路径；
- 使用隔离的非生产 VHD/测试盘人工验证 Full 与 Incremental tip 整盘还原后分区可读且数据为 tip 时刻；
- Debug/Release、源码规模、依赖检查通过。
