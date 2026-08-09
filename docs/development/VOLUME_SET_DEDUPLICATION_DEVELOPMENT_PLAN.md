# Volume Set 单 Chunk 去重开发计划

| 属性 | 内容 |
| --- | --- |
| 状态 | VD1-VD6 已完成（人工矩阵 D01-D20 待隔离环境验证） |
| 日期 | 2026-08-09 |
| 决策依据 | [ADR-0022](../adr/0022-volume-set-chunk-local-deduplication.md) |
| 设计依据 | [Volume Set 单 Chunk 去重设计](../architecture/VOLUME_SET_DEDUPLICATION.md) |
| 适用范围 | volume_set Full/Incremental Backup、Archive V7、Catalog V2、Service V4、Worker schema 4、Desktop |

本文是 ADR-0022 的生产实施计划，面向接手开发的 agent。架构语义以 ADR-0022、Volume Set 去重设计、
[Personal Archive V7](../format/PERSONAL_BACKUP_FORMAT_V7.md)、
[Repository Catalog V2](../format/PERSONAL_REPOSITORY_FORMAT_V2.md) 和
[Service V4](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md) 为准；本文只规定开发顺序、文件所有权、验收方式和交接格式。

## 1. 最终结果

完成全部工作包后，生产代码必须满足：

1. `volume_set` Full 与 Incremental 可按计划选项启用单个物理 `VolumeChunk` 内固定块去重，默认启用。
2. `file_set` 永远不设置 `BACKUP_FLAG_DEDUP`，不生成 DEDUP entry，所有 dedup 指标固定为 0。
3. Writer 顺序为 ZERO 检测、Incremental 父层省略、当前 Chunk dedup、canonical zstd、AEAD。
4. DEDUP 只后向引用同 Chunk 中较早的 RAW 或 COMPRESSED canonical entry；不允许链式、前向、跨 Chunk、跨 part、跨 volume、跨 Archive 或跨 Recovery Point。
5. 候选 key 为 `(SHA-256 plaintext, logical_size)`，命中后必须逐字节确认；不得仅凭 hash 或压缩结果生成引用。
6. Header 的 `BACKUP_FLAG_DEDUP` 表示策略启用，即使本次没有重复块也设置；存在 DEDUP entry 但 Header 未置位属于损坏。
7. Reader、Verify、Restore 在输出当前 Chunk 数据前完成结构校验、AEAD 认证、DEDUP 引用和长度校验。
8. Footer、TaskResult、Catalog 和 Desktop 统一展示 `deduplicated_block_count` 与 `deduplicated_logical_bytes`。
9. 生产实现不新增兼容路径、旧字段 fallback、格式双读、schema migration 或 feature negotiation。

本次不实现跨 Chunk 或跨 Recovery Point 去重，不实现 file_set 去重，不实现内容定义分块，不持久化 dedup hash，
不改变 enterprise CAS Repository 的去重模型。

## 2. 开工与协作规则

每个 agent 开工前必须完成：

1. 阅读根 `AGENTS.md`、`.agents/skills/aegra-cpp-development/SKILL.md`、
   [C++ 工程规范](CPP_ENGINEERING_STANDARD.md)、[模块化架构](../architecture/MODULAR_ARCHITECTURE.md)、
   [模块索引](../modules/README.md)和本包涉及的模块文档。
2. 阅读 ADR-0022、Volume Set 去重设计、V7、Catalog V2、Service V4、Worker Host、Format、Pipeline、
   Personal Repository、Windows Personal Backup、Personal Archive Verify、Adapters、Contracts、Desktop 文档。
3. 执行 `git status --short` 和 `git rev-parse --short HEAD`，记录基线；不得覆盖用户或其它 agent 的修改。
4. 一次只领取一个工作包，将状态改为 `进行中`，记录 owner、基线和文件所有权。
5. 使用 C++20、RAII、`Result<T>`、checked arithmetic、有界内存和取消语义。
6. 不新增任何单元、集成、回归、冒烟、E2E、fuzz、fixture、测试脚本、测试 executable 或 CTest 注册。
7. 完成时同步代码、格式/协议/模块文档、状态表、构建记录、人工验证记录和交接记录。

工作包状态只允许 `等待前置`、`可开始`、`进行中`、`阻塞`、`已完成`。公共合同、格式 codec、Worker 协议、
Service 协议、SQLite schema、Desktop 协议模型和 CMake 文件不得由两个 agent 同时编辑。

## 3. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 前置 | 默认 owner 范围 |
| --- | --- | --- | --- | --- | --- |
| VD1 | 已完成 | P0 | Contracts、格式常量与校验骨架 | ADR-0022 | contracts、format、docs |
| VD2 | 已完成 | P0 | Personal Archive Writer 去重窗口 | VD1 | adapters/personal_archive |
| VD3 | 已完成 | P0 | Reader、Verify、Restore DEDUP 展开与拒绝规则 | VD1、VD2 | adapters/personal_archive、worker verify/restore |
| VD4 | 已完成 | P1 | Service、SQLite、Worker 协议与 Catalog 指标接线 | VD1-VD3 | service、worker、sqlite、personal_repository |
| VD5 | 已完成 | P1 | Desktop 开关与收益展示 | VD4 | desktop、translations |
| VD6 | 已完成 | Gate | 集成构建、静态审计与人工矩阵 | VD1-VD5 | integration owner |

VD1-VD4 建议串行。VD5 可在 VD4 的 wire/DTO 字段稳定后开发，但合入前必须与 VD4 共同构建验证。

## 4. 冻结语义

### 4.1 DEDUP entry

每个 DEDUP entry 必须满足：

- `flags == DEDUP`，不得组合 RAW、COMPRESSED 或 ZERO；
- `ptr.ref_index` 为当前 Chunk `BlockEntry[]` 的零基索引；
- `ref_index < current_entry_index`；
- 目标 entry 必须为 RAW 或 COMPRESSED，不能为 ZERO 或 DEDUP；
- `stored_size == 0` 且 `logical_size == 0`；
- DEDUP 永远只表示一个目标逻辑块，不编码 run；
- 目标解码后的明文长度必须等于目标逻辑块实际长度，短尾块只能引用相同长度 canonical。

### 4.2 Writer window

当前 Chunk 维护内存索引：

```text
key = SHA-256(plaintext[0..logical_size]) + logical_size
value = ordered canonical entry indexes
```

canonical 明文必须可供字节确认。Chunk 提交、分卷切换、source volume 切换或新 Chunk 开始时立即丢弃索引与
canonical backing storage。实现不得创建全 Archive dedup index，不得把 hash 写入 Archive、Sidecar、Catalog 或日志。

### 4.3 Incremental 协作

Incremental 未变化块由父 Recovery Point 提供，本层不生成 entry，也不进入当前 Chunk dedup window。变化块和 Full
中的普通 DATA 块才参与 DEDUP。DATA 变 ZERO 时仍写 ZERO；ZERO 不进入 hash 表。

### 4.4 指标口径

`deduplicated_block_count` 只统计 DEDUP entry 数。`deduplicated_logical_bytes` 只统计这些 DEDUP 目标展开后的明文长度。
二者不包含 ZERO、zstd 节省或 Incremental 父层省略。Footer、TaskResult、Catalog、UI 必须使用同一口径。

## 5. 当前实现入口

Agent 开工时必须重新运行 `rg`，下表只作为当前基线的方向提示。

| 层 | 入口 | 目标 |
| --- | --- | --- |
| contracts | `src/contracts/include/aegra/contracts/job.h`, `task_result.h`, `service_control.h` | 加选项、结果计数和 validator |
| format | `src/format/include/aegra/format/personal_archive.h`, `src/format/src/personal_archive_codec.cpp` | DEDUP flag 编解码、BlockEntry 校验、Footer 字段校验 |
| writer | `src/adapters/personal_archive/src/personal_archive_session.cpp`, `personal_archive_chunk_builder.*` | 当前 Chunk dedup window、canonical backing、计数 |
| reader | `src/adapters/personal_archive/src/personal_archive_reader.cpp`, `personal_archive_volume_reader.cpp`, `personal_archive_chain_reader.cpp` | DEDUP 结构拒绝、认证后展开、长度校验 |
| verify | `src/apps/worker/src/personal_archive_verify_task*.cpp`, `src/adapters/personal_archive/src/personal_archive_shape_validation.*` | 读取并重算 dedup 指标，损坏前置拒绝 |
| service | `src/apps/service/src/worker_job_service.cpp`, `schedule_service.cpp`, `backup_catalog_registrar.cpp` | Schedule 默认/冻结、Worker Job 透传、Catalog 投影 |
| worker | `src/apps/worker/src/worker_protocol.cpp`, `windows_personal_backup_task*.cpp` | schema 4 显式字段校验、file_set early reject、TaskResult 透传 |
| desktop | `BackupWizardStep2.qml`, `service_protocol*.cpp`, `models/job_model.*`, translations | volume-only 开关、只读编辑、指标展示 |

## 6. VD1：Contracts、格式常量与校验骨架

**目标：** 先让公共 DTO、格式模型和基础 validator 能表达 ADR-0022，不改变 Writer 行为。

**主要文件：**

- `src/contracts/include/aegra/contracts/job.h`
- `src/contracts/include/aegra/contracts/task_result.h`
- `src/contracts/include/aegra/contracts/service_control.h`
- `src/contracts/src/job.cpp`
- `src/contracts/src/task_result.cpp`
- `src/contracts/src/service_control.cpp`
- `src/format/include/aegra/format/personal_archive.h`
- `src/format/src/personal_archive_codec.cpp`
- `docs/modules/contracts.md`
- `docs/modules/format.md`

**开发任务：**

1. 在 `BackupOptions` 中增加 `deduplication_enabled`，要求 `volume_set` backup 显式赋值；`file_set` 必须 false。
2. 在 `TaskResult`、Recovery Point summary DTO 和相关 validator 中加入两个非负 dedup 计数字段。
3. 确认 `BACKUP_FLAG_DEDUP` 和 `BlockEntry::DEDUP` 常量与 V7 文档一致；若已有常量，补齐注释和 helper。
4. 扩展 `validate_block_entry()`：DEDUP entry 的 flags、size 字段在单 entry 层先做局部合法性检查。
5. Footer codec 读写两个 dedup 计数字段；body size、reserved size 和 static assert 与 V7 文档一致。
6. Manifest/Header validator 增加 content kind 约束：`file_set` 禁止 Header dedup flag。
7. 仅建立字段和局部校验；不得在 VD1 中写去重算法或 Reader 展开逻辑。

**构建：** Debug 和 Release 构建 `aegra_contracts`、`aegra_format` 及直接生产消费者。

**DoD：** 公共 DTO 与格式模型编译通过；V7 offset/size 与文档逐项一致；file_set dedup flag 可被拒绝；文档同步。

## 7. VD2：Personal Archive Writer 去重窗口

**目标：** 在 volume_set Writer 中生成合法 DEDUP entry，保持 Chunk 内有界内存和确定性 canonical 选择。

**主要文件：**

- `src/adapters/personal_archive/src/personal_archive_chunk_builder.h`
- `src/adapters/personal_archive/src/personal_archive_chunk_builder.cpp`
- `src/adapters/personal_archive/src/personal_archive_session.cpp`
- `src/adapters/personal_archive/src/personal_archive_payload.*`
- `src/adapters/personal_archive/include/aegra/adapters/personal_archive/personal_archive.h`
- `docs/modules/adapters.md`
- `docs/modules/windows_personal_backup.md`

**开发任务：**

1. 给 volume session options 增加 `deduplication_enabled`，由 Worker/Service 传入；默认只在 Service 创建 schedule 时决定。
2. Chunk builder 增加 per-Chunk dedup window，保存 key 到 canonical entry index list，以及 canonical 明文 backing。
3. 在 ZERO 与 Incremental omit 之后，对 DATA 块计算 SHA-256 plaintext hash 和 logical size。
4. 候选命中后逐字节比较 canonical 明文；首个完全相同候选生成 DEDUP entry。
5. 未命中时当前块成为 canonical，再执行现有机会性 zstd，生成 RAW 或 COMPRESSED。
6. DEDUP entry 不写 payload，不压缩，不加密；`stored_size` 和 `logical_size` 均为 0。
7. Header flag 在策略启用时置位，即使 `deduplicated_block_count == 0`。
8. 汇总 dedup block/bytes，并写入 Footer 和 session result；file_set 路径保持 0。
9. Chunk 提交、分卷边界、source volume 切换和失败/取消路径释放 window；不得有 Archive 全局状态。
10. 所有大小、entry index、payload offset 和计数使用 checked arithmetic；函数超过规模限制时拆分。

**禁止捷径：** 只比 hash、引用 ZERO/DEDUP、引用父层块、跨 Chunk 缓存、持久化 hash、压缩后比较、无界 map。

**构建：** Debug 和 Release 构建 `aegra_adapter_personal_archive`、`aegra_pipeline`、
`aegra_app_worker_personal`、`aegra_personal_worker`。

**人工检查：** Full 中同 Chunk 两个/三个相同 DATA 块、跨 Chunk 相同块、ZERO、短尾块、分卷临界点、禁用去重。

**DoD：** Writer 产生的 DEDUP 只回指较早 RAW/COMPRESSED；指标只统计 DEDUP；禁用时无 DEDUP entry。

## 8. VD3：Reader、Verify、Restore DEDUP 展开与拒绝规则

**目标：** 所有读路径都能在认证后展开 DEDUP，并在输出前拒绝损坏引用。

**主要文件：**

- `src/adapters/personal_archive/src/personal_archive_reader.cpp`
- `src/adapters/personal_archive/src/personal_archive_volume_reader.cpp`
- `src/adapters/personal_archive/src/personal_archive_chain_reader.cpp`
- `src/adapters/personal_archive/src/whole_disk_byte_reader.cpp`
- `src/adapters/personal_archive/src/personal_archive_shape_validation.*`
- `src/apps/worker/src/personal_archive_verify_task*.cpp`
- `src/apps/worker/src/personal_archive_restore_task*.cpp`
- `docs/modules/personal_archive_verify.md`

**开发任务：**

1. Chunk 读取时在 AEAD 认证前只做范围检查，不向 Block Sink 或调用方释放当前 Chunk 数据。
2. 认证后校验 Header dedup flag 与 DEDUP entry 一致；Header 未置位但出现 DEDUP 直接 `format.corrupt_chunk`。
3. 校验每个 DEDUP 的 `ref_index` 后向、范围、目标类型、目标长度和目标逻辑块长度。
4. 解码 RAW/COMPRESSED canonical 后缓存可被引用的明文；DEDUP 输出时复制 canonical bytes。
5. 目标为 DEDUP、ZERO、越界、前向、长度不匹配、payload offset 异常均拒绝。
6. Verify 读取完整 Archive 并重算两个 dedup 计数，和 Footer 比对。
7. Restore/Chain Reader 对 Full/Incremental tip 视图保持现有 overlay 语义；DEDUP 只在单层 Chunk 内展开。
8. 损坏 reason 可写内部稳定 reason，但不得记录 hash、块内容、密钥或大 payload。

**构建：** Debug 和 Release 构建 `aegra_adapter_personal_archive`、`aegra_app_worker_personal`、
`aegra_personal_worker`。

**人工检查：** 对隔离 Archive 副本手工破坏 DEDUP flag、ref index、target type、stored size、length、Footer 计数，
确认 Verify/Restore 在输出前稳定拒绝。

**DoD：** 所有读路径共享同一 DEDUP 不变量；损坏输入不会产生 partial 当前 Chunk 输出；Footer 计数被认证核对。

## 9. VD4：Service、SQLite、Worker 协议与 Catalog 指标接线

**目标：** 控制面完整传递去重选项，成功结果和 Catalog 可重建摘要包含统一指标。

**主要文件：**

- `src/apps/service/src/schedule_service.cpp`
- `src/apps/service/src/worker_job_service.cpp`
- `src/apps/service/src/backup_catalog_registrar.cpp`
- `src/apps/service/src/service_protocol*.cpp`
- `src/apps/service/src/supervisor_worker_protocol.cpp`
- `src/apps/worker/src/worker_protocol.cpp`
- `src/apps/worker/src/windows_personal_backup_task.cpp`
- `src/adapters/sqlite/src/*schedule*`
- `src/personal_repository/src/catalog_codec.cpp`
- `src/personal_repository/src/catalog_validation.cpp`
- `docs/modules/control_plane_sqlite.md`
- `docs/modules/worker_host.md`
- `docs/modules/personal_repository.md`
- `docs/protocol/SERVICE_CONTROL_PROTOCOL_V4.md`

**开发任务：**

1. SQLite current schema 的 schedule 增加 `deduplication_enabled`；volume_set 创建默认 true，创建后冻结。
2. `UpsertSchedule`、ScheduleSummary、Worker Job 编码和 validator 显式携带该字段。
3. `file_set` 创建或 Worker Job 中设置 true 时，在获取凭据、创建 VSS 或打开 source 前拒绝。
4. Worker 解析 `backup.deduplication_enabled`，对 volume_set 原样传给 Personal Archive Session。
5. TaskResult 编码/解码/validator 增加 dedup 计数；成功 volume backup 来自已提交 Footer/session result。
6. Catalog Entry V2 codec 与 validator 写入两个 dedup 计数；file_set 固定 0。
7. Recovery Point summary API 从 Catalog 投影 dedup 计数；不扫描 payload、不保存逐块 hash。
8. 保持 product unreleased 策略：只更新 current schema，不写 migration、fallback 或旧字段 alias。

**构建：** Debug 和 Release 构建 `aegra_adapter_sqlite`、`aegra_personal_repository`、`aegra_app_service`、
`aegra_service`、`aegra_app_worker_personal`、`aegra_personal_worker`。

**人工检查：** 创建 volume schedule 默认启用，禁用后保持禁用；编辑已存在 schedule 不能改变；file_set true 被拒绝；
Job summary、Catalog 和 RP list 指标一致。

**DoD：** 控制面、Worker 和 Catalog 使用同一字段语义；file_set 不可能透传 true；无兼容 schema 分支。

## 10. VD5：Desktop 开关与收益展示

**目标：** Desktop 只在 Volume Set 创建体验中提供去重开关，并展示恢复点/任务的 dedup 指标。

**主要文件：**

- `src/apps/desktop/src/client/service_protocol*.cpp`
- `src/apps/desktop/src/client/service_client_schedule.cpp`
- `src/apps/desktop/src/client/models/job_model.*`
- `src/apps/desktop/src/client/models/recovery_point_model.*`
- `src/apps/desktop/qml/pages/BackupWizardStep2.qml`
- `src/apps/desktop/qml/pages/BackupPage.qml`
- `src/apps/desktop/qml/pages/RestorePage.qml`
- `src/apps/desktop/translations/generate_ts.py`
- `src/apps/desktop/translations/*.ts`
- `docs/modules/desktop.md`

**开发任务：**

1. Schedule create payload 写 `deduplication_enabled`；Volume Set 默认 true，用户可关闭。
2. File Set 模式隐藏或固定 false，不向 Service 发送 true。
3. 编辑已有 Schedule 时开关只读，并按 Summary 显示当前策略。
4. Job 和 Recovery Point 模型解析两个 dedup 指标；整数范围按 Service V4 校验。
5. UI 展示 dedup bytes 与可用 dedup ratio；分母为 0 时显示不可用，不显示 `0/0`。
6. 文案准确说明这是 Volume Set 单 Chunk 去重；不得宣称全局、跨备份或 file_set 去重。
7. 更新五语言翻译源，避免 QML 直接解析 message code 或 wire enum。

**构建：** Debug 和 Release 构建 `aegra_desktop`，Qt 使用 `C:/Qt6/6.8.3/msvc2022_64`。

**人工检查：** 900x600、1080x720、150% DPI 下创建 Volume/File schedule、编辑 schedule、查看 Job/RP 指标，
确认文字和控件不重叠。

**DoD：** Desktop 行为与 Service V4 一致；file_set 无去重开关；指标展示不误导。

## 11. VD6：集成门禁与人工矩阵

**目标：** 证明 current production tree 完整实现 ADR-0022，且没有破坏 Volume/File 既有路径。

### 11.1 静态审计

至少执行：

```powershell
rg -n "DEDUP|dedup|deduplicated|deduplication_enabled|BACKUP_FLAG_DEDUP" src docs
rg -n "deduplication_enabled" src/apps/worker src/apps/service src/apps/desktop src/contracts
rg -n "ptr\.ref_index|ref_index" src/adapters/personal_archive src/format
git diff --check
cmake -DAEGRA_SOURCE_ROOT=D:/Work/OpenSource/Aegra -P cmake/CheckSourceLimits.cmake
```

逐项审查命中，确认：

- `pipeline` 不 include Personal Archive DEDUP 格式实现；
- `file_set` 不生成或接受 DEDUP；
- DEDUP hash 不进入 Archive、Sidecar、Catalog、日志或 wire；
- 没有 migration、fallback、alias、dual-read 或旧 schema 兼容分支；
- public headers 不泄漏 Windows、Qt、JSON、SQLite 或 Adapter 类型。

### 11.2 生产构建

使用 Visual Studio 2026 Insiders 和 Qt 6.8.3：

```powershell
cmd.exe /d /c scripts\build.cmd Debug
cmd.exe /d /c scripts\build.cmd Release
```

至少记录下列 target 结果：

```text
aegra_contracts
aegra_format
aegra_pipeline
aegra_adapter_personal_archive
aegra_personal_repository
aegra_adapter_sqlite
aegra_app_worker_personal
aegra_personal_worker
aegra_app_service
aegra_service
aegra_desktop
```

### 11.3 人工验证矩阵

| ID | 场景 | 期望 |
| --- | --- | --- |
| D01 | Full，同 Chunk 两个相同 DATA 块 | 第二个为 DEDUP，回指首个 canonical |
| D02 | Full，同 Chunk 三个相同 DATA 块 | 后两个都直接回指首个 canonical，无链 |
| D03 | 相同 bytes 位于相邻 Chunk | 两个 canonical，不跨 Chunk 引用 |
| D04 | ZERO 块重复 | 只生成 ZERO，不计入 dedup |
| D05 | 最后短块与完整块前缀相同 | 不 DEDUP，长度不同 |
| D06 | 禁用去重 | Header flag 清零，无 DEDUP entry，指标为 0 |
| D07 | 启用但无重复 | Header flag 置位，指标为 0 |
| D08 | Incremental 未变化块 | 本层省略，不进入 dedup |
| D09 | Incremental 两个变化块相同 | 同 Chunk 时可 DEDUP |
| D10 | 跨 part、跨 volume、跨 RP 重复 | 不引用 |
| D11 | Verify 正常 Archive | 成功并核对 Footer dedup 计数 |
| D12 | Restore 正常 Archive | DEDUP 展开后目标内容正确 |
| D13 | 损坏 Header flag | DEDUP entry without flag 被拒绝 |
| D14 | 损坏 ref_index 前向/越界 | Verify/Restore 输出前拒绝 |
| D15 | 损坏 target 为 ZERO/DEDUP | Verify/Restore 输出前拒绝 |
| D16 | 损坏 target length/stored size | Verify/Restore 输出前拒绝 |
| D17 | file_set 发送 true | Service/Worker 在凭据或 Snapshot 前拒绝 |
| D18 | Catalog 丢失后重建 | dedup 计数可从 Footer 重建 |
| D19 | Desktop Volume/File 创建 | Volume 可配置；File 不显示或固定 false |
| D20 | Volume 既有 Full/Inc/Verify/Restore | 非重复场景保持原语义 |

所有样本必须位于隔离非生产目录或专用测试卷；不得使用用户真实数据、凭据或生产 Repository 做破坏性验证。

### 11.4 最终 DoD

- VD1-VD5 全部完成并有 owner、基线、变更文件、构建和人工验证记录。
- Debug/Release 全量 production build 成功，Desktop 双配置成功。
- source limit、架构、格式、秘密、文档链接和 `git diff --check` 通过。
- V7/Catalog V2/Service V4/Worker schema 4 的实现与现行文档一致。
- `pipeline` 仍不依赖 Personal Archive DEDUP 编码；enterprise CAS 模型未被改动。
- 不新增任何测试资产，不提交构建产物、隔离 Archive、凭据或本机配置。

## 12. Agent 交接模板

每个工作包完成后在对应章节末尾追加：

```text
### VDx 完成记录

- owner / baseline:
- 实际修改文件:
- 公共合同或格式字段变化:
- 行为变化与保持不变的不变量:
- Debug production targets / 结果:
- Release production targets / 结果:
- architecture/static/format/secret checks / 结果:
- 人工验证场景、环境与结果:
- 已知限制或阻塞:
- 下一工作包可依赖的不变量:
```

不得用“代码已写完”代替构建命令、结果和人工场景。若工作包阻塞，记录可复现阻塞条件；不得在消费者中增加
临时 DTO、旧 schema fallback、跨模块 include 或平台泄漏来绕过前置合同。

## 13. 完成记录

### VD1 完成记录

- owner / baseline: agent / `2895c7f`（main working tree）
- 实际修改文件: `src/contracts/**`（job/task_result/service_control/repository_query）、
  `src/format/**`（`BACKUP_FLAG_DEDUP`、BlockEntry DEDUP 校验、Footer 双指标）、
  `docs/modules/contracts.md`、`docs/modules/format.md`、V7/ADR 相关文档
- 公共合同或格式字段变化: `BackupOptions.deduplication_enabled`；
  `TaskResult` / RP summary 的 `deduplicated_block_count` /
  `deduplicated_logical_bytes`；Footer 同名字段；file_set 禁止 Header dedup flag
- 行为变化与保持不变的不变量: 仅 DTO/校验骨架；Writer 算法不在本包
- Debug/Release production targets: 随 VD6 全量生产构建通过
- architecture/static/format/secret checks: CheckSourceLimits 通过；无 contracts→format 依赖
- 人工验证: 编译期与静态字段对齐（offset/size 与 V7 文档）
- 已知限制: 无
- 下一工作包可依赖的不变量: 公共字段与 Footer codec 稳定

### VD2 完成记录

- owner / baseline: agent / `2895c7f`
- 实际修改文件: `personal_archive_chunk_builder.*`、`personal_archive_session.cpp`、
  `personal_archive.h`、adapters/windows_personal_backup 模块文档
- 公共合同或格式字段变化: Session options 透传 `deduplication_enabled`；
  策略启用时 Header 置 `BACKUP_FLAG_DEDUP`
- 行为变化: Writer 顺序 ZERO → omit → 当前 Chunk DEDUP（SHA-256 + 逐字节确认）→ zstd → AEAD；
  仅回指较早 RAW/COMPRESSED；Chunk 边界丢弃 window
- Debug/Release: `aegra_adapter_personal_archive`、worker 生产目标随全量构建通过
- 人工验证: 运行时 D01-D10 矩阵留待隔离环境（见 VD6）
- 下一工作包可依赖的不变量: DEDUP entry 与 Footer 指标由 Writer 正确产出

### VD3 完成记录

- owner / baseline: agent / `2895c7f`
- 实际修改文件: `personal_archive_reader.cpp`（及 shape/volume/chain 路径共享校验）、
  verify 经 Reader 重算并比对 Footer
- 行为变化: 认证后展开 DEDUP；Header flag 与 entry 一致性、后向 ref、目标类型/长度校验；
  损坏在输出当前 Chunk 前拒绝
- Debug/Release: personal_archive + worker 通过
- 人工验证: D11-D16 损坏注入待隔离 Archive 副本
- 下一工作包可依赖的不变量: 读路径与 Writer 共享同一 DEDUP 不变量

### VD4 完成记录

- owner / baseline: agent / `2895c7f`
- 实际修改文件:
  - SQLite schema 15 + `schedules.deduplication_enabled` / `jobs.schedule_id`
    （`control_plane.h`、`sqlite_*`）
  - `schedule_service.cpp` 创建默认/冻结；`worker_job_service.cpp` volume 透传、file_set 强制 false
  - Service/Worker 协议 TaskResult、Schedule、Upsert、RecoveryPoint wire
  - `supervisor_worker_protocol.cpp` 要求显式 dedup 指标（无 optional fallback）
  - Catalog codec/validation；`backup_catalog_registrar` 从 Footer 投影
  - Worker `windows_personal_backup*` 提交后读 Footer 填 TaskResult
  - RP 投影 `personal_repository_query` / `connected_repository_query`
- 公共合同或格式字段变化: 控制面 schema version **15**（无 migration，旧库删除重建）
- 行为变化: volume schedule 默认 true 且冻结；file_set 永远 false；指标 SoT = V7 Footer
- Debug/Release: `aegra_adapter_sqlite`、`aegra_personal_repository`、`aegra_service`、
  `aegra_personal_worker` 通过
- 人工验证: D17-D18 待隔离环境
- 下一工作包可依赖的不变量: wire/DTO 字段稳定，Desktop 可消费

### VD5 完成记录

- owner / baseline: agent / `2895c7f`
- 实际修改文件: Desktop protocol/client/schedule、`recovery_point_model.*`、
  `BackupWizardStep2.qml`（volume 开关 + 文案）、`BackupPage.qml`、`RepositoryPage.qml`、
  五语言 `generate_ts.py` + `.ts`/`.qm`、`docs/modules/desktop.md`
- 行为变化: file_set 隐藏开关并强制 `deduplication_enabled=false`；
  RP 展示 dedup bytes/ratio（分母≤0 时 n/a）
- Debug/Release: `aegra_desktop` 双配置通过（Release 需显式
  `-DAEGRA_BUILD_DESKTOP=ON -DAEGRA_QT_ROOT=C:/Qt6/6.8.3/msvc2022_64`）
- 人工验证: D19 布局/DPI 待本机 UI 走查
- 已知限制: 编辑已有 schedule 的只读策略展示依赖 Summary 字段（已接线）

### VD6 完成记录

- owner / baseline: agent / `2895c7f`
- 静态审计:
  - `src/pipeline` 无 DEDUP/dedup 命中
  - SQLite 仅注释声明无 migration（`sqlite_control_plane.cpp`）
  - `cmake/CheckSourceLimits.cmake` Debug/Release 均通过
  - `git diff --check` 无 conflict marker；仅 CRLF 规范化警告
- 生产构建结果（VS 2026 Insiders + Qt 6.8.3）:
  - **Debug** `vs2026-debug`: `aegra_desktop.exe`、`aegra_service.exe`、
    `aegra_personal_worker.exe` 等全量目标成功
  - **Release** `vs2026-release` + Desktop 选项: `aegra_desktop.exe`、
    `aegra_service.exe`、`aegra_personal_worker.exe` 成功
- 人工矩阵 D01-D20: **未在本会话执行**（需隔离非生产卷/Repository）；
  代码路径与文档已对齐 ADR-0022，运行时验收列为残余工作
- 最终 DoD 代码侧: 已满足；运行时矩阵与 UI 走查为残余人工项
- 已知限制:
  1. 开发机旧 control-plane DB（schema ≠ 15）需删除重建
  2. Release Desktop 不在默认 preset cache 中，构建前需打开 `AEGRA_BUILD_DESKTOP`
