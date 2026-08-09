# 文件集 metadata signature 增量改造开发计划

| 属性 | 内容 |
| --- | --- |
| 状态 | MS1-MS4 已实施；MS5 双配置构建与静态门禁通过，隔离环境人工矩阵待执行 |
| 日期 | 2026-08-09 |
| 决策依据 | [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md) |
| 替代计划 | [旧 USN 增量计划](FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md) 中的 MS 草案 |
| 适用范围 | file_set Full/Incremental Backup、Archive V7、Catalog V2、Service V4、Worker schema 4 |

本文是把 file_set 增量变化判断从 USN Journal 改为同路径普通文件 `write_time + logical_size` 的实施计划。
它面向执行变更的 agent，规定工作包顺序、文件所有权、完成条件和交接格式。架构语义以 ADR-0020、
[增量架构设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md) 和
[V7 格式](../format/PERSONAL_BACKUP_FORMAT_V7.md) 为准；本文不重新定义持久化格式。

## 1. 最终结果

完成全部工作包后，生产代码必须满足：

1. 每次 file_set Backup 完整枚举当前 snapshot namespace，并继续严格检测 reparse、hard link、sparse 和 ADS。
2. Incremental 只按 `(selection_id, relative_path_components, entry_kind)` 查找直接父层 entry。
3. 同路径普通文件仅在 `write_time`、`logical_size` 均相同且父 stream 有效时写 `content_storage=parent`；其它情况写
   whole-file local stream。
4. rename/move、类型变化、新文件和无法确认的父 entry 均写 local stream；删除项从当前完整 tip Index 缺席。
5. Backup 不创建、查询或读取 USN Journal，不保存 journal checkpoint，也不因 journal 状态降级 Full。
6. V7 current schema 使用 `CAP_FILE_METADATA_BASELINE` 和 `change_detection_method=mtime_size_v1`，拒绝旧
   `journal_checkpoints` key。
7. selection fingerprint、父链完整性、Archive 认证和 baseline method 仍决定 Incremental 是否合格。
8. Full、Incremental、Verify、Browse、Restore 和链式解析保持现有语义；只改变变化检测与 baseline 表达。

本次不新增 FAT32 source、非 VSS snapshot source、Differential、内容哈希比较、块级 delta、跨文件 dedup 或 rename
复用。算法不依赖 USN，因此可供未来 FAT32 source 复用；当前 Windows source 支持范围仍以
[Windows 文件集模块文档](../modules/windows_file_set_backup.md) 为准。

## 2. 开工与协作规则

每个 agent 开工前必须完成：

1. 完整阅读根 `AGENTS.md`、`.agents/skills/aegra-cpp-development/SKILL.md`、
   [C++ 工程规范](CPP_ENGINEERING_STANDARD.md)、[模块化架构](../architecture/MODULAR_ARCHITECTURE.md)、
   [模块索引](../modules/README.md)及本包涉及的模块文档。
2. 完整阅读 ADR-0020、V7、Catalog V2、Service V4、增量架构和
   [产品上限与稳定码](FILE_SET_PRODUCT_LIMITS_AND_CODES.md)。ADR-0018 只用于了解待删除的历史实现。
3. 执行 `git status --short` 和 `git rev-parse --short HEAD`，记录基线；不得覆盖用户或其它 agent 的修改。
4. 一次只领取一个工作包，将状态改为 `进行中`，记录 owner、基线和文件所有权。
5. 使用 C++20、RAII、`Result<T>`、有界内存和取消语义；不引入 Windows 类型到 contracts/ports/format/pipeline。
6. 不新增任何测试代码、fixture、测试脚本、测试 executable 或 CTest；按本计划执行生产构建和人工验证。
7. 产品未发布：直接替换 current schema 和枚举，不写 migration、alias、dual-read、fallback 或 feature negotiation。

工作包状态只允许 `等待前置`、`可开始`、`进行中`、`阻塞`、`已完成`。一个工作包只有在实现、生产构建、检查、
人工验证和文档状态全部完成后才能标记 `已完成`。

## 3. 冻结语义

### 3.1 Path key

Planner 使用无损规范 key：

```text
selection_id
+ ordered relative path components (encoding + exact encoded bytes)
+ entry_kind
```

- 不执行 locale 比较，不把 UTF-16LE 路径转为有损 UTF-8，不使用 basename 单独匹配。
- selection fingerprint 已保证选择根和选项相同；`selection_id` 仍必须进入 key，防止多 selection 串项。
- 大小写或路径组件字节变化视为新路径；rename/move 后写 local stream。
- `StableFileIdentity` 继续保存在加密 File Index 中，但不得参与匹配、变化判断或 rename 优化。

### 3.2 普通文件分类

| 当前 entry | 直接父层同 path entry | 条件 | 动作 |
| --- | --- | --- | --- |
| file | 不存在 | 任意 | local whole-file |
| file | 非 file | 任意 | local whole-file |
| file | file | `write_time` 相同且 `logical_size` 相同，父 main stream 有效 | parent stream |
| file | file | 任一 signature 字段不同 | local whole-file |
| directory | 任意 | 任意 | 当前 Index metadata，无 payload |

写 parent stream 时必须保留当前 entry 的 metadata，只把内容引用设为直接父层 `parent_stream_index`。不得用父 entry
覆盖当前 `logical_size` 或其它 metadata。父 Archive/Index 结构损坏是父链不合格；合法父层中某个 path 无匹配或类型
不符只影响该 entry，保守写 local。

### 3.3 已接受风险

内容改变但 `write_time` 和 `logical_size` 均未改变时，Incremental 会引用旧内容。这包括刻意保留 mtime、FAT32
低时间精度、短时间同大小覆盖、时钟或驱动异常。Verify 只能验证已保存链可恢复，不能发现源端这类漏判。实现不得
暗中增加抽样 hash、USN 优先或其它未被 ADR-0020 接受的判断。

### 3.4 Baseline 与降级

- 新增 `contracts::FileChangeDetectionMethod : std::uint8_t`，定义 `kNone = 0` 作为非 file_set/default 哨兵，当前
  file_set 唯一合法值为 `kMtimeSizeV1 = 1`；`is_known_file_change_detection_method()` 负责识别枚举，Manifest
  validator 负责按 content kind 限制 0/1。
- `format::FileSetBaseline` 精确包含 `fingerprint_algorithm`、`selection_fingerprint`、
  `change_detection_method`。
- `CAP_FILE_METADATA_BASELINE` 保持 bit `0x00000004`；删除旧 C++ 名称，不保留 alias。
- Incremental 父候选必须具有相同 selection fingerprint、相同 change detection method 和可认证完整链。
- 保留 `kNoParent=1`、`kSelectionChanged=2`、`kChainIncomplete=3`、`kBaselineInvalid=9` 的既有 wire 值；删除并拒绝
  旧 journal/volume-identity reason 值 4..8，不重新编号，也不生成兼容显示文案。
- metadata baseline 不合格使用 `file_backup.metadata_baseline_invalid` 或
  `IncrementalDowngradeReason::kBaselineInvalid`；credential failure、当前源读取失败和 unsupported object 仍为 hard
  failure。

## 4. 当前实现清单

Agent 不得只按此清单机械替换；开工时必须再次运行 `rg`，因为并行工作可能改变源码。

| 层 | 当前 USN 实现入口 | 目标 |
| --- | --- | --- |
| contracts | `FileJournal*`、`FileChange*`、journal validators、downgrade enum | 删除 USN DTO/校验；增加 method enum |
| ports | `IFileSnapshotView::query_journal_state/read_change_batch` | 删除 journal Port，只保留枚举和 stream reader |
| format | `FileSetBaseline::journal_checkpoints`、`kCapabilityFileUsnBaseline` | 替换为 method 字段和 metadata capability |
| pipeline | `collect_incremental_hints`、identity planner、checkpoint summary | path planner；删除 journal 读取与输出 |
| Windows adapter | `windows_usn_journal.*`、snapshot journal methods | 删除无消费者实现和 CMake source |
| Worker | `prepare_journal`、`query_journal`、`evaluate_usn_eligibility` | 删除；直接按父 baseline 运行 metadata Incremental |
| Archive adapter | incremental checkpoint/capability 校验 | 校验 method；parent stream/chain 语义不变 |
| Service/Repository | baseline 注释、parent eligibility、稳定原因投影 | 按 metadata baseline 语义收口 |
| Desktop | journal downgrade 映射和翻译 | 删除旧文案；保留当前有效原因与风险说明 |

## 5. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 前置 | 默认 owner 范围 |
| --- | --- | --- | --- | --- | --- |
| MS1 | 已完成 | P0 | Path planner 与 Pipeline 去 USN 化 | ADR-0020 | pipeline，Worker 最小编译接线 |
| MS2 | 已完成 | P0 | Current baseline contract、V7 Archive 与 Worker 编排 | MS1 | contracts、format、personal_archive、Worker |
| MS3 | 已完成 | P0 | Contracts/Ports/Windows USN 死代码删除 | MS2 | contracts、ports、windows_filesystem |
| MS4 | 已完成 | P1 | Repository/Service/Desktop 语义收口 | MS2、MS3 | personal_repository、Service、Desktop |
| MS5 | 进行中 | Gate | 双配置构建与静态检查完成；隔离环境人工矩阵待执行 | MS1-MS4 | integration owner |

MS1-MS4 建议串行执行。若多 agent 并行，只能在 integration owner 明确拆分不重叠文件后进行；公共合同、codec、
Worker composition root 和 CMake 文件不得由两个 agent 同时编辑。

## 6. MS1：Path planner 与 Pipeline 去 USN 化

**目标：** 先让数据面只依赖完整当前枚举和直接父层 path Index；旧 USN DTO/Port 暂时可保留为无消费者死代码，
但本包结束时 Pipeline 不得调用它们。

**主要文件：**

- `src/pipeline/include/aegra/pipeline/file_set_change_planner.h`
- `src/pipeline/src/file_set_change_planner.cpp`
- `src/pipeline/include/aegra/pipeline/file_set_backup_pipeline.h`
- `src/pipeline/src/file_set_backup_pipeline.cpp`
- `src/apps/worker/src/windows_file_set_backup.cpp` 中构造 Pipeline plan 的最小接线
- `docs/modules/pipeline.md`

**开发任务：**

1. 从 `FileSetChangePlannerRequest` 删除 `change_hints`；注释和分类名不再出现 USN、identity hint、rename reuse。
2. 从 `FileSetBackupPlan` 删除 `parent_checkpoints`、`change_hints`；从 `FileSetBackupSummary` 删除
   `journal_checkpoints`。
3. 删除 Pipeline 的 checkpoint 校验、current checkpoint 收集、journal continuity 和
   `collect_incremental_hints()`；不得留下“空 hints 视为 unchanged/local”分支。
4. 重写 parent index record，使 key 为完整规范 path，value 至少含 kind、write_time、logical_size、main stream index。
5. 通过父层 `list_children`/`describe_entry` 构建路径。逐层维护 entry-id 到 path 的有界状态；不得假设 entry ID 连续，
   不得只用 `name` 或 stable identity。
6. 保持现有 `parent_index_budget_bytes` 限制。若当前内存结构无法在限制内表达，返回稳定的
   `file_backup.parent_index_budget`，不得无界增长；后续可单独设计 disk spool，不在本变更猜测新持久化格式。
7. 检测重复 path key、父子环、缺失父目录、非法 entry/stream。损坏父 Index 使候选父层不合格；当前枚举损坏使 Job
   failure。
8. 仅当同 path file 的两个 signature 字段相同且父 main stream index 非零时生成 direct-parent stream。
9. 保留完整 tip Index、local payload work list、checked counters、取消检查、确定排序和 Full 全 local 行为。
10. 删除把 parent `logical_size` 回写当前 entry 的行为；当前枚举 metadata 始终权威。

**禁止捷径：** stable identity 匹配、basename 匹配、USN+metadata 双模式、只生成变化 entry、读取父 payload 比较、
任意祖先引用、无界 `unordered_map`。

**构建：** Debug 和 Release 构建 `aegra_pipeline`、`aegra_app_worker_personal`、`aegra_personal_worker`。

**人工检查：** 使用现有生产入口或调试运行验证 Full 全 local；Incremental 的 unchanged、mtime changed、size changed、
new、delete、rename/move、metadata-only、同名不同 selection 和父 stream 无效分支。

**DoD：** `rg` 确认 pipeline 无 `journal`、`USN`、`FileChangeHint`、`StableFileIdentity` 匹配；三个 production target
双配置成功；模块文档同步。

## 7. MS2：Current V7 baseline、Archive 与 Worker 编排

**目标：** current Archive 和 Worker 只生成/接受 metadata baseline，删除所有运行时 journal eligibility。

**主要文件：**

- `src/format/include/aegra/format/manifest.h`
- `src/format/include/aegra/format/personal_archive.h`
- `src/format/src/manifest_codec.cpp`
- `src/format/src/manifest_validation.cpp`
- `src/format/src/personal_archive_codec.cpp`
- `src/adapters/personal_archive/src/personal_file_archive_session.cpp`
- `src/adapters/personal_archive/src/personal_file_archive_chain_reader.cpp`
- `src/apps/worker/src/windows_file_set_backup.h`
- `src/apps/worker/src/windows_file_set_backup.cpp`
- `src/apps/worker/src/windows_file_set_backup_task.cpp`
- V7、Archive Adapter、Worker 模块文档

**开发任务：**

1. 在 contracts 增加 `FileChangeDetectionMethod` 和 validator；`FileSetBaseline` 删除 `journal_checkpoints`，增加
   强类型 `change_detection_method`。默认空 baseline 必须是 algorithm 0、zero digest、`kNone`，file_set writer 显式
   写 algorithm 1、实际 digest、`kMtimeSizeV1`。
2. Manifest codec exact key 改为 `fingerprint_algorithm`、`selection_fingerprint`、
   `change_detection_method`；缺字段、未知值、额外旧 `journal_checkpoints` 均拒绝。
3. capability C++ 常量改名为 `kCapabilityFileMetadataBaseline`，bit 保持不变；Incremental header 要求该 capability。
4. Full/Incremental file_set manifest 都要求合法 fingerprint 和 method；volume_set 的 baseline 必须为默认空状态。
5. chain reader 的 baseline equality 同时比较 fingerprint algorithm、digest 和 method；parent/child method 不同则链无效。
6. Archive session 删除 checkpoint 非空校验，改为 method 校验；parent/local stream、Footer 和发布顺序不变。
7. Worker request 删除 `parent_checkpoints`；task 打开并认证父 Archive 后验证 baseline method，不再提取 checkpoint。
8. 删除 `prepare_journal`、`query_snapshot_journal_checkpoints()`、`evaluate_usn_eligibility()`、相关 stage/log field；
   不调用 `ensure_file_change_journal_active()`。
9. `make_file_manifest()` 固定写 `kMtimeSizeV1`。Service 已降级 Full 时保持原 reason；Worker 不再产生 journal reason。
10. 全面审查 error message、注释、进度字段，禁止路径和凭据进入日志。

**构建：** Debug 和 Release 构建 `aegra_contracts`、`aegra_format`、`aegra_adapter_personal_archive`、
`aegra_app_worker_personal`、`aegra_personal_worker`。

**人工检查：** 新建 Full 和 Incremental Archive，检查 capability、manifest exact keys、parent UUID、parent stream；修改
隔离副本中的 method/旧 key/能力位，确认 current reader 在 payload 或恢复 mutation 前拒绝。

**DoD：** 新 Archive 不含 journal metadata；old current-development Archive 不兼容且明确拒绝；Worker 不查询或创建
journal；四个 production target 双配置成功。

## 8. MS3：Contracts、Ports 与 Windows USN 死代码删除

**目标：** 删除已无生产消费者的 USN 公共合同和 Windows 实现，恢复最小接口面。

**主要文件：**

- `src/contracts/include/aegra/contracts/file_set.h`
- `src/contracts/src/file_set.cpp`
- `src/ports/include/aegra/ports/file_source.h`
- `src/adapters/windows_filesystem/include/aegra/adapters/windows_filesystem/windows_filesystem.h`
- `src/adapters/windows_filesystem/src/windows_file_snapshot_view.cpp`
- 删除 `src/adapters/windows_filesystem/src/windows_usn_journal.h/.cpp`
- `src/adapters/windows_filesystem/CMakeLists.txt`
- contracts、ports、adapters、Windows file_set 模块文档

**开发任务：**

1. 保留并复核 MS2 增加的 `FileChangeDetectionMethod`，确保 contracts 不依赖 format、Windows 或 crypto。
2. 删除 `FileChangeReason`、`FileJournalCheckpoint`、`FileJournalState`、`FileJournalUnavailableReason`、
   `FileChangeHint`、`FileChangeBatch`、相关上限、validator 和 continuity helper。
3. 从 `IFileSnapshotView` 删除 journal 方法和注释；接口只保留 snapshot enumeration 与 stream reading。
4. 删除 Windows snapshot view 的 journal override、`ensure_file_change_journal_active()` 公共函数、USN source 文件和
   CMake 注册。保留 VSS、一致性枚举、stable identity capture 和 unsupported-object 检测。
5. `IncrementalDowngradeReason` known-values 只接受当前有效值 0、1、2、3、9；删除 4..8 枚举项和所有生成点。
6. 在整个 `src` 运行禁用符号搜索，确认没有 USN 类型、journal API、旧 capability 名或 checkpoint 字段。

**构建：** Debug 和 Release 构建 `aegra_contracts`、`aegra_ports`、`aegra_adapter_windows_filesystem`、
`aegra_pipeline`、`aegra_app_worker_personal`。

**DoD：** USN 文件已从源码树和 CMake 删除；核心接口无 dead method；所有直接消费者双配置成功；无兼容 shim。

## 9. MS4：Repository、Service 与 Desktop 语义收口

**目标：** 控制面、Catalog 投影和 UI 只表达 metadata baseline，不显示不可再产生的 journal 状态。

**主要文件：**

- `src/personal_repository/src/parent_selector.cpp`
- `src/personal_repository/src/catalog_validation.cpp`
- `src/apps/service/src/backup_catalog_registrar.cpp`
- `src/apps/service/src/worker_job_service.cpp`
- Service/Worker wire enum 的 codec 与 validator 消费者
- `src/apps/desktop/src/client/models/job_model.*`
- `src/apps/desktop/src/locale/message_code_map.cpp`
- `src/apps/desktop/translations/*.ts`
- Repository、Service、Desktop 和产品稳定码文档

**开发任务：**

1. `file_baseline_available` 只表示成功 RP 具有 current selection fingerprint 与
   `mtime_size_v1` baseline；不能再用“存在 journal checkpoint”描述。
2. parent selector 保持只选最新 tip、不跳祖先；baseline 不合格仍降级 Full，credential failure 仍 hard failure。
3. 删除 Service/Worker/Desktop 对 reason 4..8 的 switch、JSON 合法值、注释、消息映射和五语言翻译；保留 1、2、3、9。
4. 把旧 `file_backup.baseline_invalid` 统一为 `file_backup.metadata_baseline_invalid`；删除 journal stable error code 映射。
5. 审查 SQLite/control-plane numeric projection：current schema 只写合法 reason；不得新增 migration 或旧值兼容。
6. 更新 UI 增量说明，使其准确表达“按修改时间和大小判断”；不得宣称内容哈希级变化检测。已有 FAT32 不支持范围不
   得因本改造被悄悄放宽。

**构建：** Debug 和 Release 构建 `aegra_personal_repository`、`aegra_app_service`、`aegra_service`、
`aegra_desktop`。Desktop 配置使用 `C:/Qt6/6.8.3/msvc2022_64`。

**人工检查：** 创建 Incremental 计划、无父层降级 Full、正常 metadata Incremental、Job summary、Recovery Point
链展示和五语言文案；界面不得出现 USN/journal/file ID。

**DoD：** Catalog/Service/UI 语义与 ADR-0020 一致；不存在 journal 用户文案和死 reason；四个 production target
双配置成功。

## 10. MS5：集成门禁与人工矩阵

**目标：** 证明 current production tree 已整体切换，无旧路径、构建回归或文档冲突。

### 10.1 静态审计

至少执行：

```powershell
rg -n "CAP_FILE_USN_BASELINE|kCapabilityFileUsnBaseline|journal_checkpoints|FileJournal|FileChangeHint|FileChangeReason|query_journal_state|read_change_batch|ensure_file_change_journal_active|file_backup\.journal_" src
rg -n "CAP_FILE_USN_BASELINE|journal_checkpoints|query_journal_state|read_change_batch|file_backup\.journal_" docs
rg -n "StableFileIdentity|stable_identity" src/pipeline
git diff --check
```

第一条在 production source 中应无命中。第二条逐项人工分类：只允许 ADR-0018、旧 FI 历史段落、ADR-0020 的替代
说明和本计划的删除清单命中，任何 current schema/behavior 声明命中都必须修复。第三条只允许数据保留/校验，不允许
planner 匹配。另执行仓库已有 source limit、架构、格式、秘密和文档链接检查。

### 10.2 生产构建

使用 Visual Studio 2026 Insiders。先配置 `vs2026-debug`、`vs2026-release`，再完成两个配置的全量生产构建；Desktop
启用 `AEGRA_BUILD_DESKTOP=ON` 并指定 Qt 6.8.3。至少记录下列 target 结果：

```text
aegra_contracts
aegra_format
aegra_pipeline
aegra_personal_repository
aegra_adapter_personal_archive
aegra_adapter_windows_filesystem
aegra_app_worker_personal
aegra_personal_worker
aegra_app_service
aegra_service
aegra_desktop
```

运行：

```powershell
cmake -DAEGRA_SOURCE_ROOT=D:/Work/OpenSource/Aegra -P cmake/CheckSourceLimits.cmake
```

### 10.3 人工验证矩阵

| ID | 场景 | 期望 |
| --- | --- | --- |
| M01 | Full 普通目录/文件 | 全部 file stream 为 local，可 Verify/Restore |
| M02 | Full 后无变化 Incremental | 同 path file 全部 parent，tip Index 完整 |
| M03 | 内容和 mtime 改变、大小不变 | changed file 为 local |
| M04 | 大小改变 | changed file 为 local |
| M05 | 仅 ACL/attributes 改变，mtime/size 不变 | 当前 metadata 更新，payload 为 parent |
| M06 | rename 或跨目录 move | 新 path 为 local，旧 path 从 tip 缺席 |
| M07 | 新建与删除 | 新文件 local，删除项缺席 |
| M08 | 删除后同 path 重建，mtime/size 不同 | local |
| M09 | 内容改变但强制恢复相同 mtime/size | parent；记录为 ADR-0020 已接受风险 |
| M10 | Journal 不存在/禁用 | 不影响 Incremental，不创建或查询 journal |
| M11 | selection fingerprint 改变 | effective Full + selection changed |
| M12 | 父链缺卷、损坏或 method 非 1 | effective Full/稳定 baseline reason；不跳旧祖先 |
| M13 | 父凭据错误 | hard failure，不降级 Full |
| M14 | reparse/hard link/sparse/ADS | stable strict failure，无可见 Recovery Point |
| M15 | Full→Inc→Inc chain Verify/Browse/Restore | tip namespace 与内容解析正确 |
| M16 | 取消、目标满、spool budget 超限 | 有界失败并清理 partial，不发布 Catalog |

若当前产品仍不支持 FAT32 snapshot source，不伪造 FAT32 成功结果；交付记录必须写明“变化算法已去 USN 化，但本次未
增加 FAT32 source 能力”。

### 10.4 最终 DoD

- MS1-MS4 全部完成并有 owner、基线、变更文件、构建和人工验证记录。
- Debug/Release 全量 production build 成功，Desktop 双配置成功。
- source limit、架构、格式、秘密、文档链接和 `git diff --check` 通过。
- current source/format/protocol/UI 无 USN 增量依赖、旧字段、旧 capability 名、旧稳定码或兼容分支。
- V7/Catalog V2/Service V4/Worker schema 4 的实现与现行文档一致。
- 不新增任何测试资产，不提交构建产物、隔离 Archive、凭据或本机配置。

## 11. Agent 交接模板

每个工作包完成后在对应章节末尾追加：

```text
### MSx 完成记录

- owner / baseline:
- 实际修改文件:
- 删除的旧字段、类型和分支:
- 行为变化与保持不变的不变量:
- Debug production targets / 结果:
- Release production targets / 结果:
- architecture/static/format/secret checks / 结果:
- 人工验证场景、环境与结果:
- 已知限制或阻塞:
- 下一工作包可依赖的不变量:
```

不得用“代码已写完”“本地看起来正常”代替构建命令、结果和人工场景。若工作包阻塞，记录重复可复现的阻塞条件，
不要在消费者中增加临时 DTO、旧 schema fallback 或平台泄漏来绕过前置合同。

## 12. 本次实施记录（2026-08-09）

- owner / baseline：Codex integration owner；`c55301b`。
- MS1：Pipeline 已改为有界的直接父层 path index；key 为 selection、规范路径组件和 kind，文件仅在
  `write_time + logical_size` 相同时复用 direct-parent main stream；Full 全 local。
- MS2：V7 baseline current exact keys 为 `fingerprint_algorithm`、`selection_fingerprint`、
  `change_detection_method`；capability 常量改为 metadata baseline；Worker 删除 journal eligibility 并验证父 method。
- MS3：删除 journal contracts/validators、source Port、Windows USN reader 与 CMake source；reason 仅接受 0、1、2、3、9。
- MS4：Catalog/Service/Desktop 文案统一为 metadata baseline，删除 reason 4..8 和 journal message 映射，五语言 TS
  由 `generate_ts.py` 重建。
- Debug production build：`cmd.exe /d /c scripts\\build.cmd Debug`，全量成功，包含 Worker、Service、Desktop。
- Release production build：`cmd.exe /d /c scripts\\build.cmd Release`，全量成功，包含 Worker、Service、Desktop。
- 静态检查：禁用符号 source 扫描无命中；Pipeline stable identity/USN/journal 扫描无命中；
  `CheckSourceLimits.cmake` 与 `git diff --check` 通过。
- 人工验证：本机未配置隔离 Repository、可变更 VSS source 和专用凭据，M01-M16 未执行，MS5 保持进行中；不得把
  production source 或现有用户数据当作破坏性验证夹具。
- 已知限制：变化算法已去 USN 化，但本次未增加 FAT32 snapshot source 能力；同大小同 mtime 的内容变化仍是
  ADR-0020 明确接受的漏检风险。
