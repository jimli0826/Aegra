# ADR-0009：Windows 在线卷恢复写入安全边界

- 状态：Accepted
- 日期：2026-08-02
- 决策者：Aegra 项目
- 关联模块：ports、pipeline、adapters/windows_disk、apps/worker

## 背景

个人版需要把完整 `.bkf` 恢复到已有 Windows Volume。块级恢复会不可逆覆盖文件系统数据；盘符可能
变化、目标可能仍被进程使用，系统卷在线写入还会破坏正在运行的操作系统。通用 Restore Pipeline 只依赖
`IBlockSink`，不应知道 Windows 锁卷、卸载和设备路径规则。

## 决策

1. `WindowsVolumeBlockSink` 只接受 canonical Volume GUID Path：
   `\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\`。盘符、PhysicalDrive、GLOBALROOT 和普通文件
   都不能进入生产卷模式。
2. 打开目标后必须成功执行 `FSCTL_LOCK_VOLUME` 和 `FSCTL_DISMOUNT_VOLUME`，否则不允许任何写入。
3. Adapter 通过 Windows 目录所在 Volume GUID 识别当前系统卷，并在打开写句柄前拒绝。系统卷恢复只
   能由后续 WinPE 离线恢复程序执行。
4. Adapter 在打开目标写句柄前解析恢复链中每个 Archive 所在 Volume；任一 Archive 位于目标 Volume
   或无法安全解析其所属 Volume 时拒绝，防止卸载目标后失去恢复源或覆盖唯一副本。
5. 容量来自 `IOCTL_DISK_GET_LENGTH_INFO`。Restore Pipeline 在首个写入前完成 Archive 结构、目标容量
   和内存预算预检。
6. 写入使用显式 offset、重叠 I/O 和取消；范围必须完全位于目标容量内。成功结束调用
   `FlushFileBuffers`，析构时 best-effort 解锁并关闭句柄。
7. Application 必须显式提供完整 base-first Archive 链，并在打开目标前解析全部凭据、认证所有层及验证
   UUID、备份集与卷几何关系。单个全量 Archive 是长度为 1 的合法链；稀疏增量层不能单独恢复。
8. Adapter 的普通文件 Sink 模式只用于确定性 Port 测试和未来离线镜像目标，拒绝所有 Windows Device
   Namespace 路径。

## 备选方案

- 接受盘符并自动解析：盘符是易变展示标识，容易选错卷，不采用。
- 锁卷失败后强制卸载或继续写：无法保证没有活动文件句柄，不采用。
- 在线恢复系统卷：运行中系统会持续写入且可能立即崩溃，不采用。
- 直接恢复单个增量层：空洞会保留目标旧数据，不能得到正确恢复点，不采用。

## 影响

- 调用方必须在 UI/Service 阶段显示目标身份并完成不可逆操作确认，再提交 canonical Volume GUID Job。
- 当前 Worker 适用于非系统数据卷；系统盘在线恢复仍禁止。
- 目标卷成功锁定后在 Sink 生命周期内对其它访问不可用。
- 整盘 disk→disk 恢复（Full 或 Incremental tip 的 base-first 链）使用独立的 `kPhysicalDisk` Sink
  与 `raw_layout` 重建路径：目标必须是
  非系统 `\\.\PhysicalDriveN`，写入前删除现有分区表，Archive 不得位于目标盘；系统盘 / 裸机 PE
  路径仍留给后续 WinPE 工作包。

## 验证

- 普通文件 Sink 测试覆盖 offset 写入、边界、取消、flush 和路径策略。
- 卷路径测试覆盖 canonical/非 canonical 形式与系统卷拒绝辅助逻辑。
- Worker Task 使用 Fake Backend 覆盖请求拒绝、凭据生命周期、成功指标、容量错误和错误脱敏。
- 真实非系统卷恢复只在显式管理员集成环境运行，不进入普通 CTest。
