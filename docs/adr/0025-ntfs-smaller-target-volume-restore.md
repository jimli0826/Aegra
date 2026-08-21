# ADR-0025：NTFS 小目标卷恢复边界

- 状态：Accepted
- 日期：2026-08-20
- 决策者：Aegra 项目
- 关联模块：ntfs_core、ntfs_resize、ports、pipeline、adapters/ntfs、adapters/windows_disk、
  adapters/storage_local、adapters/windows_process、contracts、application、apps/service、
  apps/worker、apps/desktop
- 依据：[开发计划](../development/NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md)；
  [ADR-0009](0009-windows-volume-restore-safety.md)；
  [技术验证记录](../development/NTFS_SHRINK_SR0_TECHNICAL_VERIFICATION.md)

## 背景

Personal Archive 的 NTFS volume 需要恢复到比源逻辑卷更小、但仍足以容纳全部已分配数据与 NTFS
元数据的目标卷。现有路径在 `target_capacity < source_logical_size` 时一律拒绝。若直接沿用旧仓库
缩容实现，会引入容量混用、线性 Overlay、固定数组、预检不完整、取消语义不清和 CHKDSK 可选等问题。

本 ADR 冻结首版架构、Ports、失败原子性、取消、Scratch、`$LogFile`/Dirty、Boot 提交与 CHKDSK 决策。
2026-08-21 的产品决策允许 Debug 与 Release Service 都宣告 `restore.ntfs_shrink.v1`，以便标准 Release
构建能够完成真实 Analyze/Start 验证。M01–M26 仍是对外发布资格门槛；在矩阵完成前，Release 构建只能用于
隔离、可销毁 VHD/VHDX 或明确接受数据覆盖风险的目标，不得描述为已通过发布验证。

## 决策

### 1. 模块与依赖

1. 新增 `Aegra::NtfsCore`（`src/ntfs_core`）：安全解析/编码 Boot、USA fixup、MFT record、属性、
   Attribute List、Runlist、`$Bitmap`。只允许依赖 `Base`、`Ports`。
2. 新增 `Aegra::NtfsResize`（`src/ntfs_resize`）：只读分析、不可变 ShrinkPlan、复合块设备、重定位、
   提交前审计与 Boot commit escrow。只允许依赖 `Base`、`Ports`、`NtfsCore`。
3. 现有 `Aegra::AdapterNtfs` 改为依赖 `NtfsCore`，继续只公开 Explorer 只读浏览合同；不得向 Shell
   Extension 暴露 write/mutate/commit 类型，也不得链接 `NtfsResize`。
4. `NtfsResize` 不依赖 Windows Disk、Personal Archive、Storage Local、Qt 或 Service。Windows 目标卷、
   Archive reader、Sparse Scratch 与 `IProcessLauncher` 只在 `apps/worker` Composition Root 装配。
5. Adapter 之间不得互相构造或引用具体实现。

### 2. 容量三字段分离

以下字段不得复用、别名化或由同一 `capacity()` 隐式表达：

| 字段 | 含义 |
| --- | --- |
| `source_logical_size_bytes` | Archive 合并后的源逻辑卷大小 |
| `target_capacity_bytes` | 目标设备真实可写容量 |
| `new_ntfs_volume_size_bytes` | 提交到 BPB 的新文件系统大小 |

目标 Sink/Block Device 永远报告真实 `target_capacity_bytes`。禁止构造“假容量” Sink 以绕过边界检查。

NTFS BPB `TotalSectors` 是末尾 Backup Boot Sector 的零基扇区位置，而不是原始设备的扇区个数。
源卷必须满足 `source_logical_size_bytes = TotalSectors * bytes_per_sector + bytes_per_sector`；目标提交必须
写入 `new_total_sector_count = target_capacity_bytes / bytes_per_sector - 1`，并把 Backup Boot 写在该扇区。
簇边界和 `$Bitmap` 仍以 `TotalSectors / sectors_per_cluster` 计算。该显式 `+1/-1` 关系用于避免
Actiphy 旧实现中把设备 Block count 直接写入 BPB、导致 Backup Boot 写到设备末端之外的现有问题。

### 3. Ports

1. 增加设备几何与随机读能力（`IRandomAccessBlockDevice` 或等价 view）。审查后若继承 `IBlockSink`
   会造成不必要耦合，则使用独立接口并由适配 view 组合；现有 `IBlockSink` 不得获得平台语义。
2. Scratch Port 至少表达：随机读写、逻辑大小、已分配物理字节、最大允许分配、flush、页面校验、
   close/discard。实现优先放在 `Aegra::AdapterStorageLocal` 的 Windows 路径；若后续职责过大，再以
   新 ADR 拆独立 Scratch Adapter。
3. Scratch 创建时必须证明路径不在 Target volume；硬配额超额立即失败。
4. Windows 本地文件与 Sparse Overlay 必须使用 Win32 API + RAII handle，禁止 iostream 文件流。

### 4. ShrinkPlan

ShrinkPlan 是分析与执行之间的不可变合同，只保存在 Worker Scratch，不进入 Personal Archive 或
Repository 持久化格式。必须含版本、几何、plan digest、protected ranges、relocation records、
metadata mutations、scratch upper bound。生成后只读；执行前重新比对 source chain、target ID、
capacity 与 geometry。不记录密码、密钥、token 或簇内容。

### 5. 两级 Preflight 与策略

1. 使用 `VolumeSizePolicy { kRequireSourceSize, kAllowNtfsRelocation }`，禁止布尔开关。
2. Service 快速筛选只返回 `provisional` 资格，不得用 Manifest free size 冒充精确资格。
3. Worker 精确预检是写入资格的权威来源：完整扫描后生成 ShrinkPlan，且不打开 Target 写句柄。
4. Desktop 确认 token 绑定 recovery point、chain fingerprint、source volume index、target stable ID/
   capacity/geometry、`VolumeSizePolicy`、ShrinkPlan digest 与过期时间。Start 时重新校验；变化返回
   `restore.shrink_plan_changed`。
5. Disk restore 与 `kRequireSourceSize` 路径继续要求 `target >= source`。

### 6. `$LogFile`、Dirty 与源卷资格

1. VSS 备份镜像可能携带 `$Volume` dirty 标志或未完全收敛的 `$LogFile` restart/LSN 状态。首版精确
   预检必须读取 `$Volume` 与 `$LogFile` restart area。
2. 若源镜像 dirty、restart area 损坏、存在需要 replay 才能安全挂载的未决日志，或无法证明离线改写后
   不会被旧日志破坏：在 Target 写入前拒绝，稳定码走 `restore.shrink_unsupported_layout`（或后续更
   细分码），对应人工矩阵 M12。
3. 离线重写 MFT/`$Bitmap`/data runs 之后，旧 `$LogFile` 不得带着可回放的旧 LSN 进入可挂载状态。
   提交前必须将 `$LogFile` restart 区域失效或重建为“无需 replay 的干净重启状态”，使 Windows 不会把
   旧日志应用到已迁移元数据。具体编解码落在 `NtfsCore`/`NtfsResize`；实现前以可丢弃 VHD 验证。
4. `FSCTL_MARK_VOLUME_DIRTY` **不作为**正确性证明，也不是成功路径的必要步骤。成功路径以受控
   `chkdsk.exe /x /f` 与卷状态复核为准。不得用“标 dirty 留待下次开机 autochk”替代本次恢复成功判定。

### 7. Boot 失效、锁卷与挂载暴露

1. 延续 ADR-0009：目标必须是 canonical Volume GUID Path；写入前成功 `FSCTL_LOCK_VOLUME` +
   `FSCTL_DISMOUNT_VOLUME`；拒绝系统卷、启动卷、Archive 所在卷与 BitLocker 目标。
2. 锁卷成功后首先使 Primary Boot Sector 与目标末扇区 Backup Boot 位置不可作为有效 NTFS Boot 使用，
   并 flush/readback。此后到 Primary Boot 提交前，目标必须 fail-closed，不可被当作完好 NTFS 卷暴露。
3. 前缀恢复通过 protected-range sink 跳过受保护扇区；禁止“写后再修复 Boot”。
4. 持锁生命周期由 RAII 管理；异常与取消路径不得泄漏 handle 或锁状态。
5. Boot 失效期间释放用户可见盘符/自动挂载不是正确性前提；正确性前提是 Boot 签名/几何无效且关键
   写入未提交。实现仍应在锁/dismount 下工作，避免 Mount Manager 将半成品当 NTFS 使用。
6. 完整重试打开已失效 Target 时，卷可能以 RAW 状态拒绝 `FSCTL_ALLOW_EXTENDED_DASD_IO`。仅当错误为
   `ERROR_INVALID_FUNCTION` / `ERROR_INVALID_PARAMETER`，且锁卷、卸载、容量/几何查询及设备末端完整
   对齐块读取均成功时，Adapter 可继续；不得对其它错误盲目降级。

### 8. 提交顺序与结果语义

```text
flush 数据/元数据
-> readback 关键结构
-> 写 Backup Boot -> flush/readback
-> 写 Primary Boot -> flush/readback   // NTFS 可被挂载的提交点
-> 关闭原始写句柄，保持不对用户提前宣告成功
-> IProcessLauncher 启动 GetSystemDirectoryW\chkdsk.exe /x /f <volume-guid>
-> 等待 CHKDSK 正常退出（取消不得 terminate）
-> 复核 dirty/mount/几何
-> Completed
```

| 阶段 | 结果 |
| --- | --- |
| 分析完成前 / Boot invalidation 前 | 普通取消或失败；Target 未破坏性修改 |
| Boot invalidation 后、Primary commit 前 | `restore.shrink_target_incomplete`；必须完整重试 |
| Primary Boot 写入无法 readback 确认 | `restore.shrink_commit_outcome_unknown` |
| CHKDSK 或最终状态复核失败 | `restore.shrink_postcheck_failed`；不得报告成功 |

取消不得以未经验证的反向 MFT 修改伪造回滚。Boot commit 与 CHKDSK 阶段延迟取消，必须先达到可判断
稳定状态。Worker 线程由 `std::jthread`、CancellationToken、原子状态或有界队列协调，退出前 join。

### 9. CHKDSK 映射

通过 `GetSystemDirectoryW` 解析 `chkdsk.exe`，由 `IProcessLauncher` 启动；参数为 `/x` `/f` 与
canonical volume GUID（或等价 volume name）。Exit code 映射：

| Exit code | 含义 | 恢复结果 |
| ---: | --- | --- |
| 0 | 未发现错误 | 允许继续卷状态复核 |
| 1 | 发现并已修复错误 | 允许继续复核；任务日志记录已修复 |
| 2 | 执行了清理，或因未指定 `/f` 未清理 | 本产品始终传 `/x`（含 `/f`）。若仍返回 2，按成功候选继续复核，并记录警告 |
| 3 | 无法检查或无法修复 | `restore.shrink_postcheck_failed` |
| 其它 / 进程异常终止 | 不可判定或失败 | `restore.shrink_postcheck_failed`；若终止原因使卷状态不可读，可升级为 outcome unknown |

取消请求在 CHKDSK 运行期间不得调用 `terminate`。`IProcessLauncher::wait` 在取消时只返回
`kCancelled` 且不杀进程；恢复路径必须忽略该取消并继续等待 CHKDSK 结束，再返回稳定结果。

### 10. Pipeline 与 Capability

1. `RestorePlan` 增加显式 `logical_write_limit_bytes`；默认完整恢复。只有缩容策略可设为目标容量。
2. Pipeline 仍完整验证 Archive descriptor；对跨越写上限的最后 chunk 精确裁剪；不负责 NTFS 重定位。
3. Debug 与 Release Service 都宣告 `restore.ntfs_shrink.v1`。两种配置都必须执行精确 Analyze、eligible
   token、Start TOCTOU 重验证和 fail-closed Worker 状态机，不允许使用配置分支绕过安全检查。M01–M26
   完成前，该能力仍属于未取得对外发布资格的实验功能。
4. 不新增旧仓库兼容逻辑、协议 fallback、字段 alias、自动化测试代码或测试专用 executable。

## 备选方案

- 创建完整源大小 VHD 再调用 Windows 在线缩容：额外磁盘、时间与在线缩容限制不可接受，不采用。
- 边恢复边发现不支持布局：破坏性写入后才拒绝，违反 fail-closed，不采用。
- 继承 `IBlockSink` 并让其报告源容量：重复旧仓库容量谎言，不采用。
- 用 `FSCTL_MARK_VOLUME_DIRTY` + 重启 autochk 代替本次 CHKDSK：成功判定推迟且不可控，不采用。
- 中断后 MFT 级断点续作：首版状态空间过大，不采用；一律完整重试。

## 影响

- 直接恢复（目标不小于源）合同、性能与失败语义保持不变。
- Service/Desktop/Worker/协议/控制面 schema 需增加策略、可行性状态与 token 绑定字段；产品未发布，
  直接升级 current schema，不写 migration/fallback。
- 架构文档与模块文档必须登记 `NtfsCore`/`NtfsResize` 与新 Ports。
- 人工验证必须使用可丢弃 VHD/VHDX；不得接触生产数据。

## 验证

- Debug/Release 生产 Target 构建与 source-limit / 依赖边界检查。
- SR0 技术验证记录覆盖 `$LogFile`/Dirty 决策、锁卷顺序、Boot 失效暴露、CHKDSK 路径与 exit mapping。
- 对外发布前完成开发计划 M01–M26。Debug/Release capability 可用于验证，但 capability 可见不等于
  发布门禁通过。
