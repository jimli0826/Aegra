# NTFS 小目标卷恢复 — SR10 人工验证矩阵

| 属性 | 内容 |
| --- | --- |
| 状态 | 进行中：构建/静态审计先行；破坏性项待提权隔离 VHD |
| 日期 | 2026-08-21 |
| 对应 ADR | [ADR-0025](../adr/0025-ntfs-smaller-target-volume-restore.md) |
| 开发计划 | [NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md](NTFS_SMALLER_TARGET_VOLUME_RESTORE_DEVELOPMENT_PLAN.md) §19.3 |
| Capability | Debug/Release 均按 2026-08-21 产品决策宣告 `restore.ntfs_shrink.v1`；门禁 9 仍未满足 |

## 1. 记录要求

每项破坏性场景必须使用可丢弃、无生产数据的 VHD/VHDX，并记录：

- plan digest（`shrink_plan_digest`）
- source / target 几何与容量
- Worker/Service 状态日志路径与关键 stage
- CHKDSK exit code
- 最终 dirty / 可挂载状态
- 代表性文件哈希（恢复前后）

非破坏性项可在无 VHD 环境下用代码路径审查或现有直接恢复回归记录。

## 2. 矩阵状态

| ID | 场景 | 期望 | 状态 | 记录 |
| --- | --- | --- | --- | --- |
| M01 | Target 大于 Source | 走现有 direct restore，不进入 shrink | 待执行 | 需 Archive + 目标卷；非缩容路径回归 |
| M02 | Target 等于 Source | 走现有 direct restore | 待执行 | 同上 |
| M03 | 更小 Target，普通数据尾部碎片 | 成功迁移、CHKDSK 成功、哈希一致 | 阻塞：提权 VHD | |
| M04 | 已用空间接近 Target 极限 | 精确最小值并成功或写前拒绝 | 阻塞：提权 VHD | |
| M05 | Target 比最小值少一个 cluster | 精确预检拒绝，Target 零写入 | 阻塞：提权 VHD | Analyze 写前拒绝可先验证 |
| M06 | 大量 MFT / Attribute List | 完整处理或写前稳定拒绝 | 阻塞：提权 VHD | |
| M07 | `$MFT` 超过新边界 | 关键元数据迁移成功 | 阻塞：提权 VHD | |
| M08 | `$MFTMirr`/`$Bitmap` 超界 | 关键元数据迁移成功 | 阻塞：提权 VHD | |
| M09 | 稀疏文件尾部 run | 不为 sparse hole 分配簇 | 阻塞：提权 VHD | |
| M10 | compressed tail extent | 写前拒绝 | 阻塞：提权 VHD | |
| M11 | encrypted tail extent | 写前拒绝 | 阻塞：提权 VHD | |
| M12 | Source dirty / log replay | 写前拒绝或受控处理 | 部分：SR0 决策已冻结；破坏性补录待提权 | 见 SR0 验证文档 |
| M13 | 512/512e/4Kn 几何不匹配 | 写前拒绝 | 阻塞：提权 VHD | |
| M14 | 系统/启动/Archive 所在卷 | 写前拒绝 | 代码路径已有；人工确认待 | Service/Worker 拒绝逻辑已接线 |
| M15 | Scratch 位于 Target | 写前拒绝 | 代码路径已有；人工确认待 | Scratch `forbidden_volume_guid` |
| M16 | Scratch 物理空间耗尽 | fail-closed | 阻塞：提权 VHD | |
| M17 | prefix restore I/O 失败 | Target incomplete、不可挂载 | 阻塞：提权 VHD | |
| M18 | relocation I/O 失败 | Target incomplete、不可挂载 | 阻塞：提权 VHD | |
| M19 | 状态边界取消 | 符合取消表，线程退出 | 阻塞：提权 VHD | |
| M20 | 强制结束 Worker | 重启后状态可判断 | 阻塞：提权 VHD | |
| M21 | Backup Boot 写后异常 | Primary 仍无效 | 阻塞：提权 VHD | |
| M22 | Primary Boot outcome unknown | readback 决策 | 阻塞：提权 VHD | |
| M23 | CHKDSK 失败 | 不报告成功 | 阻塞：提权 VHD | |
| M24 | 完成后重启 Windows | 可挂载、非 dirty、哈希一致 | 阻塞：提权 VHD | |
| M25 | 增量 Archive chain | 合并视图正确 | 阻塞：提权 VHD | |
| M26 | Direct restore 回归 | 无 shrink 副作用 | 待执行 | 与 M01/M02 一并做 |

状态说明：

- **待执行**：当前会话可安排，不强制破坏性写
- **代码路径已有**：实现与合同已覆盖；仍需人工一次确认才算矩阵通过
- **阻塞：提权 VHD**：需要管理员 + 可丢弃 VHD/VHDX（本机 Hyper-V/diskpart 此前不可用）
- **部分**：文档/决策完成，破坏性补录未做

## 3. 发布门禁对照（§21）

| # | 条件 | 当前 |
| ---: | --- | --- |
| 1 | ADR-0025 已接受 | 满足 |
| 2 | LogFile/Dirty/Mount/CHKDSK 验证结论 | 决策满足；破坏性补录未完（SR0） |
| 3 | SR0–SR10 全部完成 | SR10 进行中 |
| 4 | 普通 + 关键元数据可迁移 | 代码完成；M03/M07/M08 未过 |
| 5 | unsupported layout 写前拒绝 | 代码完成；M10–M13 未过 |
| 6 | ShrinkPlan 可校验并绑定 token | 代码完成；待人工 digest 复核 |
| 7 | Boot 延迟提交 / outcome-unknown | 代码完成；M21/M22 未过 |
| 8 | CHKDSK 失败不报告成功 | 代码完成；M23 未过 |
| 9 | M01–M26 全部有通过记录 | **未满足** |
| 10 | Debug/Release 构建 + source-limit + 静态审计 | Debug/Release 全量构建 + source-limit 通过；静态审计见 SR10_STATIC_AUDIT（含 personal_archive/worker `fstream`→Win32 清理后复检通过） |
| 11 | 协议/模块/文案同步 | SR9 已同步；本轮复核 |
| 12 | 无测试代码 / 旧兼容 / 旧仓库依赖 | 待静态审计确认 |

**结论：** Debug/Release capability 已开启，以便用标准构建执行本矩阵；在 M01–M26 与门禁 9 未满足前，
该 Release 构建不构成发布资格，不得标记为已验证产品。

## 4. 提权环境检查清单（执行矩阵前）

1. 管理员会话；`New-VHD` 或 `diskpart` 可创建可销毁 VHDX
2. 隔离目录（非系统盘、非 Archive 所在卷）
3. 源卷：填充可哈希样本文件 + 可选稀疏/压缩/加密尾部用例
4. 备份一条 volume_set Full（及一条 Incremental 供 M25）
5. 目标卷：小于源、等于源、大于源各一
6. 记录 `AEGRA_DATA_DIR` 下 Worker restore 日志与 Service 日志路径
7. capability 已开启；矩阵全部通过并更新本文件后才能标记发布合格
