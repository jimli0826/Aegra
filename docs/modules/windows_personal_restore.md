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
**Preserve disk signature**、Target 分区条边缘拖动调整大小，与启动。无 Auto expand
选项：多余目标容量默认为未分配，由用户在 Target 上先拖边调整（Desktop 固定传
`auto_expand_last_partition=false`）。

### NTFS 小目标卷恢复（ADR-0025 / SR9）

当目标卷小于源卷且 Manifest 为 NTFS 时：

1. PrepareRestore（policy=`allow_ntfs_relocation`）只做 Service 快速筛选，返回
   `feasibility=provisional`（不可 Start）；
2. AnalyzeNtfsShrink（kind 18）在 Service 内同步调用 `NtfsShrinkAnalyzer`，签发新的
   `feasibility=eligible` token（含 `shrink_plan_digest` / 精确容量与 Scratch 上界）；分析器以簇为单位
   搜索确定性规划器可接受的最小边界。Target 低于该边界时返回带 `minimum_target_bytes` 的 provisional，
   不签发可执行计划；
3. StartRestore 仅接受 eligible；缩容 Job 携带 `volume_size_policy`、`shrink_plan_digest`、
   `source_chain_fingerprint`；
4. Worker 在 `kAllowNtfsRelocation` + 非空 digest 时进入 shrink 状态机（重分析校验 digest →
   Boot 失效 → 前缀恢复 → 复合设备重定位 → Target-only 审计 → Boot commit → 关闭锁卷句柄 →
   CHKDSK → 卷后检）；
5. Debug 与 Release Service 均宣告 `restore.ntfs_shrink.v1`；两种配置使用完全相同的 Analyze/Start 安全
   路径。M01–M26 完成前 capability 仅供受控验证，不代表发布资格。Disk restore 仍拒绝小盘。

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
- Disk：源磁盘 → 目标 `disk.N`（Preserve signature；Target 条拖边调整大小）；
- Volume：源 Manifest 卷 → 目标 Inventory `vol.*`。更小 Target 的拖放先执行 target-bound 精确 Analyze，
  只有 eligible 且容量满足 `minimum_target_bytes` 才建立映射；Summary 复用该 token 做确认与 Start。

## 整盘恢复流程

```text
Validate restore options required + disk_restore + base-first source_refs + PhysicalDrive target
-> Resolve credentials (one SecretRef per layer; empty = unencrypted)
-> Open PersonalArchiveChainReader (Full base + Incremental overlays)
-> Locate Manifest.disks[source_disk_number] on tip and require raw_layout
-> Plan volumes with extents on that disk (single extent, same disk only)
-> Phase 1 (reversible; failures leave target layout intact):
     validate_target_disk_for_raw_restore (system / capacity / sector; read-only)
     Open PhysicalDrive BlockSink briefly → reject Archive on target disk; read capacity
     prepare_target_partition_table (in memory only):
       apply_disk_signature_policy
       remove_unbacked_basic_data_partitions (keep EFI/MSR/Recovery + backed data)
       validate_raw_disk_layout_for_restore (even with empty edits):
         GPT: MBR+primary/backup headers+entry arrays; EFI PART; header/entry CRC;
              primary↔backup geometry; final intervals
         MBR: 0x55AA; reject extended/logical 0x05/0x0F; intervals/capacity
       resolve + apply partition_layout_edits (UI hints; always re-validate intervals;
         rewrite primary/backup GPT header LBAs for target size)
-> Phase 2 (irreversible):
     set_target_disk_offline fail-closed (verify DISK_ATTRIBUTE_OFFLINE)
     delete_target_disk_drive_layout
     rebuild_partition_table_from_raw_layout (complete GPT required; no partial write)
     Remap volume offsets to *resolved* starts
     Open PhysicalDrive BlockSink → RestorePipeline per volume (FREE skipped)
     Flush / close
     If bring_target_online:
       bring_target_disk_online (fail if OFFLINE remains)
       extend_filesystem_to_partition for each *resolved* layout edit (NTFS/ReFS)
         or auto_expand_last when no layout edits
     Else: leave offline; skip FS extend / auto_expand (no mounted volumes)
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
- `restore` 对象**必填**（contracts / Worker 拒绝缺失；无“省略即卷 0”回退）：

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

- `source_volume_index`：tip Manifest `volumes[].volume_index`（必须由 `restore` 显式给出）。
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
  "auto_expand_last_partition": false
}
```

- `preserve_disk_signature`：默认 true，保留源盘 MBR signature / GPT DiskId；false 时随机化（克隆数据盘且源盘仍在线时取消勾选）。
- `bring_target_online=false`：分区表与卷数据写完后保持离线；**不**执行
  `extend_filesystem_to_partition` / `auto_expand_last_partition`（二者依赖已挂载卷）。
  分区表几何仍按 layout edits 写好；文件系统填充分区需稍后联机再扩展。
- `auto_expand_last_partition`：协议字段仍存在；Desktop 磁盘恢复固定传 false。
- `partition_layout_edits`：`[{source_start_offset_bytes, target_start_offset_bytes, size_bytes}, …]`，
  来自 Target 条左右拖边，**仅作布局意图参考**。Desktop 预览会保留固定 reserved 段
  （EFI/MSR/Recovery）以免把系统分区空间当成 Unallocated。Worker 自行
  `resolve_partition_layout_edits`：保留系统分区不动，按数据分区源顺序把 UI 期望
  尺寸/起点钳制到空闲区间，再建表、按解析后的 `target_start` 写卷并扩展文件系统。
  未编辑则保持源数据分区起止（仍会重写 GPT 主/备头几何以匹配目标盘容量）。

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
- Desktop Restore 拖放/下拉映射在选择目标时即拒绝系统盘/系统卷，以及默认
  Repository locator 所在物理盘/卷，并在拖放 ghost 上显示原因（Worker 仍做最终校验）；
- 整盘恢复在第一个不可逆磁盘操作（offline / delete layout）之前完成源保护与
  最终布局校验；用户输入或 Archive 结构错误必须在此之前被拒绝；
- 当前版本拒绝含扩展/逻辑分区（MBR 0x05/0x0F）的整盘恢复（不持久化 EBR 链）；
- 写入开始后取消或 I/O 失败会留下部分恢复的目标；
- 整盘成功以分区表重建完成（以及可选 online）为界；数据 flush 在关 Sink 前完成；
- Worker 结果不输出口令、SecretRef 或底层 Win32 文本。

## 验证与完成标准

- 审查 Volume Sink 与 PhysicalDisk Sink 的安全边界；
- 审查断链、raw_layout 缺失、容量不足、系统盘、源在目标盘等拒绝路径；
- 使用隔离的非生产 VHD/测试盘人工验证 Full 与 Incremental tip 整盘还原后分区可读且数据为 tip 时刻；
- Debug/Release、源码规模、依赖检查通过。
