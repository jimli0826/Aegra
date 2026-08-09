# 文件集增量备份分阶段开发计划

> 本计划的 FI0-FI10 部分记录历史 USN 实施。现行实施依据是
> [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md) 和
> [增量架构设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md)。
> 仓库禁止新增任何测试源码、fixture、测试脚本、测试 executable、CTest 或其它项目测试资产。
>
> **设计变更 2026-08-09**：file_set Incremental 改为同路径普通文件 `write_time + logical_size`
> metadata signature 判断变化；USN baseline、journal checkpoints 和 journal continuity 不再是 current design。
> 本次改造的 agent 可执行步骤、文件所有权和验收矩阵见
> [metadata signature 增量改造开发计划](FILE_SET_METADATA_SIGNATURE_DEVELOPMENT_PLAN.md)。本文件中的 MS 表仅保留
> 路线摘要，FI0-FI10 仅保留历史实施记录。

## 1. Agent 开工规则

每个工作包 owner 开工前必须：

1. 阅读根 `AGENTS.md`、`.agents/skills/aegra-cpp-development/SKILL.md`、C++ 工程规范、模块架构；
2. 阅读 ADR-0018、增量架构、V7、Catalog V2、产品上限及本包涉及的全部模块文档；
3. 执行 `git status --short`，记录基线 commit，不覆盖用户或其它 agent 的变更；
4. 在本表将一个包标为“进行中”，写明 owner、基线和文件所有权；
5. 先修改权威 contract/format，再同步全部生产消费者；不保留过渡 alias、adapter shim 或 dual-read；
6. 严守本包允许文件，公共头、根 CMake、codec、schema 和 composition root 只由 integration owner 修改；
7. 完成实现、production build、静态/架构检查、人工验证、文档与状态更新后再交接。

一个 agent 一次只领取一个工作包或表中明确的子包。并行 owner 不得编辑同一文件。遇到跨包合同缺口时停止本包，
先交由前置包 owner 修订，不在消费者中复制临时 DTO 或解析器。

## 2. 全局硬约束

- 产品未发布：直接修改 Archive V7、Catalog V2、Service V4、Worker schema 4 和当前 SQLite schema；删除旧字段及
  分支，不实现 migration、conversion、alias、fallback、dual-read、feature negotiation 或开发 Archive 修复工具。
- 本期只支持 `file_set + full|incremental + local NTFS/ReFS + VSS + unnamed main stream`；不支持 Differential。
- reparse、hard link、sparse、ADS 在 Backup strict fail，在 Restore 第一次 mutation 前拒绝。
- 使用同路径普通文件 `write_time + logical_size` 判断 payload 是否可复用；同大小同 mtime 内容变化是已接受风险。
- selection fingerprint、父链、metadata baseline 不可证明时 effective Full；credential failure、源不可读和不支持对象仍是 Job failure。
- tip File Index 是完整当前树；不能把增量实现成只含变化 entry 的 event log。
- changed file 整文件写入；禁止 block delta、跨文件 dedup、任意祖先引用和 `.bhx` 复用。
- 文件树、父 path 索引、payload 和 page 处理均有 byte/count 上限、取消和背压；禁止全树内存 map。
- `pipeline` 不 include Windows、Archive Adapter、Repository、SQLite 或 Qt；Adapter 之间不直接依赖实现。
- 客户路径、名称和 security descriptor 不进入 Catalog、普通日志或 TaskResult arguments。

## 2.1 Metadata Signature 改造路线摘要（历史编号）

详细任务、禁止事项、逐包构建和人工验证以
[独立 MS 开发计划](FILE_SET_METADATA_SIGNATURE_DEVELOPMENT_PLAN.md) 为唯一执行依据；下表是设计切换时留下的路线
摘要，其编号、顺序和完成状态不得用于 agent 派工。

| ID | 状态 | 优先级 | 工作包 | 前置 |
| --- | --- | --- | --- | --- |
| MS1 | 已完成 | P0 | 合同/格式重命名：`CAP_FILE_METADATA_BASELINE`、`change_detection_method`，移除 current metadata `journal_checkpoints` | ADR-0020 |
| MS2 | 已完成 | P0 | Pipeline planner：parent path index + `write_time/logical_size` 比较，删除 USN hints | MS1 |
| MS3 | 已完成 | P0 | Worker/Service 编排：不再查询 journal eligibility，不再生成 journal downgrade reason | MS1、MS2 |
| MS4 | 已完成 | P1 | Adapter 清理：USN Port 和 Windows USN reader 已从 current source 删除 | MS3 |
| MS5 | 进行中 | Gate | 双配置生产构建与静态检查已通过；隔离 VSS/Archive 人工矩阵待执行 | MS1–MS4 |

## 3. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 前置 | 可并行关系 |
| --- | --- | --- | --- | --- | --- |
| FI0 | 已完成 | Gate | 范围收缩与现行合同清理 | ADR-0018 | 无 |
| FI1 | 历史完成 | P0 | USN baseline、file identity 与增量格式合同（已被 MS1 替换） | FI0 | 无 |
| FI2 | 历史完成 | P0 | Windows USN source 与不支持对象检测（USN source 已由 MS4 删除） | FI1 | FI3（合同冻结后） |
| FI3 | 已完成 | P0 | Parent Index comparator 与 change planner | FI1 | FI2 |
| FI4 | 已完成 | P0 | Incremental Archive writer 与 parent stream | FI1、FI3 | FI2 |
| FI5 | 已完成 | P0 | File chain reader 与 chain Verify | FI4 | FI6（接口冻结后） |
| FI6 | 已完成 | P1 | Repository/Catalog 选父、降级 Full、删除/保留 | FI1、FI4 | FI5 |
| FI7 | 已完成 | P1 | Worker/Service/SQLite 计划任务编排 | FI2、FI3、FI4、FI6 | 无 |
| FI8 | 已完成 | P1 | Browse/Restore 接入 chain reader | FI5、FI7 | 无 |
| FI9 | 已完成 | P2 | Desktop file Incremental UX | FI7、FI8 | 无 |
| FI10 | 已完成 | Gate | 全量构建、人工矩阵、文档与发布门禁 | FI0–FI9 | 无 |

状态只允许：`可开始`、`进行中`、`等待前置`、`阻塞`、`已完成`。不得以“代码已提交”代替完成标准。

## 4. FI0：范围收缩与现行合同清理

**目标：** 先让当前 Full 路径诚实地只支持 directory、regular file 和 unnamed main stream，消除 silent loss 与
虚假 sink capability，再开展增量。

**前置：** ADR-0018 Accepted。

**主要文件所有权：**

- `docs/format/PERSONAL_BACKUP_FORMAT_V7.md`
- `src/contracts/include/aegra/contracts/file_set.h` 及实现
- `src/ports/include/aegra/ports/file_source.h`
- `src/ports/include/aegra/ports/file_sink.h`
- `src/ports/include/aegra/ports/file_backup_session.h`
- `src/ports/include/aegra/ports/file_recovery_point.h`
- `src/pipeline/src/file_set_backup_pipeline.cpp`
- `src/pipeline/src/file_set_restore_pipeline.cpp`
- `src/adapters/personal_archive/**`
- `src/adapters/windows_filesystem/**`
- 对应模块文档与 CMake（仅确有源文件拆分时）

**开发任务：**

- 从 current-format model/codec 删除 `kReparse`、alternate stream、hard-link group、allocated ranges、sparse flags、
  reparse platform metadata tag 和相关 limit；同步所有 switch/visitor/serializer/parser。
- 把 File Index exact schema 收敛为 root/directory/regular file + one unnamed main stream。
- 从 Source/Sink Port 删除 reparse/hard-link/sparse/ADS 方法与 capability；不得保留永远为 false 的 future stub。
- 删除 Backup/Restore Pipeline 中 materialize hard link、sparse hole、alternate stream、reparse object 分支。
- Windows source 在完整枚举期间实现四类精确检测及稳定错误映射。
- `WindowsFileTreeSink::capabilities` 不再宣称未实现能力；Restore preflight 加 defense-in-depth 拒绝。
- 更新产品上限和人工矩阵：原 S05–S08 成功场景改为 Backup/Restore 拒绝场景。
- 搜索生产树，确认不存在旧字段、旧 enum、虚假 `supports_*` 或 lossy path。

**禁止捷径：** 仅在 UI 隐藏选项、仅在文档声明不支持、跳过 stream enumeration、把 sparse 当 dense、把 hard link
当独立文件、follow reparse 或只读 unnamed stream。

**Production targets：** `aegra_contracts`、`aegra_ports`、`aegra_format_personal_archive`、`aegra_pipeline`、
`aegra_adapter_personal_archive`、`aegra_adapter_windows_filesystem`、`aegra_app_worker_personal`、
`aegra_personal_worker`、Service、Desktop Debug/Release 直接消费者。

**人工验证：** 普通文件/目录 Full Backup、Verify、Restore 正常；分别创建 reparse、两名称 hard link、sparse、ADS，
确认 Backup stable strict fail 且无可见 RP；在隔离复制的开发 Archive 注入旧字段，确认 Restore 写前拒绝。

**DoD：** 四类对象没有可写 current-format 表达、没有 Source/Sink 能力、没有恢复分支；普通 Full 行为构建和人工验证
通过；无兼容代码。

## 5. FI1：USN baseline、file identity 与增量格式合同

**目标：** 冻结并实现平台无关合同和 current V7/Catalog V2 codec，使后续 Adapter/Pipeline 不再猜字段。

**前置：** FI0。

**主要文件所有权：** `contracts/file_set*`、`contracts/job*`、`ports/file_source*`、
`ports/file_backup_session*`、`ports/file_recovery_point*`、`format/personal_archive/**`、
`PERSONAL_BACKUP_FORMAT_V7.md`、`PERSONAL_REPOSITORY_FORMAT_V2.md`、contracts/ports/format 模块文档。

**开发任务：**

- 定义 `StableFileIdentity`、`FileJournalCheckpoint`、`FileChangeReason`、`FileChangeBatch` 和固定上限。
- 定义 selection fingerprint 算法输入的规范编码、算法 ID、输出长度与排序；实现共享纯 codec/hash helper。
- 扩展 source Port：查询 snapshot journal state、从指定 USN 读取有界 batch；写清所有权、短读、取消和错误语义。
- 扩展 entry descriptor 保存 stable identity；目录和普通文件必填，root 使用明确定义的 null identity。
- 定义 stream `content_storage=local|parent` tagged union；parent 只携带 direct `parent_stream_index`。
- V7 file_set 允许 Full/Incremental，增加 `CAP_FILE_USN_BASELINE`、metadata checkpoints/fingerprint 和拒绝规则。
- Catalog V2 exact keys 直接增加 `file_selection_fingerprint`、`file_baseline_available`，允许 file parent/type 组合。
- Job/Result contract 增加 requested/effective type、candidate/effective parent 与 downgrade enum；同步 validators。
- 所有 codec 做 duplicate key、enum、count、排序、length、overflow、unknown critical 和非法组合拒绝。

**禁止捷径：** 暴露 `HANDLE`、`USN_RECORD`、Windows flags、SQLite、JSON 或 physical Archive offset 到 Port；用字符串
reason；保存任意 ancestor UUID；保留旧 Catalog exact key fallback。

**Production targets：** contracts、ports、format、personal archive codec、personal repository 及所有直接生产消费者。

**人工验证：** 通过生产 codec/CLI 入口生成 Full/Incremental 隔离 Archive；手工翻转 parent、capability、checkpoint、
fingerprint、content_storage 和 parent stream index，确认 current reader 拒绝；旧开发 Archive 统一 unsupported/corrupt。

**DoD：** 合同文档逐字段明确，核心层无平台类型，writer/reader 对合法组合对称，所有消费者只使用当前 schema。

## 6. FI2：Windows USN source 与不支持对象检测

**目标：** 从同一 VSS snapshot view 提供可靠 journal state/records，并完成每次完整枚举的四类对象检测。

**前置：** FI1 合同冻结。

**主要文件所有权：** `src/adapters/windows_filesystem/**`、必要的 `src/apps/worker` snapshot mapping 接口、
`docs/modules/adapters.md`、`docs/modules/windows_file_set_backup.md`。

**开发任务：**

- 为 volume/snapshot handle 建立 RAII journal reader；查询 journal ID、lowest valid USN、next USN。
- 证明 journal IO 来自 snapshot-consistent view；若 Windows API 在 snapshot 上不支持，返回明确 unavailable 供 Full
  downgrade，不得读取 live volume 代替。
- 按 `[start_usn,end_usn)` 读取有界 record batch，支持 V2/V3 record 或明确拒绝未知版本；验证 record length/alignment。
- 将 Windows reason mask 映射为平台无关 content/namespace/metadata/ambiguous reason，未知 bit 保守 ambiguous。
- 提供 `(volume_identity, FILE_ID_128)`；使用 create/delete record 和当前 handle identity 防止 ID reuse。
- 在 Full/Incremental 完整枚举中检测 reparse、link count、sparse 和额外 `$DATA` stream；稳定码不得含路径。
- 所有 handle、buffer、privilege 和 cancellation path 使用 RAII；不持锁执行阻塞 IO。

**禁止捷径：** live journal + snapshot tree 混合、只检查 file attributes 而不枚举 ADS、用 path 匹配 USN、忽略未知
record/reason、因为 parent 标记 unchanged 就跳过 unsupported detection。

**Production targets：** `aegra_adapter_windows_filesystem`、Worker file-set composition targets。

**人工验证：** NTFS 上记录 create/write/truncate/rename/move/delete/security/basic-info；VSS 后读取固定区间；禁用/删除/
重建 journal 和制造区间过旧，核对状态；ReFS 能力不足时明确 Full downgrade；四类对象 strict fail。

**DoD：** Adapter 只实现 Port、snapshot consistency 有文档证据、所有 buffer/range 有界、错误分类足以驱动 FI7。

## 7. FI3：Parent Index comparator 与 change planner

**目标：** 用 parent Index、完整 current enumeration 和 USN hints 生成有界、确定的 current entry plan。

**前置：** FI1；FI2 可并行，实现时使用已冻结 Port。

**主要文件所有权：** `src/pipeline/include/aegra/pipeline/file_set_*`、`src/pipeline/src/file_set_*`、
必要的新 planner translation unit、`docs/modules/pipeline.md`。

**开发任务：**

- 定义 planner 状态机：ingest journal hints → enumerate current → parent identity lookup → classify → emit plan。
- 用有界 batch、page lookup 或 disk spool 关联 stable identity；禁止将全部 parent/current entry 放入内存 map。
- 实现分类表：new/content/metadata-only/rename-move/unchanged/ambiguous/deleted。
- 对 new/content/ambiguous 输出 local whole-file stream；metadata/rename/unchanged 输出 direct-parent stream。
- current File Index 始终输出全部当前 entry；deleted entry 不输出；父子与 stable identity 唯一性逐批验证。
- size 变化强制 local；相同 size/mtime 不改变 USN 结论；身份/kind 不一致按 new。
- deterministic entry/stream ordering、checked totals、spool budget、取消和 backpressure。
- 将 downgrade eligibility 与 entry classification 分离：eligibility 未通过时直接走现有 Full planner。

**禁止捷径：** timestamp-only、USN-only namespace、变化 entry-only Index、任意祖先查找、读取父 payload 比较、block delta。

**Production targets：** `aegra_pipeline` 及直接 Worker production consumer。

**人工验证：** 在隔离树比较 planner 输出与当前 snapshot：无变化、内容变化、相同大小 overwrite、metadata-only、
rename、跨目录 move、delete/recreate same name、file ID reuse、多卷选择、取消和 spool budget。

**DoD：** 同输入产生确定计划，歧义永远 local，内存与队列有界，Full planner 未回归。

## 8. FI4：Incremental Archive writer 与 parent stream

**目标：** 写出完整 current Index、local changed payload 和可认证 direct-parent stream reference。

**前置：** FI1、FI3。

**主要文件所有权：** `src/adapters/personal_archive/**`、相关 format codec、Archive/session 模块文档。

**开发任务：**

- file session 接收 effective type、parent identity、fingerprint、journal checkpoints 和 planned stream storage。
- local stream 沿用 bounded chunk write；parent stream 不写 payload，只写 exact parent reference。
- Index spool 支持 stable identity、local/parent union 和完整当前 namespace；保持 page plain-size/count/depth 上限。
- finalize 前验证 Full 不含 parent stream，Incremental parent UUID 非零且所有 parent reference 语法有效。
- Footer counts 区分 current entry/stream、local stream/chunk/logical bytes；统计定义同步文档/UI。
- split Archive 仍以完整 record 边界分卷，续卷/首卷发布顺序不变；file_set 不生成 sidecar。
- Abort/异常/取消清理 partial 与 spool；不能先发布 Catalog。

**禁止捷径：** 把 parent payload 复制进当前层仍标 incremental、引用 arbitrary layer UUID、空 extent 暗示 parent、复用
volume sidecar、把全 Index 放入 metadata envelope。

**Production targets：** format、personal archive Adapter、pipeline/Worker 直接消费者。

**人工验证：** Full→Inc 无变化（无 local payload）、单文件变化、全文件变化、分卷、加密、取消、spool 满、目标满；
检查 tip Index 完整、parent ref 指向直接父层且首卷最后可见。

**DoD：** current V7 reader/writer 合同一致，Archive 可结构扫描，invalid ref 在 commit 前拒绝，无旧格式路径。

## 9. FI5：File chain reader 与 chain Verify

**目标：** 为 browse/Verify/Restore 提供一个认证完整链、以 tip Index 查询并递归解析内容的统一 reader。

**前置：** FI4。

**主要文件所有权：** `src/ports/include/aegra/ports/file_recovery_point.h`、
`src/adapters/personal_archive/**`、`src/personal_repository/**` 中只读 chain open、Verify application/worker、模块文档。

**开发任务：**

- 定义 chain open request/result、layer lifetime、credential lifetime、线程安全和 cancellation contract。
- 打开 tip→Full 链，验证 repository/backup set/content kind/parent relation/fingerprint/深度/无环/generation。
- directory browse 和 entry lookup 只暴露 tip Index；不合并各层 namespace。
- local/parent resolver 逐层验证 stable identity、stream kind、logical size 和 exact parent stream index。
- 维护 visited key 与最大 128 层；禁止 recursion overflow、悬空引用和跨链访问。
- range read 只在完整解析后读取最终 local payload；认证/解压失败不返回数据。
- 用户 Verify 验证每层所有 page/local payload，并解析 tip 全部 stream；输出 layer/entry/byte 计数。

**禁止捷径：** Verify 只检查 tip、browse 合并历史 entry、缺父时返回空文件、只验证被恢复的一部分 payload却报告完整 Verify。

**Production targets：** ports、personal archive Adapter、personal repository、Worker Verify、Service Verify consumer。

**人工验证：** 1/2/128 层、重复 parent、循环、缺父、跨 backup set、错误 fingerprint、坏 parent stream、坏 payload、
错误 credential、取消；无变化 stream 从根层读取一致。

**DoD：** 所有 file Recovery Point consumer 有唯一 chain API；用户 Verify 表示可恢复性而非单文件结构健康。

## 10. FI6：Repository/Catalog 选父、降级 Full、删除与保留

**目标：** 让 Catalog graph、scanner、父层选择和 lifecycle 正确理解 file chain。

**前置：** FI1、FI4；可在 FI5 接口冻结后并行。

**主要文件所有权：** `src/personal_repository/**`、Catalog Adapter/codec、application repository use cases、
`PERSONAL_REPOSITORY_FORMAT_V2.md`、`docs/modules/personal_repository.md`。

**开发任务：**

- Catalog V2 reader/writer/scanner 同步新 exact keys 和 file parent/type 组合；旧开发 Entry 统一拒绝。
- graph validator 对 volume/file 使用各自规则；file chain 同 backup set/fingerprint、Full root、max depth、无分叉歧义。
- parent selector 只选择最新完整 tip；结构不合格返回稳定 downgrade reason，不向后跳选旧祖先。
- 无 credential scan 只重建结构；认证后补全 fingerprint/baseline，未认证候选不可用于新 Incremental。
- deletion planner 阻止删除有保留后代的层；整链删除后代到祖先；tombstone member 不含 `.bhx`。
- retention 计算 retained tip 的 ancestor closure；达到策略触发点时请求新 Full，不实现 chain rewrite。
- Catalog 发布冲突、scanner rebuild 和 generation race 保持现有原子语义。

**禁止捷径：** file chain 套用 volume geometry/sidecar、Catalog 取代 Archive fingerprint 权威、缺父自动断链当 Full、
删除祖先后让后代显示 corrupt。

**Production targets：** personal repository、application、Service/Worker 直接 consumers。

**人工验证：** Catalog rebuild、无凭据/有凭据扫描、缺父/冲突/fingerprint mismatch、保留最近 N、拒绝单删祖先、
整链删除、Catalog publish 失败恢复。

**DoD：** graph/selector/deletion/retention 对 file chain 有明确 production 行为，Catalog 不泄露客户树数据。

## 11. FI7：Worker、Service、SQLite 与计划任务编排

**目标：** Schedule 到 Worker 端到端执行 requested Incremental，并持久化 effective result 和 downgrade reason。

**前置：** FI2、FI3、FI4、FI6。

**主要文件所有权：** `src/apps/worker/**`、`src/apps/service/**`、application backup use cases、
`src/adapters/control_plane_sqlite/**`、Service V4/Worker schema codec、对应模块/协议文档。

**开发任务：**

- Service 允许 file Schedule `full|incremental`，创建时生成/freeze backup set 与 selection fingerprint。
- 当前 SQLite schema 直接增加 requested/effective type、candidate/effective parent、downgrade reason；不写 migration。
- scheduler 每次运行调用 parent selector，构造严格 Worker schema 4 payload；Job retry 固定 request identity/语义。
- Worker 先解析 credential、打开父 metadata，再创建一个 VSS Snapshot Set；验证 USN 后选择 Incremental/Full pipeline。
- eligibility downgrade 产生 structured progress/event；credential、unsupported object、unreadable source 保持 failure。
- effective Full 使用同一 Schedule backup set、`parent_uuid=0`，成功后成为后续父候选。
- TaskResult、Job projection、ListRecoveryPoints/Jobs/Schedules 同步 current exact schema；不暴露路径/file ID/USN。
- 明确 crash windows：Archive visible/Catalog missing、Catalog published/SQLite pending 时沿既有 reconcile 收敛。

**禁止捷径：** Service 自行读 USN/Archive payload、Worker 查询 SQLite、请求 Incremental却始终写 Full且不记录原因、
父凭据失败时静默 Full、schema optional alias。

**Production targets：** contracts/application/control-plane SQLite/Service/Worker 全部 personal production targets。

**人工验证：** 手动 Start 与 scheduler run；无父、连续 journal、reset/wrap、selection 新 backup set、凭据错误、取消、
deadline、Worker crash、Service restart、Catalog publish race；核对 requested/effective/result/UI projection。

**DoD：** 计划任务纵向闭环可运行，安全 Full 有明确可观察原因，所有状态在重启后收敛且无兼容 schema。

## 12. FI8：Browse、Restore 与 chain reader 集成

**目标：** 所有文件树查询和选择性恢复都以 tip Index + chain reader 工作。

**前置：** FI5、FI7。

**主要文件所有权：** application recover use cases、Service Recovery Point handlers、Worker restore composition、
file restore pipeline 接线、相关模块/协议文档。

**开发任务：**

- `ListRecoveryPointEntries` 打开认证 chain，分页查询 tip Index；token 绑定 tip root digest 与完整 chain generation digest。
- `PrepareFileRestore` 解析选择闭包，并在写前解析全部 selected parent streams 到 local owner。
- preflight token 增加 chain generation digest；Start 重查任一层 generation 变化。
- Worker restore 注入 chain reader，而不是单 Archive reader；pipeline 仍只依赖 recovery Port。
- chain/parent/reference 错误映射为稳定 file_recover code；写前失败不产生 target mutation。
- defense-in-depth 扫描四类 unsupported semantics；不依赖 Windows Sink capability 去做有损恢复。
- 结果统计按 tip logical bytes 与实际读取 layer bytes 分开，避免进度回退或重复计数。

**禁止捷径：** query 历史层合并树、Start 时只验证 tip generation、缺父内容写零、恢复时临时转 Full。

**Production targets：** application、Service、Worker restore、pipeline/ports 直接 consumers。

**人工验证：** 多层 tip browse、分页 token、从根/中间/当前层解析内容、目录选择、冲突策略、目标满、preflight 后删除/
替换父层、坏链写前拒绝、partial restore 统计。

**DoD：** browse/restore 不再有单层 file reader 路径，所有 selected bytes 来自已认证 chain。

## 13. FI9：Desktop file Incremental UX

**目标：** 用户可配置 file Incremental 计划并理解实际产生的 Full/Incremental，不暴露内部 USN 细节或路径。

**前置：** FI7、FI8。

**主要文件所有权：** `src/apps/desktop/**`、QML 页面、Desktop 模块文档与翻译资源。

**开发任务：**

- file schedule backup type 提供 Full/Incremental；不显示 Differential。
- 创建/编辑时说明选择范围变化会建立新 Full 基线；不提供兼容旧 Schedule 的迁移 UI。
- Job 与 Recovery Point 列表分别显示 requested/effective type；downgrade reason 使用稳定本地化文案。
- details 展示链深度、父 RP 时间/ID 的安全摘要和 baseline availability，不显示 file ID、USN 或内部 object key。
- 删除 UI 根据 server chain-aware plan 展示会删除/保留的 RP 数，不自行计算依赖。
- Verify/Restore 在缺父、凭据错误、chain corrupt 时给明确可执行错误；不建议有损恢复。
- 四类 unsupported source 错误映射为准确文案，不暗示“已跳过”。

**禁止捷径：** Desktop 直接枚举文件、读取 Archive/Catalog、推断链、展示路径型错误参数、用 warning 掩盖 effective Full。

**Production targets：** Desktop Debug/Release；Service protocol consumer 同步构建。

**人工验证：** 100%/125%/150% scaling，多语言、窄窗口；创建计划、手动运行、downgrade、链删除、Verify、Restore；
所有动态文本不重叠，按钮状态与 Service capability 一致。

**DoD：** file Incremental 从配置到结果可见，用户能区分请求类型与实际类型，UI 不承担权威判断。

## 14. FI10：发布门禁与文档收口

**目标：** 用生产构建、静态/架构检查和完整人工矩阵证明功能闭环，清除临时状态。

**前置：** FI0–FI9 全部完成。

**允许文件：** 文档、构建修复所需对应 production 文件；不得新增测试资产。

**执行任务：**

- 使用 VS 2026 Insiders 和 Qt 6.8.3 完成 Windows Debug/Release 全部 production build。
- 运行仓库 architecture/static/format/secret checks、`git diff --check` 和文档链接检查。
- 搜索确认无 reparse/hard-link/sparse/ADS 写入/恢复路径、虚假 capability、V6/V7 dual-read、Catalog alias、SQLite
  migration、Service/Worker schema fallback 或 ADR-0020 以外的旧 timestamp/USN 双路径。
- 执行 §15 人工矩阵，记录环境、命令、结果、日志 code 与未提交隔离数据位置；验证后删除隔离数据。
- 更新 ADR/architecture/format/protocol/module/product/status 文档，工作包全部标为已完成。
- 对 production functions/files 运行工程规模检查；拆分超限代码，不以注释豁免。

**DoD：** 所有 production build/check 通过，人工矩阵无 P0/P1 缺口，文档无冲突，工作树只含预期修改，没有
测试资产或兼容路径。

## 15. 人工验证矩阵

所有样本仅在隔离临时目录或非生产 Volume 上创建，验证后删除。

### 15.1 成功链

| ID | 场景 | 期望 |
| --- | --- | --- |
| I01 | 首次请求 Incremental | effective Full、parent null、reason no_parent |
| I02 | 无变化 Full→Inc | 完整 tip tree，无 local file payload，内容从父层读取 |
| I03 | create/overwrite/extend/truncate | changed file 整流 local，其它 parent |
| I04 | metadata/security only | 当前 metadata + parent content |
| I05 | rename/move | tip 仅新层级，新路径按 local stream 写入 |
| I06 | delete | tip 不含 entry，旧 RP 仍可浏览/恢复 |
| I07 | delete+recreate same name | mtime/size 任一不同则 local；若同 mtime+同大小则按 ADR-0020 风险接受可能复用 parent |
| I08 | 连续 3/16/128 层 | browse/Verify/Restore 一致，深度上限明确 |
| I09 | 多 selection/多 volume | 同一 VSS set，按各 selection-relative path 比较 metadata signature |
| I10 | 加密+分卷 incremental | 跨 part page/payload/parent resolution 正确 |
| I11 | scheduler restart | 下一次仍选正确 tip，requested/effective 状态可重建 |
| I12 | Catalog rebuild | 结构链重建；认证后恢复 fingerprint/baseline 投影 |

### 15.2 安全 Full downgrade

| ID | 场景 | 期望 |
| --- | --- | --- |
| D01 | parent 无 metadata baseline / method 不匹配 | effective Full + baseline reason |
| D02 | selection fingerprint 改变 | 新 baseline Full，不接旧链 |
| D03 | parent structurally missing/corrupt | effective Full + chain reason；不跳选旧父 |
| D04 | volume identity 改变 | effective Full + volume_identity_changed |
| D05 | parent credential wrong | Job failed credential；不得 downgrade |
| D06 | 同大小同 mtime 内容变化 | 按 ADR-0020 风险接受：判定未变并引用 parent stream |

### 15.3 不支持对象

| ID | 场景 | 期望 |
| --- | --- | --- |
| U01 | selection 含 symbolic link/junction/reparse | `file_source.unsupported_reparse`，无 RP |
| U02 | selection 含 `NumberOfLinks > 1` file | `file_source.unsupported_hard_link`，无 RP |
| U03 | selection 含 sparse file | `file_source.unsupported_sparse`，无 RP |
| U04 | selection 含 named ADS | `file_source.unsupported_ads`，无 RP |
| U05 | unchanged parent-ref 候选后来含上述对象 | 完整当前枚举仍 strict fail |
| U06 | 开发 Archive 含旧字段/enum | Reader/Restore 写前拒绝，不兼容读取 |

### 15.4 损坏、资源与生命周期

| ID | 场景 | 期望 |
| --- | --- | --- |
| C01 | parent UUID/fingerprint/backup set 错 | chain open 拒绝 |
| C02 | parent stream index 缺失/类型或大小错 | Verify/Restore 写前拒绝 |
| C03 | parent cycle/depth 129 | graph/reader 拒绝 |
| C04 | 任一父层缺卷/tag/payload 损坏 | user Verify 失败；Restore 无 mutation |
| C05 | USN record truncation/unknown version | eligibility 不成立，安全 Full |
| C06 | enumeration/content/index spool 取消 | 有界取消、Abort、无可见 RP |
| C07 | destination/index spool full | stable failure、partial 清理 |
| C08 | Archive 已发布但 Catalog 失败 | scanner 可重建，状态明确收敛 |
| C09 | Restore preflight 后父 generation 变化 | Start 拒绝并要求重新 preflight |
| C10 | 删除有后代祖先 | planner 拒绝；整链删除后代先行 |

## 16. 每包交接模板

```markdown
### FIx 交接

- owner / baseline commit：
- 实际修改文件：
- 合同或持久化变化：
- 删除的旧字段/分支（确认无兼容路径）：
- production targets（Debug/Release）与结果：
- architecture/static/format/secret checks 与结果：
- 人工验证场景、环境与结果：
- `git diff --check` / 文档链接检查：
- 已知限制（必须属于后续工作包，不得是本包 DoD 缺口）：
- 下一包可依赖的不变量：
```

交接不得只写“build passed”。必须列出准确 target、配置、人工输入/预期/结果和稳定 message code。隔离 Archive、
credentials、日志和样本不提交仓库。

### FI0 交接

- owner / baseline commit：Grok agent / `b41043c`
- 实际修改文件（核心）：
  - contracts: `file_set.h/.cpp`、`service_control.h/.cpp`
  - ports: `file_sink.h`
  - format: `file_index.h`、`file_index_codec.cpp`
  - pipeline: `file_set_backup_pipeline.cpp`、`file_set_restore_pipeline.cpp`、restore header
  - adapters: `windows_file_snapshot_view.cpp`、`windows_file_tree_sink.cpp`、`windows_file_source_browser.cpp`、
    windows_filesystem header、sqlite schema/support（随 restore_ads 删除同步）
  - service/worker/desktop 协议与消费者：删除 `restore_ads`/`reparse_policy` 字段与 UI 文案
  - docs: V7、protocol V4、modules、limits、architecture、本计划、ADR-0018 相关交叉引用
- 合同或持久化变化：
  - `FileEntryKind` 仅 directory/file；`FileStreamKind` 仅 main
  - 删除 hard_link_group、allocated_ranges、ADS/reparse 表达与相关 flags
  - `FileSinkCapabilities` 仅 `supports_security_descriptor` + `free_bytes`
  - `FileRestoreTarget` / Service V4 / Worker schema：删除 `restore_ads`；删除 `reparse_policy`
  - File Index exact schema 拒绝旧字段（reader fail，无 dual-read）
- 删除的旧字段/分支（确认无兼容路径）：
  - `kReparse`/`kOther`/`kAlternate`、hard-link materialize、sparse hole、ADS restore 分支
  - 虚假 sink `supports_*`（reparse/hard-link/sparse/ADS）
  - Desktop `restore_ads` 开关与翻译 id
- production targets（Debug/Release）与结果：
  - vs2026-debug / vs2026-release：`aegra_contracts`、`aegra_format`、`aegra_pipeline`、
    `aegra_adapter_personal_archive`、`aegra_adapter_windows_filesystem`、`aegra_adapter_sqlite`、
    `aegra_app_service`、`aegra_service`、`aegra_app_worker_personal`、`aegra_personal_worker`、
    `aegra_desktop` — 均成功（`/WX`）
- architecture/static/format/secret checks 与结果：
  - `cmake -P cmake/CheckSourceLimits.cmake` 通过
  - 生产树 grep：无 `restore_ads`/`kReparse`/`kAlternate`/虚假 capability 残留（codec 仅含拒绝旧字段逻辑）
  - `git diff --check`：无 whitespace error（仅 CRLF 提示）
- 人工验证场景、环境与结果：
  - 本包未在本机跑完整 Backup/Restore UI 矩阵（需 Service+Worker 运行态与隔离样本）
  - 代码路径已就绪：`reject_unsupported_object` →
    `file_source.unsupported_reparse|hard_link|sparse|ads`；codec/pipeline 写前拒绝旧字段
  - 建议 FI10/人工：U01–U04 + 普通 Full Backup/Verify/Restore（I 系列成功链中的 Full 子集）
- 已知限制（后续包）：
  - Incremental / USN / chain 全部属于 FI1+
  - 工作树可能含与 FI0 无关的 Desktop/docs 改动（开工前已存在）；FI0 不声称拥有那些 diff
- 下一包可依赖的不变量：
  - Full 路径只表达 dir/file + unnamed main stream
  - Source 四类对象枚举期 strict fail，无 RP
  - Sink 不宣称未实现能力；Restore preflight 检查 security capability
  - 无兼容/migration 路径；FI1 可直接在 current V7 上扩展 USN/identity 字段

### FI1 交接

- owner / baseline commit：Grok agent / `b41043c`
- 实际修改文件（核心）：
  - contracts: `file_set.h/.cpp`（identity/USN/fingerprint/change batch/downgrade）、`job.h/.cpp`、
    `task_result.h/.cpp`
  - ports: `file_source.h`（`query_journal_state` / `read_change_batch`）
  - format: `manifest.h`、`manifest_codec/validation`、`file_index_codec`、`personal_archive.h`/
    codec（`CAP_FILE_USN_BASELINE`）
  - personal_repository: Catalog V2 keys `file_selection_fingerprint` / `file_baseline_available`
  - pipeline: Full planner 固定 `content_storage=local`
  - adapters: Windows snapshot 填 `StableFileIdentity`；journal Port stub `available=false`
  - service/worker: BackupOptions wire（fingerprint + candidate_parent）、Service 计算 fingerprint、
    Catalog 注册 hex fingerprint、TaskResult requested/effective 类型字段
  - docs: V7、Catalog V2、Service V4 TaskResult、contracts/ports/worker/file_set modules、本计划
- 合同或持久化变化：
  - file_set Job 要求 `selection_fingerprint`；允许 Full/Incremental + `candidate_parent_uuid`
  - V7 file_set Manifest 增加 `file_set_baseline`；Index entry 增加 `stable_file_identity`；
    stream 增加 `content_storage`/`parent_stream_index`
  - Header 能力 `CAP_FILE_USN_BASELINE`；Incremental 必须置位
  - Catalog 允许 file_set incremental parent；exact keys 增加 fingerprint/baseline 字段
- 删除的旧字段/分支：无（FI0 已清）；无 dual-read/alias
- production targets（Debug/Release）与结果：
  - vs2026-debug / vs2026-release：`aegra_contracts`、`aegra_format`、`aegra_pipeline`、
    `aegra_personal_repository`、`aegra_adapter_windows_filesystem`、`aegra_app_worker_personal`、
    `aegra_personal_worker`、`aegra_app_service`、`aegra_service` — 均成功（`/WX`）
- architecture/static/format/secret checks 与结果：
  - 核心层无 Windows 类型；fingerprint preimage 在 contracts，hash 在 composition root
  - 生产消费者只使用当前 schema 字段
- 人工验证场景、环境与结果：
  - 本包以 codec/validator 与生产构建验收为主；完整 Full Backup 人工矩阵与 corrupt Archive 翻转属
    FI10 / 后续包运行态
  - Full 路径仍只执行 Full；Incremental 执行与 USN 实读属 FI2–FI7
- 已知限制（后续包）：
  - journal Port 在 Windows Adapter 上仍 stub（`available=false`）→ FI2
  - Incremental planner / parent stream writer / chain reader / 选父编排 → FI3–FI7
  - Catalog `file_baseline_available` 在 Full 路径仍为 false（待 FI2 checkpoint 写入后投影）
- 下一包可依赖的不变量：
  - 平台无关 USN/identity/change/fingerprint 合同与校验器已冻结
  - V7/Catalog V2 codec 对称读写合法组合，非法组合拒绝
  - Full 备份 Job 始终携带 non-zero selection fingerprint；Catalog 持久化 hex 摘要
  - FI2 只实现 Port，不改合同字段名；FI3 可直接使用 `content_storage` 与 stable identity

### FI2 交接

- owner / baseline commit：Grok agent / `b41043c`（与 FI1 同工作树并行 FI3）
- 实际修改文件：
  - `src/adapters/windows_filesystem/src/windows_usn_journal.{h,cpp}`（新建）
  - `windows_file_snapshot_view.cpp`（journal Port 实装）
  - `CMakeLists.txt`（adapter）
  - Worker `windows_file_set_backup.cpp`：snapshot 上查询 journal，可用时写入 Manifest checkpoints
  - docs: adapters / windows_file_set_backup / modules README
- 行为：
  - `query_journal_state` / `read_change_batch` 仅用 snapshot volume handle；不可用 → `available=false`
  - V2/V3 USN record；未知 major → 失败；reason 保守映射；batch 有界
  - 四类 unsupported 检测保持 FI0 路径（枚举期 strict fail）
- production targets Debug/Release：`aegra_adapter_windows_filesystem`、`aegra_app_worker_personal` 通过
- 已知限制：Catalog `file_baseline_available` 投影与 Incremental 选父仍属 FI6/FI7；VSS snapshot 上 journal
  是否可用取决于平台，不可用时 Full 不带 checkpoints

### FI3 交接

- owner / baseline commit：Grok agent / `b41043c`（与 FI2 并行）
- 实际修改文件：
  - `src/pipeline/include/aegra/pipeline/file_set_change_planner.h`（新建）
  - `src/pipeline/src/file_set_change_planner.cpp`（新建）
  - `file_set_backup_pipeline.h/.cpp`：effective_type / parent / checkpoints / hints；接入 planner
  - `pipeline/CMakeLists.txt`；docs: pipeline.md
- 行为：
  - Full：全部 local（回归路径）
  - Incremental：USN hints + 紧凑 parent identity 索引 + 分类表 → local|parent；歧义永远 local
  - tip Index 仍是完整当前树；仅 local 非空 stream 进入 payload work list
- production targets Debug/Release：`aegra_pipeline`、`aegra_app_worker_personal` 通过
- 已知限制：Worker 默认仍发 Full；选父编排属 FI6/FI7

### FI4 交接

- owner / baseline commit：Grok agent / `b41043c`
- 实际修改文件：
  - `personal_archive.h`：`FileArchiveCreateRequest.parent_uuid`
  - `personal_file_archive_session.cpp`：Full/Incremental Header、parent stream 拒绝/接受、finalize Index 校验
  - Worker `windows_file_set_backup.h/.cpp`：effective_type / parent_uuid / parent reader / checkpoints
  - docs: adapters、windows_file_set_backup、V7 Footer 计数、modules README、本计划
- 行为：
  - Full：`parent_uuid=0`，无 parent stream；Incremental：`parent_uuid` 非 0 + `CAP_FILE_USN_BASELINE`
  - parent stream 不写 payload；local 沿用 chunk write
  - finalize 拒绝 Full 含 parent、重复 entry/stream id、非法 parent ref 语法
  - Footer：tip entry/stream 全集 vs 本层 local chunk/logical_bytes
- production targets Debug/Release：`aegra_adapter_personal_archive`、`aegra_app_worker_personal` 等
- 已知限制：任务入口仍默认 Full；chain 解析 parent payload 属 FI5；选父/降级编排属 FI6/FI7

### FI5 交接

- owner / baseline commit：Grok agent / `b41043c`
- 实际修改文件：
  - `ports/file_recovery_point.h`：chain-capable `IFileRecoveryPointReader` 语义（tip browse + parent resolve）
  - `personal_archive.h` + `personal_file_archive_chain_reader.cpp`：`PersonalFileArchiveChainReader`、
    `FileStreamOwnerView`、`FileChainVerifyResult` / `verify_recoverability`
  - `personal_file_archive_reader.cpp`：stream index map、`describe_stream_owner`、单层拒绝 parent read
  - Worker file_set Verify/Restore：base-first multi-layer `source_refs` + matching credentials + chain open
  - `contracts/job.cpp`：file_set verify/restore 允许 1..`kMaximumFileChainDepth` source_refs
  - docs: ports、adapters、pipeline、windows_file_set_backup、modules README、本计划
- 行为：
  - open 校验 Full root、parent_uuid 链、backup_set、selection fingerprint、无环、深度
  - browse/describe 仅 tip Index；`read_stream` 迭代解析 parent→local 并校验 identity/kind/size
  - Verify 认证每层 local payload + 解析 tip 全部 stream（可恢复性，非仅 tip 结构）
- production targets Debug/Release：`aegra_adapter_personal_archive`、`aegra_contracts`、
  `aegra_app_worker_personal`、`aegra_personal_worker` 已通过；`CheckSourceLimits` 通过
- 已知限制：Service 选父与组装完整链 source_refs 仍属 FI7；Catalog 生命周期属 FI6（已完成）

### FI6 交接

- owner / baseline commit：Grok agent / `b41043c`
- 实际修改文件：
  - `chain_graph.cpp`：file/volume 分规则（content_kind、file fingerprint、file 父类型）
  - `parent_selector.h/.cpp`：`select_file_incremental_parent` + `is_volume_chainable_parent`
  - `retention.h/.cpp`：tip 祖先闭包、最近 N tip、后代探测、链深触发 `request_new_full`
  - `personal_repository/CMakeLists.txt`：新 TU + PUBLIC `Aegra::Contracts`
  - Service：`file_baseline_available` 在 fingerprint 非空时为 true；volume 选父复用 repository API
  - docs: personal_repository、增量架构、本计划
- 行为：
  - 选父只认 `last_recovery_point_id` tip，不合格稳定降级 Full，不跳祖先
  - 降级 reason 使用 contracts `IncrementalDowngradeReason`（NoParent/SelectionChanged/
    ChainIncomplete/BaselineInvalid）
  - 删除仍 descendant-first；file_set 无 `.bhx`；retention 保留 tip→Full 闭包
  - 无凭据扫描 fingerprint 空 → baseline false → 不可作新 Incremental 父
- production targets Debug/Release：`aegra_personal_repository`、`aegra_app_service` 已通过；
  `CheckSourceLimits` 通过
- 已知限制：Browse/Restore 多链 source_refs 组装属 FI8

### FI7 完成记录

- owner：Grok agent
- 行为：
  - file_set Schedule/StartBackup 允许 `full|incremental`；Service 用 Catalog
    `select_file_incremental_parent` 选 tip，不合格时 wire 保留 requested Incremental +
    `service_full_reason`，合格时填 `candidate_parent_uuid` + `parent_source_ref`
  - Worker 打开父 Archive 校验 identity/set/checkpoints；VSS 后评估 USN 连续性，不可证明则
    effective Full 并 progress `file_backup.incremental_downgraded_full`；父凭据失败仍 hard fail
  - TaskResult / JobRecord / JobSummary 投影 requested/effective type、effective parent、
    downgrade reason；Catalog 发布使用 effective 而非 requested
  - SQLite control-plane schema **13**（无 migration；开发库需重建）
- 已知限制：ListRecoveryPointEntries / Restore 多链 reader 属 FI8（已完成）；Desktop Incremental UX 属 FI9

### FI8 完成记录

- owner：Grok agent
- 行为：
  - `file_recovery_chain`：Catalog tip→Full 解析 + 认证 `PersonalFileArchiveChainReader` 打开
    （base-first）；凭据顺序与 F7 一致（显式 secret → 空口令 → connection default）
  - `ListRecoveryPointEntries` 仅经 chain reader 分页 tip Index；continuation 绑定
    `chain_generation|tip_index_generation|parent_entry_id|reader_token`
  - `PrepareFileRestore` 打开完整链，选择闭包后对每个普通文件 `resolve_stream_reference`
    （写前解析 parent stream）；fingerprint 含 tip digest、chain generation、chain_depth
  - `StartFileRestore` 重开链比对任意层 generation；Worker Job `source_refs` 为 base-first 全链
  - file_set `prepare_verify` 同样注入 base-first chain source_refs
  - chain reader 新增 `chain_generation_digest()` / `resolve_stream_reference()`
- 构建：Debug/Release `aegra_adapter_personal_archive`、`aegra_app_service`、
  `aegra_app_worker_personal`、`aegra_service`、`aegra_personal_worker` 通过；
  `CheckSourceLimits` 通过
- 已知限制：无（Desktop Incremental UX 属 FI9，已完成）

### FI9 完成记录

- owner：Grok agent
- 行为：
  - Desktop 协议：file_set Schedule 允许 `backup_type` Full|Incremental；Recovery Point 接受
    Incremental + parent_uuid；JobSummary 投影 requested/effective/parent/downgrade 到 UI map
  - 创建 file_set 计划：向导可选 Full/Incremental（无 Differential），附选择变化建立新 Full 基线说明
  - JobModel / schedule 状态：展示 requested→effective 与本地化 downgrade reason；不暴露 USN/file ID
  - RecoveryPointModel：parent 安全摘要、链深度展示、baseline 标记；Repository 删除走
    PlanDelete/ExecuteDelete，仅展示 server targetCount（+ 列表 retained 提示）
  - message_code_map + 五语言：file recover parent/chain、incremental_downgraded_full、journal、
    四类 unsupported source（明确未跳过）
- 构建：Debug/Release `aegra_desktop` 通过；`cmake -P cmake/CheckSourceLimits.cmake` 通过
- 已知限制：无（发布门禁与人工矩阵属 FI10）

### FI10 完成记录

- owner：Grok agent / baseline `b41043c`（工作树含 FI0–FI9 与门禁修补）
- 生产修补：
  - Desktop volume `createSchedule` / `upsertSchedule` 接线 `backup_type`（1|2）；
    `setScheduleEnabled` 保留既有 schedule 的 backupType，避免误写回 Full
  - `BackupPage` 卷计划创建传入向导选择的 backupType
  - `SERVICE_CONTROL_PROTOCOL_V4.md` 去除 trailing whitespace（`git diff --check`）
- production targets（Debug/Release，`out/build/vs2026-{debug,release}`，
  `AEGRA_BUILD_DESKTOP=ON`，Qt `C:/Qt6/6.8.3/msvc2022_64`，VS 2026 Insiders）：
  - 全量 `cmake --build` 通过（含 `aegra_service`、`aegra_personal_worker`、
    `aegra_mount_host`、`aegra_desktop` 及全部 library targets）
- architecture/static/format/secret checks：
  - `cmake -DAEGRA_SOURCE_ROOT=... -P cmake/CheckSourceLimits.cmake` → 通过
  - `git diff --check` → 无 whitespace error（仅 CRLF 提示）
  - 文档相对链接抽检（增量相关 docs）→ 全部可解析
  - 兼容/禁路径审计：`src` 无 `restore_ads`/`reparse_policy` 生产路径；`hard_link_group`/
    `allocated_ranges` 仅 exact-schema 拒绝；无 timestamp-incremental；SQLite 无 schema
    migration（仅 exact version write）；reparse/sparse/hard-link 仅 strict-fail 检测
  - 稳定码覆盖核对：`file_source.unsupported_*`、`file_backup.journal_*`、
    `IncrementalDowngradeReason`、parent_selector demote 路径均存在
- §15 人工矩阵：
  - 静态覆盖：I/D/U/C 各场景对应 code path 与 message_code 已在生产代码中定位
  - 提升权限下的完整 live Service 矩阵（隔离样本 + VSS 运行态）与 F10 相同，需本机
    admin Service 与隔离数据；不在本机自动化环境提交隔离 Archive/凭据
  - 无 P0/P1 代码缺口：不支持对象 strict fail、metadata baseline 不可证明时降级 Full、chain
    browse/restore 经 chain reader、Desktop 不暴露内部 file identity
- 文档同步：ADR-0018 已 Superseded，ADR-0020 保持 Accepted；architecture 增量/基础设计状态转 metadata signature；
  modules README、windows_file_set_backup、PRODUCT_SCOPE、本计划总览表
- 已知限制：无（file_set Incremental 工作包序列结束）
