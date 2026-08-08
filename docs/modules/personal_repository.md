# `personal_repository` 模块开发文档

## 目标

为个人版提供受管理 `.bkf` Archive Store：创建/打开 Repository、发布 Catalog Entry、扫描和重建恢复点
目录、构建备份链图、解析显式恢复链，以及生成和执行链感知删除计划。

本模块不解析 Chunk Payload、不执行 Backup/Restore Pipeline、不解析凭据、不访问 SQLite、不实现计划和
保留策略，也不包含企业 CAS Pack、Chunk Index、Gateway、GC 或 Compaction。

权威决策见 [ADR-0010](../adr/0010-personal-repository-authority-and-catalog.md)，持久化 schema 见
[个人版 Repository V2](../format/PERSONAL_REPOSITORY_FORMAT_V2.md)（Catalog Entry `schema_version=2`，
含 `content_kind` / `file_entry_count` / `file_stream_count`）。

## 依赖与 Target

计划目录：

```text
src/personal_repository/
├── CMakeLists.txt
├── include/aegra/personal_repository/
│   ├── catalog.h
│   ├── chain_graph.h
│   ├── delete_plan.h
│   ├── repository.h
│   └── scanner.h
└── src/
    ├── catalog_codec.cpp
    ├── chain_graph.cpp
    ├── delete_plan.cpp
    ├── repository.cpp
    └── scanner.cpp
```

Target：`aegra_personal_repository` / `Aegra::PersonalRepository`。

允许依赖 `Aegra::Base`、`Aegra::Contracts`、`Aegra::Ports`、`Aegra::Format`。不得依赖 Windows SDK、
SQLite、具体 Storage Adapter、Personal Archive Adapter、JSON/HTTP Controller 或 UI。Catalog codec 若使用
第三方 JSON 库必须是 PRIVATE 依赖，公共接口只暴露 Aegra/C++ 类型。

## 端口需求

不要创建万能 Storage Backend。实现阶段按真实用例增加以下最小能力：

- `IObjectReader`：读取对象属性和有界范围；
- `IStagedObjectWriter`：写入暂存对象并显式完成/中止；
- `IPrefixEnumerator`：分页列举固定前缀，返回 key、size、generation；
- `IObjectPublisher`：条件创建或原子 rename 暂存对象；
- `IObjectDeleter`：带 operation ID 的幂等删除。

Port 必须使用 Repository 相对 key，支持取消、结构化错误和资源上限。Adapter 通过 capability 明确是否
支持原子 rename、条件创建、强一致 read-after-write 和强一致 list；模块不能根据 URI 或后端名称猜测。

## 核心模型

### RepositoryDescriptor

包含 Repository UUID、schema/layout 版本和固定前缀。打开 Repository 时先验证 Descriptor，再允许任何
扫描、发布或删除操作。

### RecoveryPointRecord

合并 Archive Header、结构扫描、可选认证 Manifest 和 Catalog Entry 的结果：

```text
identity: file_uuid, backup_set_uuid, parent_uuid, backup_type
location: archive_main_key, split_part_count, has_sidecar
summary: created_utc_ms, logical_size, stored_size, source_count, source_volume_ids
state: discovery, structural, authentication, chain, verification projection
```

`source_volume_ids` 是有序稳定 Volume 身份（与备份 Job 的 `source_refs` / Manifest `volume_id` 一致），
用于校验父点几何。自动选父的主轴是 `backup_set_uuid`：同一 Schedule 固定 set；序列可为
Full → Inc → … → Full → Inc（后继 Full 仍属同一 set；Archive/Catalog 中 Full 的 `parent_uuid` 仍为
null，set 内为森林）。

#### Service 增量选父与「树完整」判定

由 Service（`worker_job_service`）在启动增量 Job 前完成。StartBackup 不传 parent；**父候选唯一来源**是
控制面 `schedules.last_recovery_point_id`（下称 last_rp）。**不做 Catalog tip 扫描回退。**

1. **读 last_rp**  
   - 空 / 缺失 → **降级 Full**（仍用 `schedules.backup_set_uuid`）  
   - 有值 → 按 key `catalog/recovery-points/<file_uuid>.entry` 读单条 Catalog Entry  

2. **父点自身合格**  
   - `has_sidecar`、`structural_state == "complete"`  
   - `source_volume_ids` 与本次 Job 有序一致  
   - 类型 Full 或 Incremental  
   - `backup_set_uuid` 等于本 Schedule 的 set  
   - 任一项失败 → **降级 Full**（不重算 tip）  

3. **祖先链完整**（从 last_rp 沿 `parent_uuid` 逐条读 Catalog，深度上限 128）  
   - 每一步父 entry 必须存在且同 set  
   - 无环；终点为 `backup_type == Full` 且无 parent  
   - 缺层 / 环 / 超深 / 跨 set → **降级 Full**  
   - 存储 IO 或 JSON 解码失败 → 硬错误（拒绝 StartBackup）  

4. **成功挂父** → 本次 Job 为 Incremental，parent = last_rp  

5. **推进 tip**  
   - 仅在 Worker 成功且 **Catalog Entry 发布成功** 后，将 `last_recovery_point_id` 更新为本次
     `file_uuid`（Full 与 Inc 均更新）  
   - Job 失败 / 取消 / Catalog 未发布 → tip 不变  
   - 更换 Schedule 的 `repository_connection_id` → 清空 tip

同一字段存在多个来源时采用固定优先级：认证 Archive > 结构扫描 Archive > Catalog Entry。任何身份冲突
都返回损坏/冲突，不进行 last-writer-wins。

正常备份由 Service 先按 UTC 计算 `archives/YYYY/MM/<file_uuid>.bkf`，Worker 提交 Archive Group，
Service 再核对固定 Header、连续分卷、末卷 Footer 和 Sidecar Header，最后通过 staging + create-only
publish 写入对应 Catalog Entry。文件名、Archive Header 和 Catalog Entry 必须使用同一个 `file_uuid`；
不得使用 job ID 作为 Archive 文件名。

状态维度彼此独立：

| 维度 | 取值示例 | 权威依据 |
| --- | --- | --- |
| discovery | discovered、missing、deleting | 对象列举与 Deletion Tombstone |
| structural | unknown、complete、incomplete、corrupt | Header、分卷、Chunk 结构与 Footer |
| authentication | not_attempted、authenticated、failed | 指定凭据下的 Archive AEAD |
| chain | unknown、complete、incomplete、invalid | RecoveryPointGraph |
| verification | never、succeeded、failed | 本机 SQLite Verify 投影，不写 Catalog |

只有 structural=complete、authentication=authenticated、chain=complete 的点可以进入 Restore/Mount；全量
点的 chain=complete 表示自身合法，增量/差异点则要求完整祖先链。

### RecoveryPointGraph

以 `file_uuid` 为节点、`parent_uuid` 为有向边。构建时检测重复 UUID、自环、环、跨 Backup Set 父引用、
非法备份类型和深度上限。图可以包含结构完整但缺父的节点；此类节点不可生成 Restore Chain。

`resolve_chain(file_uuid)` 从指定点沿 `parent_uuid` 上溯到 Full，得到 base-first 祖先列表。完整条件：

- 上溯路径上每个父 UUID 均在图中有对应 Entry（中间节点缺失即 incomplete）；
- 路径终点的 Entry 类型为 Full（增量/差异终点不是 Full 即 incomplete）；
- 深度不超过图的 `maximum_chain_depth`（默认 128）。

增量选父与恢复预检共用该定义：Service 用它判定 tip/显式父的祖先链是否可挂增量；Application 用它
生成 Restore/Mount 的 base-first 层列表。二者都只依赖 Catalog 图，不在图构建阶段打开 Archive。

### DeletePlan

计划包含 operation ID、按后代优先排序的 Recovery Point、每个 Catalog generation 和所有已发现 Archive
成员。Plan 创建与执行分离；执行前重新校验图和 generation，禁止边扫描边删除。

## 用例与状态机

### 创建和连接

```text
Create: Validate Empty Target -> Stage Descriptor -> Conditional Publish -> Open
Open: Read Descriptor -> Validate Schema/Prefixes -> Load Catalog -> Optional Reconcile
```

非空且没有 Descriptor 的目标不能自动初始化；用户必须显式选择“导入现有 Archive”，该用例先扫描、
报告冲突，再创建新的 Descriptor。由于产品未发布，不提供旧 Repository 格式导入器。

### 提交后登记

Worker Commit 成功后，Application 向 Repository 提交已知 Archive 主 key 和预期 `file_uuid`：

```text
Open Archive Structurally
-> Verify Expected Identity and Group Completeness
-> Build Catalog Entry
-> Conditional Publish Entry
-> Return Registered or AlreadyRegistered
```

Catalog 写失败返回“Archive 已提交但未登记”，不能重跑非幂等备份。Service 安排 Reconcile 即可。

### 扫描与认证

扫描分为廉价结构阶段和显式认证阶段。结构阶段不获取 Secret，不解析加密 Manifest；认证阶段只处理明确
选中的 Recovery Point，并由 Application 提供已解析 Secret 生命周期。模块不保存 SecretRef。

### 链解析

Application 按用户选择的叶子调用 `resolve_chain()`，得到 base-first 的 Archive key 列表。随后 Application
根据 `repository_uuid + file_uuid` 解析每层 CredentialRef，并交给现有 PersonalArchiveChainReader。

### 删除

Application 先请求 `plan_delete()` 并向用户展示影响范围；确认后用同一 Plan 执行。删除文件的具体 I/O
经 Storage Port 完成。不存在成员视为幂等成功，身份或 generation 冲突要求重新计划。

## SQLite 边界

个人版 SQLite Adapter 位于 `adapters/sqlite`，由本地 Service 组合。建议投影包含：

- Repository 连接与 Storage CredentialRef；
- `repository_uuid + file_uuid` 到 Archive CredentialRef 的映射；
- Recovery Point 查询摘要和最近扫描状态；
- Job、Schedule、Retention Policy、Verify Result 和 UI 状态。

SQLite 不能成为以下信息的唯一来源：Recovery Point UUID、父链、Archive key、提交状态或恢复所需
Manifest。删除 SQLite 不得删除 Repository 数据；重新连接后必须能够重建查询投影。

## 并发、取消与错误

- 同一 Repository 实例允许并发只读查询；写操作以 Repository-scoped operation 协调，但不得持锁执行
  网络 I/O。
- 每个写操作使用稳定 operation UUID，重试不得创建第二个 Archive 或 Catalog Entry。
- 扫描、列举、认证和删除接受 CancellationToken。已开始的单对象 publish/delete 必须完成或报告未知
  结果，调用方随后通过属性查询对账。
- 错误码区分 unavailable、conflict、corrupt、incomplete、credential required、cancelled 和 partial。
- 日志可以记录 Repository UUID、operation UUID、file UUID、稳定 message code 和对象数量，不记录
  Storage Credential、SecretRef、源路径或客户 Metadata。

## 验证

- 审查 Catalog codec、Storage Port、Scanner、Graph、Registration 和 Delete Plan 的全部边界规则。
- 使用隔离的非生产 Repository 人工验证重启、rename、只读目录、空间不足、损坏输入和路径逃逸。
- 构建 PersonalRepository、Memory/Local Storage Adapter 及其直接生产消费者。

## Definition of Done

- Repository/SQLite/企业 CAS 的权威边界与 ADR 一致。
- Catalog 丢失后可完全从结构完整的 Archive Group 重建身份、位置和链图。
- 无 Credential 扫描不执行 KDF；正式 Restore 前仍执行 Archive 认证和 Chain Reader 校验。
- 删除不能产生仍显示为可恢复的断链后代，并可从任意单对象失败幂等继续。
- Local Storage 与 Memory Adapter 的行为与公共 Storage Port 契约一致。
- Debug/Release、源码规模、依赖、静态分析、格式和秘密扫描通过。

## 当前状态

阶段 12A 已实现 `Aegra::PersonalRepository` 的首个纯核心切片：

- Repository Descriptor、Catalog Entry 和 Deletion Tombstone 的 C++20 DTO；
- V1 UTF-8 JSON codec，严格拒绝重复/未知 key、错误类型、非规范 UUID、超限文档和路径逃逸；
- Archive 主 key、分卷删除顺序、父链与备份类型不变量验证；
- `RecoveryPointGraph` 的重复 UUID、跨 Repository/Backup Set、差异父层、环和深度检查；
- 缺父节点的可发现状态，以及可恢复节点的 base-first 显式链解析。

阶段 12B 已实现细粒度 Repository Object Storage Port、稳定 `kOutcomeUnknown` 错误语义、线程安全 Memory
Object Storage 参考实现。Memory 实现支持范围短读、流式 staging、分页列举、
generation 条件发布、幂等删除、取消和确定性故障注入。

阶段 12C 已实现 Windows Local Storage Adapter，并遵循公共 Object Storage Contract；其路径、staging、
generation、发布和删除语义见 [Local Storage 模块文档](storage_local.md)。

阶段 13B 已实现 `RepositoryCatalogScanner`：先验证 Descriptor，再分页读取 Catalog Entry 与 Deletion
Tombstone，隐藏删除中的 Recovery Point，验证 Repository/UUID/链图不变量，并按 `file_uuid` 稳定分页。

S5 已增加 `delete_plan`：descendant-first 计划、带 Storage generation 的 `members`
（sidecar → 续卷 → 主卷）、strict revalidation、Tombstone 发布与条件/幂等成员及 Catalog 删除执行。
Catalog Reconcile（从 Archive 结构补建 Entry）仍属后续工作。

F2 已将 Catalog Entry 升至 schema 2：`content_kind`（`volume_set`|`file_set`）、文件统计字段与
`format_version=7` 校验；`file_set` 禁止 sidecar/source_volume_ids，且 `backup_type` 必须为 Full。

S4 Add 业务闭环已实现创建路径：仅缺失或空目标可初始化；Application 生成 Descriptor、通过 staging +
create-only publish 写入 `aegra.repository`，并在持久化 Available 连接前读回验证。Import 仍只打开并验证已有
Descriptor，不承担初始化。
