# 文件集备份与恢复分阶段开发计划

> 本计划是 agent 执行依据。ADR-0016 接受前只允许完成 F0 文档决策；接受后按工作包前置关系实施。
> 仓库遵循 ADR-0015：禁止增加测试源码、fixture、测试脚本、测试 Target 或 CTest 注册。验证使用生产 Target
> 构建、静态/架构检查和隔离数据上的人工运行/UI 验证。

## 1. Agent 使用规则

每个 agent 开工前必须：

1. 阅读根 `AGENTS.md`、`.agents/skills/aegra-cpp-development/SKILL.md`、工程规范和模块架构；
2. 阅读本计划、ADR-0016、完整设计、受影响模块文档以及它们引用的 ADR/格式/协议；
3. 检查 `git status --short`，不覆盖其他 agent 或用户未提交变更；
4. 在总览表把负责包标成“进行中”，记录 owner、基线 commit 和实际文件所有权；
5. 先完成依赖和公共契约，再让消费者接线；禁止临时 include Adapter、Qt、JSON 或 Win32 绕过边界；
6. 完成时更新实现、模块文档、格式/协议文档、状态表和人工验证记录。

一个 agent 只领取一个工作包或一个明确子包。公共头、协议 codec、根 `CMakeLists.txt`、模块 README 和
composition root 由该包 integration owner 修改。并行包不得同时编辑同一文件。

## 2. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 前置 | 可并行关系 |
| --- | --- | --- | --- | --- | --- |
| F0 | 已完成 | Gate | ADR、V7 格式和 V4 协议冻结 | 无 | 无 |
| F1 | 已完成 | P0 | Contracts、Ports 与公共模型 | F0 | 无 |
| F2 | 已完成 | P0 | Personal Archive V7 与 Catalog V2 | F1 | F3、F4 |
| F3 | 已完成 | P0 | FileSet Backup/Restore Pipeline | F1 | F2、F4 |
| F4 | 已完成 | P0 | Windows Filesystem Source/Sink Adapter | F1 | F2、F3 |
| F5 | 已完成 | P1 | Worker 文件备份纵向切片 | F2、F3、F4 | 无 |
| F6 | 已完成 | P1 | Service 浏览、控制面与文件备份编排 | F1、F5 | 无 |
| F7 | 已完成 | P1 | Recovery Point 文件查询与 Verify | F2、F6 | F8 仅可设计审查 |
| F8 | 已完成 | P1 | 文件选择性恢复纵向切片 | F3、F4、F7 | 无 |
| F9 | 已完成 | P2 | Desktop 文件备份/恢复体验 | F6、F7、F8 | 无 |
| F10 | 已完成 | Gate | 全量回归、文档收口和发布门禁 | F2-F9 | 无 |
| F11 | 已完成 | P3 | 文件级 Incremental 决策移交 | F10、ADR-0018 | 由 FI0–FI10 替代 |

状态只允许：`可开始`、`进行中`、`等待前置`、`阻塞`、`已完成`、`暂缓`。integration owner 维护本表，
不能以文件存在代替完成标准。

## 3. 全局技术约束

- **产品未发布，不做任何兼容性处理。** 正式格式直接升级 V7，Service 只支持 API V4，Worker 只支持 Job
  schema 4，Catalog 只支持 V2，SQLite 只支持新版正式 schema。
- 禁止 V6/V7 dual-read、V3/V4 negotiation、旧 magic/schema 特判、旧字段 alias、format conversion、database
  upgrade、data migration、fallback、兼容 feature flag 和先新后旧的 retry 路径。
- Parser 对 `version != current` 执行统一拒绝即可，不得为了给旧开发数据更友好的错误而解析旧 Header、payload、
  Catalog 或数据库。现有开发 Archive、Repository、SQLite 和 IPC 样本直接删除并重新生成。
- 当前已完成实现只允许 `file_set + full + local NTFS/ReFS + VSS + strict failure`。Incremental 实施使用
  [FI0–FI10 计划](FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md)，不得继续扩展本计划。
- 本期不支持 reparse、hard link、sparse、ADS；FI0 先删除相关预留合同/分支并建立 strict reject。
- V7 Volume 行为必须与现有 V6 功能等价，不能为了文件功能搁置 Volume Backup/Verify/Restore。
- 文件树和 Index 处理必须有界；禁止 `vector<all files>`、单个巨型 CBOR 和无界 producer queue。
- Desktop 不使用 `QDir`/`QFileInfo` 读取保护源，不向 Service 发送绝对路径。
- 路径、文件名和 security metadata 属于客户数据；不得进入 Catalog、普通日志、错误文本或 task event。
- 所有新持久化字段固定宽度、明确字节序、版本、上限、checked arithmetic 和拒绝规则。
- 每个生产 C++ 函数、lambda、类和文件遵守工程规模限制。

## 4. F0：决策与规范冻结

**目标：** 在写生产代码前冻结不可逆 disk/wire/security contract。

**允许修改：** `docs/adr`、`docs/architecture`、`docs/development`、`docs/format`、`docs/protocol` 和文档索引。

**实现任务：**

- 评审并接受或修订 ADR-0016；固定 `content_kind`、V7 单版本切换、Service V4、Worker schema 4、strict
  failure、VSS 和 reparse no-follow。
- 编写 `PERSONAL_BACKUP_FORMAT_V7.md`，逐字节定义 Header、record prefix、volume/file chunk、File Index
  page、Footer、分卷、AEAD AAD、摘要、最大值和拒绝规则。
- 明确 Windows 文件名编码。默认采用 tagged `windows_utf16le` bytes，禁止 codec 隐式 lossy UTF-8 转换。
- 定义 File Index B+tree key/value、排序、页引用、stream extent 和 root digest。
- 编写 `PERSONAL_REPOSITORY_FORMAT_V2.md`，增加 `content_kind`，删除 V1-only 假设。
- 编写 `SERVICE_CONTROL_PROTOCOL_V4.md` 和相应 ADR，冻结 kind、payload、分页/token、权限、幂等和 frame
  示例；明确 V3 被替代而不是共存。
- 冻结产品上限、stable error/message codes 和人工损坏样本矩阵。

**必须回答：**

- SACL 无权限时是 strict failure，还是首版明确不承诺 SACL；不能留给 Adapter 猜测。
- 目标不支持 ADS/sparse/ACL/reparse 时是 preflight reject，还是有显式 loss policy；首版推荐 reject。
- Index spool 的最大磁盘预算、位置、ACL 和崩溃清理规则。
- 分卷时 Index record 可位于哪些 part，Footer 如何定位跨 part root page。

**完成标准：** 所有字段和语义无“待定/TODO”；ADR 状态为 Accepted；格式、协议和设计无冲突。

**验证：** `git diff --check`；人工核对 ADR 模板、format offset/size 算术和 protocol frame 上限。

## 5. F1：Contracts、Ports 与公共模型

**目标：** 提供不含 Qt、JSON、Win32 和文件系统路径类型的稳定核心契约。

**主要文件：**

- `src/contracts/include/aegra/contracts/job.h`
- `src/contracts/include/aegra/contracts/service_control.h`
- `src/contracts/include/aegra/contracts/progress.h`
- `src/contracts/src/job.cpp`、`service_control.cpp`、`progress.cpp`
- 新增 `src/ports/include/aegra/ports/file_source.h`
- 新增 `src/ports/include/aegra/ports/file_sink.h`
- 新增 `src/ports/include/aegra/ports/file_browser.h`
- 新增 `src/ports/include/aegra/ports/file_backup_session.h`
- 新增 `src/ports/include/aegra/ports/file_recovery_point.h`
- `docs/modules/contracts.md`、`ports.md`

**实现任务：**

- 把 `JobRequest` 升至 schema 4，引入互斥 `ContentKind`、Volume/File source payload 和 typed restore target。
- 把 Service DTO 升至 V4，增加 browse、File Index query、file restore preflight/command 和 schedule tagged spec。
- 定义 name encoding、entry/stream/extent descriptor、platform metadata envelope 和所有上限常量。
- 定义枚举 batch、stream reader、file backup session、file recovery reader、staged sink 的所有权与取消语义。
- 定义 Service 文件浏览的分页 Port；输入/输出使用 opaque node identity，不暴露平台路径。
- 每个 Port 注释线程安全、短读、payload 生命周期、错误、取消和析构 Abort 行为。
- 扩展 progress，使 enumeration 可以表达 item count 和 byte total 尚未知；wire 整数不超过 `INT64_MAX`。
- 更新 validator：严格字段组合、唯一性、count/range、UTF-16LE 长度、相对 component 和枚举值。

**禁止：** Contracts/Ports 出现 `std::filesystem::path`、`HANDLE`、Qt、JSON、VSS、SQLite 或 Archive
物理 offset。

**完成标准：** `aegra_contracts` 与直接生产消费者可构建；旧 schema 常量和解析分支删除；公共接口文档完整；
没有消费者用字符串判断 content kind。

**人工验证：** 通过现有生产 executable 的协议入口提交合法/非法 schema 4 frame，确认拒绝未知版本、混合
Volume/File payload、绝对组件和超限计数；不保存样本为 fixture。

## 6. F2：Personal Archive V7 与 Catalog V2

**目标：** 实现纯格式 codec、V7 Archive 会话和 Repository 可重建摘要。

**主要范围：** `src/format/*`、`src/adapters/personal_archive/*`、`src/personal_repository/*`、对应 CMake 和
`docs/format`、`docs/modules/format.md`、`adapters.md`、`personal_repository.md`。

### F2.1：V7 基础 codec

- 替换 V6 Header/Footer/Manifest schema；保留 Volume V7 字段但不保留 V6 Reader。
- 使用显式小端读写，逐字段 checked offset；实现 record prefix 与 unknown critical kind 拒绝。
- 将 `content_kind` 纳入 Header validation 和 Catalog scanner。

### F2.2：File Index codec

- 实现 entry、stream、extent、platform metadata 与 B+tree page 编解码。
- 解密/解压前检查 encoded/plain 上限；认证后检查排序、parent graph、root reachability、page cycle/depth、
  stream/extent 唯一性和 chunk reference。
- Writer 规范化输出；Reader 拒绝重复 key、非 text CBOR key 和未知关键版本。

### F2.3：Archive Session

- 新增 file session，流式接受 entry 和 chunk；index records 写 staging spool，不能累计整树。
- 最终化时写 Index、root digest、Footer；失败/取消/异常通过 RAII 删除 spool 和 Archive partial。
- 分卷只在完整 record 边界切换；发布仍为 Sidecar/续卷先、首卷最后。

### F2.4：Reader、Verify 与 Catalog

- 提供分页 directory lookup、entry lookup、stream chunk resolution；对象 range read 不要求整 Archive 下载。
- Verify 覆盖所有页、file chunks 和引用关系。
- Catalog Entry 升 V2，增加 `content_kind` 和文件统计，禁止路径字段。
- Scanner 从 V7 Header 无凭据重建结构摘要；File Full 不参与 Volume parent geometry/Sidecar 逻辑。

**完成标准：** Volume V7 与 File V7 均能生成、结构扫描、认证、Verify 和分页读取；损坏引用在返回 payload 前
拒绝；内存不随 entry 总数线性增长。

**人工验证：** 在隔离目录生成小型和大规模非生产 Archive；手工截断/翻转复制件中的 Header、page、tag、
extent 和 Footer；验证失败分类与 partial 清理。样本不得提交仓库。

## 7. F3：FileSet Backup/Restore Pipeline

**目标：** 实现平台无关、存储无关的文件数据面编排。

**主要文件：** 新增 `file_set_backup_pipeline.h/.cpp`、`file_set_restore_pipeline.h/.cpp`，更新
`src/pipeline/CMakeLists.txt` 和 `docs/modules/pipeline.md`。

**实现任务：**

- 实现枚举、计划、读取、索引、最终化状态机；每个状态有 guard 和唯一失败映射。
- 用 byte-bounded queue 传递内容 buffer，用 count-bounded queue 传递 entry metadata。
- hard-link group 只读取一次内容；ADS 为独立 stream；sparse hole 不分配/读取 payload。
- 对 entry ID、parent、stream index、extent 和 totals 使用 checked arithmetic 与确定排序。
- 取消贯穿 enumeration、open/read、queue wait、session write、finalize；线程由 RAII join。
- Restore 在任何 Sink mutation 前完成 Index/selection preflight，再按设计顺序写入。
- partial restore 返回结构化计数和稳定 warning/error code，不返回路径。

**禁止：** include personal archive、Windows filesystem、VSS、Qt 或 Repository；禁止隐藏全局执行器。

**完成标准：** `aegra_pipeline` 独立构建；空选择、空文件、尾块、sparse、取消、背压和 commit/abort 已代码
审查并人工运行；Block Pipeline 行为未改变。

## 8. F4：Windows Filesystem Source/Sink Adapter

**目标：** 把 Windows 文件系统精确语义实现为 F1 Ports。

**目录与 Target：** 新增 `src/adapters/windows_filesystem/`，Target
`aegra_adapter_windows_filesystem` / `Aegra::AdapterWindowsFilesystem`。只依赖 Base、Contracts、Ports 和
Windows SDK，不依赖另一个有状态 Adapter。

**Source 任务：**

- 通过 Volume handle 解析 selection，拒绝 UNC、设备路径注入和非 NTFS/ReFS。
- 接收 composition root 提供的 snapshot mapping；Adapter 本身不创建 VSS Session。
- 实现 F1 `IFileSourceBrowser`，按调用方授权上下文分页枚举 roots/children；token 生命周期仍由 Service 管理。
- 使用 handle-relative/no-follow 枚举，获取 file ID、hard-link identity、stream、allocated range、security
  descriptor、attributes/times 和 reparse data。
- 精确编码 UTF-16LE 名称；检测超限、目录深度、重复 identity 和 unsupported EFS/cloud state。
- 所有 Win32 handle 使用 RAII；错误映射为稳定 `ErrorCode`，不把原始路径写入错误。

**Sink 任务：**

- 打开并固定目标根目录句柄，后续相对创建不能逃逸；每级检查 reparse。
- 实现同目录 staging 文件、完整写、flush、close、冲突策略和原子 rename。
- 支持 ADS、sparse ranges、hard link、reparse no-follow 和 metadata 应用顺序。
- 析构清理未发布 staging；已发布 mutation 不得伪报取消。
- preflight 报告目标 FS capability；缺能力按 F0 决策拒绝。

**完成标准：** Adapter 单 Target 构建；人工隔离目录验证 ordinary/empty/Unicode/deep tree/ACL/ADS/sparse/
hard-link/reparse；path escape、target reparse swap、disk full 和 access denied 得到稳定失败。

## 9. F5：Worker 文件备份纵向切片

**目标：** 让 schema 4 File Backup Job 在独立 Worker 中生成并提交 V7 Recovery Point。

**主要范围：** `src/apps/worker/*`、Worker CMake、`docs/modules/worker_host.md`，新增
`docs/modules/windows_file_set_backup.md`。

**实现任务：**

- Worker codec 只接受 schema 4；编码/校验 typed File source。
- 新增 `WindowsFileSetBackupTask`，解析 SecretRef，构造一个跨卷 VSS Snapshot Set。
- VSS 成功后建立每个 Volume 的 snapshot mapping，注入 Windows Filesystem Source。
- 组合 FileSet Pipeline 和 Personal Archive V7 Session；Secret 生命周期覆盖 Archive 操作。
- 资源销毁顺序：停止 pipeline -> 关闭 readers -> abort/commit session -> 清理 spool -> 删除 VSS。
- 结果/progress 使用稳定 code；不输出路径、token、SecretRef 或 frame body。
- File Job 明确拒绝 Incremental/Differential、UNC、EFS 和 offline cloud placeholder。

**完成标准：** `aegra_app_worker_personal`、`aegra_personal_worker` Debug/Release 构建；人工通过真实 Worker
Pipe 运行 Full 文件备份、取消、deadline、VSS failure、unreadable file 和 destination full；成功 Archive 可由
F2 Reader/Verify 打开。

## 10. F6：Service 浏览、控制面与备份编排

**目标：** 建立 Desktop 到 Worker 的可信选择和计划闭环。

**主要范围：** `src/application/*`、control-plane Port、SQLite Adapter、`src/apps/service/*`、Service V4 协议和
application/control_plane_sqlite/service_host 模块文档。

### F6.1：Browse use case

- 组合 F1 `IFileSourceBrowser` 与 F4 Windows 实现；Service 负责 caller identity 授权和 token 生命周期。
- token store 有 TTL、随机 token、caller/session binding、最大 active tokens 和断线清理。
- 分页稳定排序，重复/倒退 token、超限页和过期 token 返回稳定错误。

### F6.2：Schedule persistence

- `UpsertSchedule` 创建时解析 token、重新验证 identity、规范化/去重 selection 并事务持久化。
- 保存 owner identity；update 保持 source frozen；列表只返回安全 display summary。
- command fingerprint 对规范化业务输入稳定；replay 返回已存结果，不重新消费过期 token。

### F6.3：Backup orchestration

- StartBackup 从 Schedule 加载 selections、Repository、加密选项和 protected password。
- 重新校验 source availability，构造 schema 4 File Source Ref，创建 queued Job 后提交 Supervisor。
- Job source summary 使用 opaque selection IDs，不把路径写入 Job/Audit/Event。
- capability 只有在 Browse、SQLite、Worker 和 Repository 组合均可用时发布。

**完成标准：** `aegra_application`、`aegra_adapter_sqlite`、`aegra_app_service`、`aegra_service` 构建；人工
验证 unauthorized browse、token expiry、create/replay、source frozen、Service restart、background schedule
和 Worker failure。

## 11. F7：Recovery Point 文件查询与 Verify

**目标：** Service 在不向 Desktop 暴露 Archive path 的情况下浏览文件恢复点并完整 Verify。

**实现任务：**

- Application 按 connection/RP 打开可信 Archive Group，解析 credential，认证 V7 root/index。
- 实现 `ListRecoveryPointEntries`；continuation 绑定 index root digest 和 parent entry。
- 为 reader 设置 KDF、metadata、page、depth、entry、stream 和累计读取预算。
- 扩展 Verify Worker/Service，遍历认证全部 Index page、Chunk、extent 和 parent graph。
- Catalog-only 状态不能开启浏览；credential required/failed 与 corrupt 分开表达。
- Service cache 只保存短期 reader/session 投影，不保存明文 Secret 或完整文件树；断线/TTL 后清理。

**完成标准：** 大目录分页不一次性加载；非法 parent、重复 token、generation 改变和认证失败正确拒绝；Verify
真实读取每个 payload，不只验证索引。

## 12. F8：文件选择性恢复纵向切片

**目标：** 完成 Prepare -> Start -> Worker -> target directory 的恢复闭环。

**实现任务：**

- Service `PrepareFileRestore` 认证 selection closure、解析 target token、检查 owner/capability/space/policy。
- durable preflight 保存资源 identity 和 digest，不保存 Archive/target absolute path、Secret 或文件树。
- Start 唯一占用 token，重查 generation/index digest/target identity 后构造 Worker Job。
- Worker 打开 V7 Reader 和 Windows Sink，运行 FileSet Restore Pipeline。
- 支持 `fail`、`replace`、`rename`，默认 `fail`；original location 和 system directory 首版拒绝。
- TaskResult 区分 before-write failure、success 和 partial restore，记录 counts 与稳定 codes。
- 失败/取消/crash 后 Service 收敛 Job 为 interrupted；staging 由受限 cleanup 处理。

**完成标准：** Worker/Service 构建；人工恢复 single/multiple files、subtree、empty dir、ACL、ADS、sparse、
hard link、reparse，并验证 collision、disk full、cancel、target swap 和 partial result。

## 13. F9：Desktop 文件备份与恢复体验

**目标：** 在旧 Backup/Restore 页面视觉基线上接入真实 Service V4 数据。

**主要范围：** `src/apps/desktop/src/client/*`、`qml/pages/BackupPage.qml`、`RestorePage.qml`、translations 和
`docs/modules/desktop.md`。

**集成前置：** 当前工作区可能存在直接使用 `QDir/QFileInfo` 的 `FileTreeModel` 试验实现。Agent 先检查用户
未提交改动，不得直接删除或覆盖；正式模型只消费 Service response，不执行本地枚举。

**实现任务：**

- Desktop codec 全量升级 V4，删除 V3 parser/fallback；严格校验 kind、request ID、分页和整数范围。
- `FileTreeModel` 改为 lazy Service-backed model：node 保存 opaque token/entry ID、loading/error/check state，
  不保存绝对路径。
- Backup wizard 提供 `Volumes`/`Files and folders` mode；file mode 支持 lazy expand、tri-state selection、
  inaccessible/unsupported state 和 selection summary。
- 创建后 source 不可编辑；Schedule list 显示安全 label 和 file-set 类型。
- Restore 按 `content_kind` 切换 Disk layout 或 File browser；支持 multi-select、target browse、policy、preflight、
  confirm、progress 和 partial result。
- 可见文本使用稳定翻译 ID，更新五种语言；QML 不解析 wire enum/message code。
- 保存旧/新 900x600、1080x720、150% DPI 并排截图，检查文字和控件不重叠。

**完成标准：** `aegra_desktop` 使用 Qt 6.8.3 构建；人工验证 Service stop/restart、token expired、paged loading、
empty dir、permission error、backup/restore success/failure；Desktop 不打开保护源或 Archive。

## 14. F10：系统收口与发布门禁

**目标：** 证明文件功能没有破坏 Volume 路径、Repository 权威、安全和资源边界。

**生产构建：** 使用 Visual Studio 2026 Insiders 和 Qt 6.8.3，按仓库脚本真实接口执行 Debug/Release：

```powershell
cmd.exe /d /c scripts\build.cmd Debug
cmd.exe /d /c scripts\build.cmd Release
```

**静态门禁：**

- 运行仓库已有 architecture/static/format/secret 检查；缺少的门禁只记录，不新增测试脚本；
- `git diff --check`；
- 检查函数、lambda、class、`.h/.cpp` 行数和 dependency direction；
- 搜索 Desktop 的 `QDir/QFileInfo/std::filesystem` 数据访问和 core 的 Win32/Qt/JSON 泄漏；
- 搜索日志/错误是否包含 password、SecretRef、node token、security descriptor 或完整路径列表。
- 搜索并审查 `V6`、`schema 3`、`Catalog V1`、`legacy`、`migration`、`fallback`、`alias` 等命中；文档说明和
  “当前版本不匹配即拒绝”可以保留，任何生产兼容分支必须删除。

**人工验证矩阵：**

- Volume：Full、Incremental、分卷、加密/不加密、Verify、Volume/Disk Restore、Catalog rebuild；
- File：single file、recursive directory、multi-root/multi-volume、encryption、split、Verify、browse、restore；
- Metadata：empty、Unicode、deep tree、ACL；reparse、hard link、sparse、ADS 改由 FI0 验证 strict reject；
- Failure：VSS fail、unreadable、source removed、disk full、cancel、deadline、Worker crash、Service restart、
  Catalog publish fail、missing part、corrupt index/chunk/footer、target collision/reparse swap；
- UI：五语言、900x600/1080x720/150% DPI、disconnect/reconnect、expired token、partial restore。

样本只位于隔离临时目录或专用非生产 Volume，验证后清理，不提交仓库。

**文档收口：** 更新模块当前状态、产品范围、格式入口、协议入口、Desktop/Service 完成计划和发布说明。只有全部
门禁通过后把 F2-F9 标为完成。

## 15. F11：文件级 Incremental 决策移交（已完成）

ADR-0018 的 USN baseline 已被 [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md) 替代。
现行文件级 Incremental 使用完整 current namespace Index、direct-parent stream、chain reader 与
`write_time + logical_size` metadata signature。实际开发不再扩展 F11，统一按
[metadata signature 增量改造开发计划](FILE_SET_METADATA_SIGNATURE_DEVELOPMENT_PLAN.md) 的 MS 工作包执行。FI0 首先移除并严格拒绝
reparse、hard link、sparse、ADS；旧开发格式与接口不保留兼容路径。

## 16. Agent 交付模板

每个工作包完成时填写：

```text
Work package: F?.?
Owner:
Baseline commit:
Files owned/changed:
Public contracts changed:
Production targets built:
Architecture/static checks:
Manual validation performed:
Security/path cases reviewed:
Format/protocol docs updated:
Known remaining work (only outside this package):
```

若发现前置契约缺失，agent 停止修改消费者并反馈对应前置包；不得在本包创建私有重复 DTO、解析器、路径协议或
兼容分支。

## 17. F0 交付记录

```text
Work package: F0
Owner: agent (docs freeze)
Baseline commit: c8e0c80
Files owned/changed:
  docs/adr/0016-file-set-backup-and-restore-boundary.md (Accepted + F0 decisions)
  docs/adr/0017-service-control-protocol-v4.md (new)
  docs/adr/0013-service-control-protocol-v3.md (Superseded pointer)
  docs/format/PERSONAL_BACKUP_FORMAT_V7.md (new)
  docs/format/PERSONAL_REPOSITORY_FORMAT_V2.md (new)
  docs/protocol/SERVICE_CONTROL_PROTOCOL_V4.md (new)
  docs/development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md (new)
  docs/architecture/FILE_SET_BACKUP_RESTORE.md (Accepted links)
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md (status)
  docs/README.md, docs/adr/README.md, docs/modules/README.md, docs/requirements/PRODUCT_SCOPE.md
Public contracts changed: none in code; wire/disk contracts frozen in docs only
Production targets built: n/a (docs-only gate)
Architecture/static checks: git diff --check (clean of whitespace errors)
Manual validation performed:
  Header/Footer/record size arithmetic verified (256/480/32/96/80/112)
  F0 open questions A–E answered in ADR-0016
  Cross-links ADR ↔ format ↔ protocol ↔ limits
Security/path cases reviewed: SACL strict; target capability reject; spool ACL; no path in catalog
Format/protocol docs updated: V7, Catalog V2, Service V4, limits/codes matrix
Known remaining work (only outside this package): F1 contracts/ports implementation
```

## 18. F5 交付记录

```text
Work package: F5
Owner: agent (worker file_set backup vertical slice)
Baseline commit: c8e0c80
Files owned/changed:
  src/apps/worker/include/aegra/apps/worker/windows_file_set_backup_task.h (new)
  src/apps/worker/src/windows_file_set_backup.h (new)
  src/apps/worker/src/windows_file_set_backup.cpp (new)
  src/apps/worker/src/windows_file_set_backup_task.cpp (new)
  src/apps/worker/src/worker_protocol.cpp (file_source_refs / file_restore_target decode)
  src/apps/worker/src/worker_host.cpp (content_kind dispatch)
  src/apps/worker/src/windows_personal_backup_task.cpp (volume_set guard)
  src/apps/worker/CMakeLists.txt (sources + AdapterWindowsFilesystem)
  docs/modules/windows_file_set_backup.md (new)
  docs/modules/worker_host.md, apps.md, README.md
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md (status)
Public contracts changed: none (reuses schema 4 JobRequest / TaskResult)
Production targets built: aegra_app_worker_personal, aegra_personal_worker (vs2026-debug)
Architecture/static checks: production compile with /WX
Manual validation performed:
  Debug build of personal worker library and executable
  Full VSS/file backup path requires elevated machine + live volumes (not run in this agent turn)
Security/path cases reviewed:
  TaskResult/progress without path components; task log logs selection_id only
  Index spool under data_dir/staging/job-<uuid>/; cleaned after success/failure
  VSS required for all volumes (no raw fallback)
Format/protocol docs updated: worker_host schema 4 file_source_refs table
Known remaining work (only outside this package):
  F6 Service browse/orchestration; F7 file verify; F8 selective restore
  Live manual matrix: cancel, deadline, VSS fail, unreadable, destination full
```

## 19. F6 交付记录

```text
Work package: F6
Owner: agent (service browse / control plane / file_set backup orchestration)
Baseline commit: (working tree after F5)
Files owned/changed:
  src/ports/include/aegra/ports/control_plane.h (schema 11, Job/Schedule content_kind + file_selections)
  src/adapters/sqlite/* (schema v11, schedule_file_selections, job/schedule stores)
  src/application/include|src/file_browse_service.* (token TTL/caller binding)
  src/apps/service/schedule_service.* (file_set create resolve + owner_sid)
  src/apps/service/worker_job_service.cpp (prepare_file_set_backup + volume revalidate)
  src/apps/service/worker_supervisor.cpp (JobRecord.content_kind)
  src/apps/service/backup_catalog_registrar.cpp (Catalog V2 file_set fields)
  src/apps/service/service_host.* (session context, BrowseFileSources, Upsert caller)
  src/apps/service/service_security_host.cpp / service_main.cpp (peer SID session + browser roots)
  src/apps/service/CMakeLists.txt (AdapterWindowsFilesystem)
  docs/modules/control_plane_sqlite.md, service_host.md, contracts.md
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md
Public contracts changed: control-plane schema 11; Service V4 browse/schedule file_set handlers
Production targets built: aegra_application, aegra_adapter_sqlite, aegra_app_service, aegra_service
Architecture/static checks: production compile with /WX (vs2026-debug)
Manual validation performed:
  Build of application / sqlite / app_service / service targets
  Live browse unauthorized/token expiry and schedule create/replay require elevated Service run
Security/path cases reviewed:
  Desktop never receives paths; Job source_ids are selection UUIDs only
  Browse tokens bound to caller SID + pipe session; cleared on disconnect
Format/protocol docs updated: module docs for schema 11 and F6 host behavior
Known remaining work (only outside this package):
  F7 RP file query + verify; F8 selective restore; F9 Desktop UX
  Live manual matrix: unauthorized browse, token expiry, source frozen, restart, schedule fire
```

## 20. F7 交付记录

```text
Work package: F7
Owner: agent (recovery point file query + file_set verify)
Baseline commit: (working tree after F6)
Files owned/changed:
  src/apps/service/src/file_recovery_point_query.h|.cpp (new: ListRecoveryPointEntries)
  src/apps/service/src/service_host.cpp (kind 14 dispatch + file.recover_browse capability gate)
  src/apps/service/src/service_main.cpp (capabilities: file.recover_browse, recovery_point.verify)
  src/apps/service/src/worker_job_service.cpp (prepare_verify content_kind from Catalog V2)
  src/apps/service/CMakeLists.txt
  src/apps/worker/include|src/personal_file_archive_verify_task.* (full stream payload verify)
  src/apps/worker/src/worker_host.cpp (file_set verify path)
  src/apps/worker/CMakeLists.txt
  docs/modules/service_host.md, worker_host.md, windows_file_set_backup.md
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md
Public contracts changed: none (wire already V4); runtime enables file.recover_browse + recovery_point.verify
Production targets built: aegra_app_service, aegra_service, aegra_app_worker_personal, aegra_personal_worker
Architecture/static checks: production compile with /WX (vs2026-debug)
Manual validation performed:
  Build of service + personal worker targets
  Live ListRecoveryPointEntries / StartVerify on file_set RP requires elevated Service + sample archive
Security/path cases reviewed:
  No Archive path or stream offset on wire; display_name is ASCII projection of UTF-16LE
  Credential errors map to file_recover.credential_required|failed|corrupt; volume_set -> content_kind_mismatch
  Continuation bound to index_generation + parent_entry_id
Format/protocol docs updated: module docs for F7 host/worker behavior
Known remaining work (only outside this package):
  F8 selective file restore; F9 Desktop UX
  Live matrix: large directory paging, wrong password, generation change, full stream verify
```

## 21. F8 交付记录

```text
Work package: F8
Owner: agent (selective file restore Prepare/Start/Worker)
Baseline commit: (working tree after F7)
Files owned/changed:
  src/ports/include/aegra/ports/control_plane.h (schema 12; RestorePreflightRecord.entry_ids)
  src/adapters/sqlite/* (restore_preflight_entry_ids table; insert/load entry_ids)
  src/apps/service/src/worker_job_service_file_restore.cpp (PrepareFileRestore + StartFileRestore)
  src/apps/service/include|src/worker_job_service.* (file_browse + prepare/start methods)
  src/apps/service/src/service_host.cpp (kind 15/48 dispatch)
  src/apps/service/src/service_main.cpp (capability file.restore when browse available)
  src/apps/service/CMakeLists.txt (AdapterWindowsFilesystem on app_service)
  src/apps/worker/include|src/personal_file_archive_restore_task.* (V7 reader + tree sink pipeline)
  src/apps/worker/src/worker_host.cpp (file_set restore dispatch)
  docs/modules/control_plane_sqlite.md, service_host.md, worker_host.md, adapters.md,
    windows_file_set_backup.md
  docs/protocol/SERVICE_CONTROL_PROTOCOL_V4.md (file.restore enabled)
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md
Public contracts changed: control-plane schema 12; runtime enables file.restore
Production targets built: aegra_adapter_sqlite, aegra_app_service, aegra_service,
  aegra_app_worker_personal, aegra_personal_worker (vs2026-debug)
Architecture/static checks: production compile with /WX
Manual validation performed:
  Debug build of service + personal worker library and executable
  Live Prepare/Start restore path requires elevated Service + sample file_set archive
Security/path cases reviewed:
  Durable preflight stores target_root_identity (f1|volume|path_blob) and entry_ids, not absolute
    Archive/target paths or secrets
  chain_fingerprint prefix filec| separates file restore from volume preflight tokens
  Start revalidates index_root_digest and unique preflight token occupancy
  Worker re-resolves volume GUID identity before opening WindowsFileTreeSink
Format/protocol docs updated: schema 12 + file.restore capability notes
Known remaining work (only outside this package at F8 close):
  F9 Desktop UX (completed in subsequent package)
  Live matrix: single/multi file, subtree, ACL/ADS, collision policies, disk full, cancel, partial
```

## 22. F9 交付记录

```text
Work package: F9
Owner: agent (Desktop file backup/restore UX on Service V4)
Baseline commit: (working tree after F8)
Files owned/changed:
  src/apps/desktop/src/client/service_client.h|.cpp (file models, capabilities, expand hooks)
  src/apps/desktop/src/client/service_client_file.cpp
  src/apps/desktop/src/client/service_protocol.h|.cpp|commands|file
  src/apps/desktop/src/client/models/file_browse_model.*
  src/apps/desktop/src/client/models/file_recover_model.*
  src/apps/desktop/src/client/models/recovery_point_model.* (contentKind)
  src/apps/desktop/qml/pages/BackupPage.qml (files mode lazy tree + createFileSetSchedule)
  src/apps/desktop/qml/pages/RestorePage.qml (file_set recover tree + target + startFileRestore)
  src/apps/desktop/src/locale/message_code_map.cpp
  src/apps/desktop/translations/* (generate_ts + five locales)
  src/apps/desktop/CMakeLists.txt
  docs/modules/desktop.md
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md
Public contracts changed: none (consumes Service V4 kinds 13/14/15/48)
Production targets built: aegra_desktop (vs2026-debug)
Architecture/static checks: production compile with /WX; source size limits passed
Manual validation performed:
  Debug build of aegra_desktop
  Live Service stop/restart, paged browse, backup/restore success paths require elevated Service
Security/path cases reviewed:
  FileBrowseModel/FileRecoverModel store tokens/entry IDs only
  No QDir/QFileInfo enumeration of protect sources or archives in Desktop client
  Schedule selection_summaries parse display labels only (no absolute paths on wire)
Known remaining work (outside this package):
  Live elevated Service matrix (backup/restore/verify on isolated data)
  Live UI matrix screenshots at 900x600 / 1080x720 / 150% DPI
```

## 23. F10 交付记录

```text
Work package: F10
Owner: agent (file_set release gate / docs closure)
Baseline commit: c8e0c80 (working tree F0–F9 + gate)
Files owned/changed:
  docs/development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md (F10 已完成 + 本记录)
  docs/architecture/FILE_SET_BACKUP_RESTORE.md (status)
  docs/modules/windows_file_set_backup.md
  docs/modules/desktop.md
  docs/modules/service_host.md
  docs/modules/pipeline.md
  docs/modules/README.md
  docs/requirements/PRODUCT_SCOPE.md (scope already V7/V4/file_set; no code change)
Public contracts changed: none (gate only)
Production targets built:
  cmd.exe /d /c scripts\build.cmd Debug   → exit 0
  cmd.exe /d /c scripts\build.cmd Release → exit 0
  (includes CheckSourceLimits via CMake; source size limits passed)
Architecture/static checks:
  git diff --check → exit 0 (CRLF warnings only, no whitespace errors)
  Desktop QDir: only main.cpp temp lock path; no QFileInfo / no protect-source enumeration
  base/contracts/ports/pipeline: no Win32/Qt/JSON includes (comment-only “Win32” mentions OK)
  format: nlohmann JSON only in codec .cpp (pre-existing Manifest/File Index codec pattern)
  Compat keyword audit (V6/schema 3/Catalog V1/legacy/migration/fallback/alias):
    - no V6 dual-read, V3/V4 negotiation, schema migration, or format conversion paths
    - open_legacy_repository: CLI LocalObjectStorage open-existing only (name is not V6 compat)
    - restore inventory disk.N synthetic: volume inventory addressability, not format compat
    - file_set path: explicit no raw VSS fallback (ADR-0016)
  Secret/log audit: password fields log empty|present or layer counts only; no SecretRef material
    or security descriptors in ordinary logs
Manual validation performed:
  Full Debug + Release production builds of all targets
  Live elevated Service matrix deferred (requires admin Service + isolated sample data)
Security/path cases reviewed:
  Desktop file models: opaque tokens/entry IDs only
  Control-plane file selections: no absolute paths on wire
  Unreleased product: reject wrong format/protocol/schema versions (no dual-read)
Format/protocol docs updated: status pointers only; V7/V2/V4 remain authoritative
Known remaining work (outside this package):
  FI0-FI10 file-level Incremental implementation (ADR-0018 accepted)
  Live Volume + File artificial validation matrix on isolated hosts
  Live five-locale UI matrix at 900x600 / 1080x720 / 150% DPI
```
