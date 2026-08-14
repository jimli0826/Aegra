# ADR-0016：文件集备份、恢复与个人 Archive V7 边界

- 状态：Accepted
- 日期：2026-08-07
- 决策者：Aegra 项目
- 关联模块：contracts、ports、format、pipeline、application、personal_repository、adapters/windows_filesystem、apps/worker、apps/service、apps/desktop
- 关联文档：[文件集备份与恢复设计](../architecture/FILE_SET_BACKUP_RESTORE.md)、[分阶段开发计划](../development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md)、[个人版 `.bkf` V7](../format/PERSONAL_BACKUP_FORMAT_V7.md)、[Repository Catalog V2](../format/PERSONAL_REPOSITORY_FORMAT_V2.md)、[Service 控制面 V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)、[产品上限与错误码](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md)
- 后续决策：文件 Incremental 与 reparse/hard link/sparse/ADS 范围已由 [ADR-0018](0018-file-set-incremental-usn-and-chain.md) 替代；本文其它边界仍有效

## 背景

Aegra 当前个人版数据面以磁盘或 Volume 的连续逻辑块地址空间为中心。`IBlockSource`、
`BackupPipeline`、Manifest `volumes[]`、V6 Chunk `source_type=volume`、恢复目标和 Service Inventory
都假定源是 Volume。文件或目录不是连续块设备；它们还包含目录层级、名称、多个数据流、ACL、属性、时间戳、
稀疏区间、硬链接、重解析点和逐项恢复语义。

直接把目录拼成一个虚拟块流会丢失文件边界并阻碍按文件浏览和恢复；让 Desktop 直接枚举路径并把绝对路径交给
Worker，则会绕过 Service 的身份、授权、规范化和计划持久化边界。当前 V6 又把完整 CBOR metadata 放在一个
最多 64 MiB 的 envelope 中，不能可靠承载百万级文件树。

产品尚未发布。可以直接确定正式格式和协议，不保留 V6、Service V3、Worker Job schema 3 或当前试验性
Desktop 文件树实现的兼容路径。

### 兼容性硬约束

本决策实施后，生产代码只实现当前正式版本：Archive V7、Service V4、Worker Job schema 4 和 Catalog V2。
禁止增加 V6/V7 双读、V3/V4 协商、旧字段别名、旧 magic 探测、格式转换、数据库升级、数据迁移、fallback、
feature flag 或“先尝试新格式、失败后读取旧格式”的分支。通用解析器仍必须拒绝不等于当前版本的输入，但不得
对 V6、V3、schema 3 或 Catalog V1 编写专用识别和处理逻辑。现有开发期 Archive、Repository、SQLite 和 IPC
样本均视为可丢弃数据，由开发者删除并使用当前代码重新生成。

## 决策

1. 文件和目录作为新的 `file_set` 保护类型，与现有 `volume_set` 并列。二者共享 Repository、Archive Group、
   压缩、加密、分卷、任务监督和发布协议，但使用不同的 Source/Sink Port 和 Backup/Restore Pipeline。
2. 不把文件集合适配成 `IBlockSource`，不把全部文件打成 TAR/ZIP 风格的不透明大流，也不让 Block Pipeline
   理解文件语义。
3. 个人 `.bkf` 升级到 V7。V7 同时支持 `volume_set` 和 `file_set`，现有 Volume 行为同步迁移到 V7；正式
   Reader/Writer 不增加 V6 双读、格式探测回退或迁移代码。
4. V7 Header 明文暴露最小的 `content_kind`，使无凭据 Repository 扫描可以重建 Catalog。客户文件名、路径、
   ACL、主机信息和文件树统计仍只存在于认证加密 metadata 中。
5. `file_set` 使用分段、独立保护的分页 File Index。加密 Archive 的每页使用独立 AEAD；未加密 Archive 的
   每页使用格式规定的摘要和校验值检测意外损坏，但不宣称抵抗主动篡改。索引在数据 Chunk 后写入，Footer 保存
   索引根定位、页数和根摘要。实现必须使用有界内存和 staging spool，不能把完整文件树常驻内存。
6. V7 文件数据 Chunk 使用 `source_type=file_stream`；`source_index` 指向 File Index 中唯一的 stream index，
   BlockEntry 的逻辑位置相对于该 stream。目录和无数据条目不占用 stream index。
7. Desktop 只消费 Service 的分页文件浏览结果。Service 返回与调用者身份、浏览会话和有效期绑定的 opaque
   `node_token`；Desktop 不发送绝对路径、Volume GUID、NT device path 或 VSS path。
8. 创建 Schedule 时，Service 把有效 token 解析为 durable `FileSelectionSpec`，保存稳定 Volume identity、
   规范化相对组件、递归与排除策略。后台执行不依赖 Desktop 会话或 token。
9. Service 到 Worker 的受保护内部协议携带已经解析的可信 File Source Ref。Worker 仍要重新验证组件、Volume
   归属和根逃逸，不信任持久化内容或进程输入。
10. 一个 File Job 涉及的全部本地 NTFS/ReFS Volume 加入同一个 VSS Snapshot Set，先完成快照，再从快照枚举
    目录并读取内容。首版提供 filesystem-consistent 语义，不宣称 application-consistent。
11. 重解析点默认只保存 link 本身的 tag/data，绝不跟随；这样避免环、越界和选择范围外数据。硬链接只存一份
    内容并记录 link group。稀疏文件保存 allocated range；ADS 作为命名 stream 保存。
12. 首版只支持本机 NTFS/ReFS 的 Full 文件备份和按文件恢复。UNC、EFS raw、离线 Cloud placeholder、
    application writer coordination 和文件级 Incremental 均显式返回稳定 unsupported/preflight 错误，不静默
    跳过或降级为不完整成功。
13. 默认完整性策略为 strict：选中范围内任一未被显式排除的条目无法枚举、读取或持久化时，整个 Job 失败并
    Abort。不存在“成功但悄悄漏文件”的路径。
14. 文件恢复默认写入用户选择的新目标目录，默认冲突策略为 `fail`。`replace` 和 `rename` 必须显式选择。
    每个普通文件先写同目录 staging 文件，flush、关闭并应用元数据后原子发布；目录树整体不宣称原子事务。
15. 恢复在第一次写入前完成 Archive/Index 认证、选择解析、目标根句柄绑定、空间预检、名称和路径安全检查。
    所有后续创建相对于已验证目录句柄进行，拒绝绝对路径、`.`、`..`、分隔符注入、设备名、ADS 注入和
    reparse escape。
16. Service 控制协议升级为 V4，Worker Job 升级为 schema 4，Repository Catalog 升级为 V2。所有生产消费者
    同步切换；不并行支持旧 schema。详见 [ADR-0017](0017-service-control-protocol-v4.md)。
17. 文件级 Incremental 不属于首版。后续只能在单独验收阶段启用：必须验证 USN Journal identity 和连续范围，
    任何 wrap、reset、不可用或基线不匹配都生成一个明确记录为 Full 的新恢复点，不生成未经证明的增量层。

### F0 冻结的开放决策

以下条目在 F0 完成时全部冻结，Adapter/Pipeline 不得再猜测：

#### A. SACL 读取策略（strict）

- 首版**承诺**备份 self-relative security descriptor 中的 Owner、Group、DACL 与 SACL。
- Worker/Adapter 在枚举前尝试启用 `SeBackupPrivilege` 与 `SeSecurityPrivilege`。
- 对显式选择范围内任一条目，若在启用 privilege 后仍无法读取完整 security descriptor（含 SACL），
  Job 按 strict failure 失败并 Abort，返回 `file_source.security_descriptor_unreadable`。
- 不存在“省略 SACL 仍算成功”的路径，也不允许 Adapter 静默写入空 SACL。
- 产品不承诺绕过 ACL 读取客户无权访问的内容；Backup privilege 是恢复可见性的手段，不是绕过审计的后门。

#### B. 目标文件系统能力（preflight reject）

- 恢复 preflight 必须查询目标根所在文件系统的能力：ADS、sparse、ACL/security descriptor、reparse。
- 若 Archive 选择闭包需要某项能力而目标不支持，preflight **拒绝**，返回稳定
  `file_restore.target_capability_missing`，并在 `message_arguments` 中给出能力名枚举值。
- 首版不提供 lossy 恢复策略（不丢 ADS、不丢 ACL、不把 sparse 展开为全零、不把 reparse 降为普通文件）。
- 目标必须是本地 NTFS 或 ReFS；其它 FS 在 preflight 拒绝。

#### C. File Index staging spool

| 项 | 规则 |
| --- | --- |
| 位置 | Worker Job 私有 staging 目录下的 `index-spool/`；不得放在保护源、目标恢复根或 Repository `archives/` |
| 命名 | `job-<job_id>/index-spool/page-<ordinal>.spool` 与 `entries.spool`；`job_id` 为控制面稳定 ID |
| 最大磁盘预算 | 单 Job 默认 **8 GiB**；超过返回 `file_backup.index_spool_budget_exceeded` 并 Abort |
| 单页上限 | 与格式 Index page 编码上限一致（见 V7：plain ≤ 1 MiB，encoded ≤ 1 MiB + AEAD overhead） |
| ACL | 仅 LocalSystem / 执行该 Worker 的服务账户可读写；继承禁用；Desktop 用户不可读 |
| 生命周期 | 仅存在于 Backup Job 运行期；Commit 成功后立即删除；Abort/Cancel/异常析构必须删除 |
| 崩溃清理 | Service/Worker 启动时扫描未完成 Job 的 spool 目录并删除；不得把 spool 当作恢复权威 |
| 内容 | 可含 entry/stream 元数据与页草稿；写盘前视为未认证；不得复制到 Catalog 或普通日志 |

#### D. 分卷与 Index / Footer 定位

- 分卷只在**完整 record** 边界切换（volume chunk、file stream chunk、index page 均为完整 record）。
- Index page record **可以**出现在任一 part（含首卷与续卷），但必须位于该 Archive 全部 file stream data
  chunk 之后、Footer 之前。同一逻辑 page ID 不得重复。
- Footer **只**写在末卷，且是末卷最后一个 record。
- Footer 使用 `(split_part_index, absolute_offset)` 二元组定位 index root page；Reader 打开对应 part 后
  从该绝对 offset 读取 root page record。
- `index_page_count`、`index_root_digest`、全 Archive 计数仅以 Footer 为准；Header 不保存这些字段。
- 发布顺序不变：Sidecar（若有）→ 续卷从后向前 → 首卷最后。File Full 不生成 `.bhx`，`has_sidecar=false`。

#### E. Windows 文件名编码

- Archive、Port、Worker 内部名称组件使用 tagged encoding：`name_encoding = windows_utf16le`（值 `1`）。
- 组件值为 **原始 UTF-16LE code unit 字节**，长度必须为偶数，范围 2–512 字节（1–256 个 UTF-16 code unit）。
- Codec **禁止**隐式 lossy UTF-8 转换；Service 展示层可替换不可显示 code unit，恢复必须使用认证原始 bytes。
- 排序与 B+tree key 比较使用 UTF-16LE 字节的稳定二进制序，不使用 locale collation。

## 备选方案

- **把目录压成单一 TAR/ZIP 流：** 实现较快，但不能高效分页浏览、选择性恢复、随机读取或可靠表达 Windows
  元数据，不采用。
- **实现虚拟 `IBlockSource`：** 可以复用 Block Pipeline，但会把文件边界和错误语义隐藏在 Adapter 内，恢复
  端仍需第二套私有索引，不采用。
- **把全部文件条目放入 V6 根 CBOR：** 大目录会超过 metadata 上限，打开时需要一次性分配和解析，不采用。
- **Desktop 直接枚举并提交路径：** 会产生授权混淆、TOCTOU、计划不可重放和高权限 Worker 任意路径输入，
  不采用。
- **首版使用实时文件系统、不使用 VSS：** 目录树与内容可能来自不同时间点，不能声明一致恢复点，不采用。
- **首版遇到不可读文件只给 warning：** 会把数据丢失伪装成成功备份，不采用。
- **沿用 V6 并增加可选 extension：** file index、Footer 和 Source 语义是关键格式变更，extension 无法给严格
  Reader 提供完整边界，不采用。
- **SACL 可选 / 目标 lossy 恢复：** 会让“成功”含义不确定，不采用。

## 影响

- 当前 Volume Reader/Writer、Sidecar、Catalog 和协议消费者需要一次性迁移到 V7/V2/V4/schema 4。
- `format` 增加 File Index 的纯模型和 codec，但仍不知道 Windows、路径 API、VSS、Qt 或 Storage。
- `ports` 和 `pipeline` 增加文件专用小接口；Block Pipeline 的行为不变。
- 新增 `adapters/windows_filesystem` Target，Win32 文件语义不进入核心模块。
- Archive 最终化阶段需要 staging index spool；崩溃和取消必须清理 spool 与未发布 Archive Group。
- 文件路径属于客户 metadata，只能出现在认证 Archive、受保护控制面记录和最小必要诊断日志中；Catalog 不保存
  原始路径。
- Desktop 当前本地 `QDir` 文件枚举不能进入正式数据流，应替换为 Service-backed model。

## 验证

- 审查 V7 Header、Index Page、Chunk 与 Footer 的固定布局、字节序、长度、上限、加密模式 AEAD AAD 和未加密
  模式摘要规则。
- 人工生成隔离样本，覆盖普通文件、空文件、深目录、Unicode/Windows 名称、ACL、ADS、稀疏文件、硬链接和
  不跟随的重解析点，并执行备份、分页浏览、选择性恢复和内容/元数据核对。
- 人工损坏 Header、Index page、父子引用、stream extent、Chunk 引用、Footer、分卷顺序和认证 tag，确认 Reader
  在写目标前拒绝。样本矩阵见[产品上限与错误码](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md)。
- 人工验证不可读文件、快照失败、磁盘满、取消、Worker crash、Catalog 发布失败和恢复冲突策略。
- 构建所有受影响生产 Target，运行仓库架构/静态检查、格式检查、`git diff --check` 和秘密扫描。
- 按 ADR-0015 不新增测试源码、测试 Target、CTest、fixture 或测试专用脚本。
