# `personal_repository` 模块开发文档

## 目标

为个人版提供受管理 `.bkf` Archive Store：创建/打开 Repository、发布 Catalog Entry、扫描和重建恢复点
目录、构建备份链图、解析显式恢复链，以及生成和执行链感知删除计划。

本模块不解析 Chunk Payload、不执行 Backup/Restore Pipeline、不解析凭据、不访问 SQLite、不实现计划和
保留策略，也不包含企业 CAS Pack、Chunk Index、Gateway、GC 或 Compaction。

权威决策见 [ADR-0010](../adr/0010-personal-repository-authority-and-catalog.md)，持久化 schema 见
[个人版 Repository V1](../format/PERSONAL_REPOSITORY_FORMAT_V1.md)。

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
summary: created_utc_ms, logical_size, stored_size, source_count
state: discovery, structural, authentication, chain, verification projection
```

同一字段存在多个来源时采用固定优先级：认证 Archive > 结构扫描 Archive > Catalog Entry。任何身份冲突
都返回损坏/冲突，不进行 last-writer-wins。

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

## 测试

- Catalog codec：golden、roundtrip、重复/未知 key、错误类型、非法 UUID/key 和容量上限。
- Storage Contract：短读、分页、取消、条件冲突、unknown outcome、幂等删除和 capability。
- Scanner：空仓库、Catalog 缺失/冲突、孤立 staging、缺卷、缺 Footer、重复 UUID和损坏 Header。
- Graph：全量、增量、差异、分叉、断链、环、跨 Set、重复节点和最大深度。
- Registration：Catalog 成功、失败后 Reconcile、同内容重复登记和 UUID 冲突。
- Delete Plan：叶子、子树、整链、遗漏后代拒绝、generation 变化和部分删除恢复。
- Local Adapter 集成：进程重启、文件 rename、只读目录、空间不足和路径逃逸。

## Definition of Done

- Repository/SQLite/企业 CAS 的权威边界与 ADR 一致。
- Catalog 丢失后可完全从结构完整的 Archive Group 重建身份、位置和链图。
- 无 Credential 扫描不执行 KDF；正式 Restore 前仍执行 Archive 认证和 Chain Reader 校验。
- 删除不能产生仍显示为可恢复的断链后代，并可从任意单对象失败幂等继续。
- Local Storage Adapter 通过公共 Contract Test；Memory Adapter 覆盖所有故障注入路径。
- Debug/Release、源码规模、依赖、静态分析、格式和秘密扫描通过。

## 当前状态

阶段 12A 已实现 `Aegra::PersonalRepository` 的首个纯核心切片：

- Repository Descriptor、Catalog Entry 和 Deletion Tombstone 的 C++20 DTO；
- V1 UTF-8 JSON codec，严格拒绝重复/未知 key、错误类型、非规范 UUID、超限文档和路径逃逸；
- Archive 主 key、分卷删除顺序、父链与备份类型不变量验证；
- `RecoveryPointGraph` 的重复 UUID、跨 Repository/Backup Set、差异父层、环和深度检查；
- 缺父节点的可发现状态，以及可恢复节点的 base-first 显式链解析。

Storage Port、Memory/Local Adapter、Repository Scanner、Catalog Reconcile、Delete Plan 生成和 Tombstone 执行
尚未实现，属于阶段 12B 及后续工作。
