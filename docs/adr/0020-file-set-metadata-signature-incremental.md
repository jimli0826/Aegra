# ADR-0020：文件集增量备份的 metadata signature 基线

- 状态：Accepted
- 日期：2026-08-09
- 决策者：Aegra 项目
- 关联模块：contracts、ports、format、pipeline、personal_repository、adapters/windows_filesystem、apps/worker、apps/service、apps/desktop
- 替代：ADR-0018 中以 USN Journal 连续性证明文件变化的增量判断
- 关联文档：[增量架构设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md)、[V7 格式](../format/PERSONAL_BACKUP_FORMAT_V7.md)、[Catalog V2](../format/PERSONAL_REPOSITORY_FORMAT_V2.md)

## 背景

Aegra 当前 file_set 增量设计依赖 NTFS/ReFS USN Journal、稳定文件 ID 和 journal checkpoint。该方案能降低漏备风险，
但会把文件级备份绑定到 Windows NTFS/ReFS 的 journal 语义，并且在 snapshot journal 不可用、journal wrap/reset 或
其它平台文件系统上频繁降级为 Full。

项目现在明确要求改为使用文件 metadata 判断内容是否变化，具体判断为 `write_time + logical_size`。设计目标是让
file_set 增量成为扫描式、文件系统无 journal 依赖的模型，并为 FAT32 等没有 NTFS USN 的源留下清晰边界。

## 决策

文件集增量采用：

> 完整枚举当前快照命名空间；对同一路径、同类型的普通文件，若 `write_time` 与 `logical_size` 均等于直接父层，
> 则认为文件内容未变化并引用父层 stream；否则本层写入完整普通文件主数据流。

具体规则：

- 每个 Recovery Point 仍保存完整、认证、可分页的当前 File Index；删除项直接从 tip Index 缺席。
- 父层匹配 key 为规范化 selection-relative path 与 entry kind；rename/move 在新路径上按新文件处理。
- 普通文件只有在父层存在同路径普通文件、`logical_size` 相同、`write_time` 相同且父 stream 可验证时，才写
  `content_storage=parent`。
- 任一条件不满足，包括新文件、大小变化、mtime 变化、目录/文件类型变化、父引用缺失或父层不可认证，均写完整
  local stream。
- 目录没有 payload；目录创建、删除、rename/move 和 metadata 变化由当前完整 File Index 表达。
- selection fingerprint、backup set、父链完整性、父 Archive 认证和不支持对象检测仍是增量资格条件。
- 不再需要 USN Journal、journal checkpoint、`FSCTL_READ_USN_JOURNAL` 变化提示或 journal 连续性证明来决定
  `effective_type=incremental`。
- `StableFileIdentity` 可继续作为加密 File Index 中的诊断/校验 metadata，但不再作为增量匹配 key 或变化证明。

## 格式与持久化

- V7 `CAP_FILE_USN_BASELINE` 更名为 `CAP_FILE_METADATA_BASELINE`，bit 值保持 `0x00000004`。产品未发布，不提供旧名兼容。
- file_set Full/Incremental 均必须携带 `file_set_baseline.selection_fingerprint`。
- `file_set_baseline.change_detection_method = 1` 表示 `mtime_size_v1`。
- `journal_checkpoints[]` 从 current V7 file_set metadata schema 移除。
- Catalog V2 的 `file_baseline_available=true` 表示已认证 selection fingerprint 与 metadata baseline，不再表示
  journal checkpoint 可用。

## 风险接受

该设计接受 metadata 判断的固有限制：如果文件内容变化但 `write_time` 和 `logical_size` 均未变化，本次增量可能错误
引用父层旧内容。该风险在以下场景更明显：

- 应用或用户刻意保留/回写 mtime；
- 文件系统时间戳精度较低，例如 FAT32 写入时间粒度可能导致短时间内修改被同一 mtime 表示；
- 跨工具复制、解压或同步时保留原始时间戳；
- 时钟回拨或文件系统/驱动未可靠更新时间戳。

Aegra 通过以下边界降低误解，但不消除该风险：

- 文档和 UI 不把该模式描述为内容哈希级别的变化检测；
- 当前完整枚举仍检测删除、新路径、大小变化和不支持对象；
- changed 文件写 whole-file，不做块级 delta；
- Verify 验证已保存/引用 payload 的可恢复性，但不能证明源端曾经发生过“同 mtime + 同大小”的未捕获修改。

## 备选方案

- **继续使用 USN：** 变化证明更强，但平台绑定和降级路径复杂；本次需求明确改为 mtime+大小。
- **每次读取并哈希所有文件：** 可发现同大小同 mtime 的内容变化，但成本接近 Full 扫描读取，违背当前简化目标。
- **双模式：USN 优先、metadata fallback：** 行为复杂且不同文件系统语义不一致；当前先统一为 metadata baseline。
- **按稳定文件 ID 匹配 rename/move：** 可减少 rename 后重传，但 FAT32 等目标没有同等身份合同；当前使用路径匹配保持一致。

## 影响

- FAT32 等无 USN 文件系统可在未来放宽 source 支持时复用同一变化判断；是否支持 VSS/ACL 等仍由 Windows source 能力另行决定。
- rename/move 后即使内容未变，也会在新路径写 local stream，空间效率低于稳定身份/USN 方案。
- 同大小同 mtime 修改存在漏备风险，是产品语义而非实现缺陷。
- 代码实现需要删除或停用 file_set 增量对 `query_journal_state` / `read_change_batch` 的必需依赖，并改造 planner 为
  parent path index + metadata signature 比较。

## 验证

- 构建受影响 production targets，运行架构/静态/格式/秘密检查和 `git diff --check`。
- 人工验证 Full→Incremental：无变化、内容变化、大小变化、mtime 变化、删除、新文件、rename/move、父层缺失、
  selection 改变、父凭据错误。
- 人工验证同大小同 mtime 的修改会被判为未变化，并在产品风险说明中记录该限制。
- 不新增测试源码、fixture、脚本、test executable、CTest 或其它项目测试资产。
