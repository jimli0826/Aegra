# NTFS 小目标卷恢复 — SR0 技术验证记录

| 属性 | 内容 |
| --- | --- |
| 状态 | SR0 决策已冻结；破坏性 VHD 矩阵留待提权实验机补录 |
| 日期 | 2026-08-20 |
| 对应 ADR | [ADR-0025](../adr/0025-ntfs-smaller-target-volume-restore.md) |
| 开发计划 | [NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md](NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md) |

## 1. 验证环境

| 项 | 值 |
| --- | --- |
| 主机 | Windows（非管理员交互会话） |
| `GetSystemDirectoryW` 等价路径 | `C:\WINDOWS\system32` |
| `chkdsk.exe` | 存在：`C:\WINDOWS\system32\chkdsk.exe`（53248 bytes） |
| Hyper-V / diskpart VHD | 本会话无管理员权限；`New-VHD` 不可用；`diskpart` 创建 VHD 被系统拒绝 |
| 结论 | 路径级与文档级验证可完成；锁卷/Boot 失效/CHKDSK 破坏性场景必须在可丢弃 VHD 的提权环境补录，但不阻塞 ADR 完整性决策 |

## 2. `$Volume` Dirty 与 `$LogFile`

### 2.1 已知语义（文档与既有实现约束）

1. NTFS `$Volume` 可携带 dirty 状态；Windows 在 dirty 卷上倾向要求 CHKDSK / autochk。
2. `$LogFile` 开头包含双份 `RSTR` restart page；挂载时可能按 LSN replay 日志。
3. 离线改写 MFT/`$Bitmap`/data runs 后，若保留可回放的旧 restart/LSN，Windows 可能把旧日志应用到新元数据，造成静默破坏。
4. `FSCTL_MARK_VOLUME_DIRTY` 只安排下次重启 autochk，**不能**证明当前离线改写正确（见 Microsoft IFS 文档）。
5. 现有备份路径使用 VSS；快照一致性足以做块级整卷恢复，但**不足以**自动证明缩容离线改写可安全 replay 旧日志。

### 2.2 ADR 冻结结论（不再推迟）

| 问题 | 决定 |
| --- | --- |
| 源镜像 dirty / 日志需 replay / restart 损坏 | 精确预检写前拒绝（矩阵 M12） |
| 离线改写后旧 `$LogFile` | 提交前失效或重建为无需 replay 的干净重启状态 |
| `FSCTL_MARK_VOLUME_DIRTY` | 不用于成功路径，不作正确性证明 |
| 成功门槛 | 受控 `chkdsk /x /f` + 卷状态复核 |

### 2.3 提权补录项（不改变上述决策）

在可丢弃 VHD 上记录：

1. 干净 NTFS 卷的 `$Volume` flags 与 `$LogFile` restart magic/LSN 基线；
2. 人为制造 dirty 后同字段差异；
3. 失效 restart 后挂载是否跳过 replay；
4. 故意保留旧 restart + 改 MFT 是否触发损坏（反例）。

## 3. 锁卷、Dismount、Flush、重新打开

### 3.1 代码与 ADR-0009 既证

`WindowsBlockSink`（`src/adapters/windows_disk/src/windows_block_sink.cpp`）生产卷模式已强制：

1. canonical `\\?\Volume{GUID}\`；
2. `FSCTL_LOCK_VOLUME` 成功，否则拒绝写入（含 sharing violation 稳定消息）；
3. `FSCTL_DISMOUNT_VOLUME` 成功；
4. 容量来自 `IOCTL_DISK_GET_LENGTH_INFO`；
5. 成功路径 `FlushFileBuffers`；析构 best-effort 解锁。

### 3.2 缩容路径补充顺序（ADR-0025）

```text
open GUID volume (R/W)
-> LOCK + DISMOUNT
-> invalidate Primary + Backup Boot; flush + readback
-> prefix restore / relocate / audit（持锁）
-> commit Backup Boot; flush + readback
-> commit Primary Boot; flush + readback
-> close write handle
-> chkdsk /x /f
-> reopen for status query only
```

### 3.3 提权补录项

对可丢弃卷验证：LOCK 失败拒绝、DISMOUNT 后其它句柄失效、unlock/close 后可重新打开、异常析构无锁泄漏。

## 4. Boot 失效与 Mount Manager 暴露

### 4.1 决策依据

1. NTFS 识别依赖有效 Boot OEM/BPB；Primary Boot 无效时 Windows 不应把它当完好 NTFS 文件系统使用。
2. 缩容在 invalidation 后、Primary commit 前必须 fail-closed；即使用户看到 RAW/未分配提示，也不得当成恢复成功。
3. 持锁 + dismount 降低 Mount Manager 自动重挂载风险；正确性仍以 Boot 无效与未提交为准。

### 4.2 提权补录项

1. 清零/破坏 OEM ID 后资源管理器与 `GetVolumeInformation` 行为；
2. 仅写 Backup Boot、Primary 仍无效时卷仍不可当 NTFS 使用；
3. Primary commit + flush/readback 后可挂载。

## 5. CHKDSK 生产路径

### 5.1 本机已验证

| 检查 | 结果 |
| --- | --- |
| `GetSystemDirectoryW` → `%SystemRoot%\system32` | `C:\WINDOWS\system32` |
| `chkdsk.exe` 存在 | 是 |
| 官方 exit code | 0 无错误；1 已修复；2 清理相关；3 失败/未修复 |

### 5.2 `IProcessLauncher` 合同（代码审查）

`ports/process_launcher.h`：

- `wait` 在取消时返回 `kCancelled`，**不**自动 `terminate`；
- 调用方决定是否强杀。

缩容路径要求：CHKDSK 期间忽略普通取消的 terminate，必须等到进程退出再映射结果。

### 5.3 启动参数（冻结）

```text
<SystemDirectory>\chkdsk.exe /x /f <VolumeGuidPathOrName>
```

- `/x` 含 `/f` 并强制 dismount；
- 使用 Volume GUID/volume name，避免盘符竞态；
- Exit mapping 见 ADR-0025 §9。

### 5.4 提权补录项

在可丢弃 NTFS VHD 上实际跑通 exit 0/1/3 与“取消不杀进程”。

## 6. Outcome 语义总表（人工验收用）

| 场景 | 期望稳定码 / 结果 |
| --- | --- |
| 精确分析前取消 | 普通取消；Target 未改 |
| Boot invalidation 后失败/取消 | `restore.shrink_target_incomplete` |
| Primary Boot readback 不确定 | `restore.shrink_commit_outcome_unknown` |
| CHKDSK exit 3 / 异常终止 | `restore.shrink_postcheck_failed` |
| CHKDSK 0/1 且复核干净 | 允许 `Completed` |
| 源 dirty/需 log replay | 写前拒绝（不进入破坏性阶段） |

## 7. SR0 DoD 判定

| 项 | 状态 |
| --- | --- |
| ADR-0025 已写并 Accepted | 完成 |
| 日志/Dirty/挂载/CHKDSK 成功失败/unknown 语义已定义 | 完成 |
| 未决数据完整性问题是否推迟到实现 | **否** — 拒绝策略、`$LogFile` 失效、CHKDSK 硬门槛、Dirty FSCTL 不用作证明均已冻结 |
| 破坏性 VHD 实测矩阵 | 阻塞于本机管理员权限；列为 SR8/SR10 提权补录，不反向修改上述完整性决策 |

## 8. 交接

```text
工作包：SR0
状态：已完成（决策冻结）；提权 VHD 补录挂到 SR8/SR10 人工矩阵
基线提交：<pending>
修改文件：docs/adr/0025-...、docs/development/NTFS_SHRINK_SR0_...、docs/adr/README.md、开发计划状态
生产 Targets：无（文档门禁）
静态/边界检查：不适用
人工验证：chkdsk 路径已核；锁卷/Boot/CHKDSK 破坏性项待提权 VHD
稳定码/协议/文档：ADR-0025 Accepted
未决风险：无完整性决策缺口；仅缺提权环境实测记录
下一工作包：SR1
```
