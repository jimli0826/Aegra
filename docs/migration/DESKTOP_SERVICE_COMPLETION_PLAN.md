# Desktop 迁移与个人版 Service 完成计划

> 2026-08-04 验证策略更新：根据 [ADR-0015](../adr/0015-no-project-test-suite.md)，本文所有要求新增、维护或
> 运行测试源码、测试 Target、CTest、E2E 脚本或测试证据的条目均已废止，不再构成工作包门禁。后续工作以
> 生产 Target 构建、静态/架构检查和必要的人工运行或 UI 验证为准。Repository connection 的 `Test` 命令是
> 产品业务能力，不属于测试用例。

## 1. 文档目的

本文把旧项目 `backup/src/gui` 中仍有价值的 Desktop 体验迁移到 Aegra，并收敛个人版 Service 剩余控制面工作。
它是后续 agent 领取任务、确定文件所有权和验收合并的执行依据。

当前基线为阶段 13C + D0/D1/S0-S4：Desktop 已能通过 schema 3 Named Pipe 与 Service 握手、自动重连并分页
查询个人版 Recovery Point；Repository 页面、无边框窗口、折叠侧栏和 `blueExtra` 主题已迁移。Service 已接入
SQLite 控制面、Inventory、Repository connection/query API 和单任务 Worker Supervisor；计划触发、完整事件查询、
Verify/Restore/Mount Service 编排与对应 Desktop 页面仍待后续工作包完成。

工作包状态最后更新于 2026-08-04。状态定义如下：

- `已完成`：满足本工作包 Definition of Done，并已同步实现、验证和文档；
- `进行中`：已有 owner 领取并产生实现工作，尚未完成全部验收；
- `可开始`：全部前置工作包已完成，可以立即领取；
- `等待前置`：至少一个前置工作包尚未完成；
- `阻塞`：前置已满足，但存在需要明确解决的技术或外部条件阻塞。

第 5 节总览表是工作包状态的唯一权威来源。Agent 开始或完成工作时必须同步更新该表；不能仅根据代码文件
存在就把工作包标记为已完成。

本计划不要求兼容未发布旧项目的 UI Backend、通信协议、配置或数据。旧 `backup/src/gui` 的 UI 显示效果、
页面结构、视觉密度和主要交互路径是 Desktop 的强制基线，不是可选参考；只有数据来源、后端调用和安全边界改为
新版 Service IPC。

## 2. 目标与边界

### 2.1 目标

- Desktop 显示效果必须与旧项目 `D:\Work\OpenSource\backup\src\gui` 保持一致，包括窗口 shell、侧栏、
  页面结构、视觉密度、卡片/表格/抽屉样式和主要操作路径，同时完全改用新版 Service IPC。
- Desktop 首批支持 `en_US`、`zh_CN`、`zh_TW`、`ja_JP` 和 `de_DE`，并可在运行时切换语言。
- Service 成为个人版唯一控制面，负责 Repository 连接、任务、计划、事件和 Worker 生命周期。
- 形成 Backup、Verify、Restore 和 Mount 的 `Desktop -> Service -> Worker/Mount Host -> Repository` 闭环。
- 每个工作包有独立文件边界、前置条件、验证和 Definition of Done，可由不同 agent 顺序或并行完成。

### 2.2 硬边界

- 生产逻辑使用 C++20；Qt 只能存在于 `src/apps/desktop`。
- Desktop 不链接或直连 Repository、Worker、SQLite、Windows VSS、Volume、Dokan 或凭据存储。
- Service 返回稳定枚举、错误码和 `message_code`，不返回本地化展示文本。
- Service 只负责任务编排和权威控制面状态；备份、校验和恢复数据面继续由 Worker 执行。
- 明文密码不得进入 IPC、数据库、日志或 QML 状态；跨进程只传递 `SecretRef`。
- 个人版控制面使用 SQLite；PostgreSQL 和全局去重 CAS 属于企业版，不进入本计划。
- `.bkf`、Recovery Point 和 Chunk Index 的权威内容不存入控制面数据库。SQLite 只保存可重建摘要、连接、
  任务、计划、事件和本地偏好引用。
- 不复制旧 Backend、HTTP/WebSocket、TLS 绕过、旧协议兼容层或 GUI 内业务执行代码。

## 3. 目标结构

```text
Qt/QML Desktop
    | versioned local IPC: commands, queries, task events
    v
Windows Management Service
    |-- Application use cases
    |-- SQLite control-plane adapter
    |-- Personal Repository query/planning ports
    |-- Worker supervisor ------> one-task Worker processes
    |-- Mount supervisor  ------> isolated Mount Host
    `-- Schedule/event services
```

协议 DTO 位于 `contracts`，用例位于 `application`，外部能力抽象位于 `ports`，SQLite、Windows Service、
Named Pipe ACL 和进程启动等实现位于 `adapters` 或对应 `apps` 组合根。Service Host 只做 framing、校验、
dispatch、会话生命周期和错误映射。

## 4. Desktop 国际化规范

国际化基础是所有未迁移页面的前置条件，不允许先写硬编码可见文本再集中返工。

### 4.1 技术方案

- 使用 Qt Linguist `.ts` 源文件和构建生成的 `.qm`，不迁移旧 `I18n.qml` JavaScript 字典。
- QML 可见文本使用稳定 `qsTrId("aegra.<area>.<meaning>")`；Desktop C++ 使用 `qtTrId`。
- `en_US` 是源语言，首批提供 `zh_CN`、`zh_TW`、`ja_JP` 和 `de_DE` 翻译。
- Desktop 的 `LocaleController` 负责发现语言、加载 `QTranslator`、暴露当前语言并调用
  `QQmlEngine::retranslate()`。
- 首次启动使用系统 locale；用户显式选择后写入 Desktop 本地 `QSettings`。语言是 UI 偏好，不增加 Service API。
- 日期、时间、数字、百分比和容量通过 `QLocale` 格式化，不在 QML 中拼接英文单位或固定日期格式。
- Service `message_code` 由 Desktop 中的集中映射表转换为翻译 ID；未知 code 使用通用安全文本并记录 code，
  不展示 Service 原始异常。
- 名称、用户路径、Repository 标签等用户数据不得送入翻译函数；翻译参数使用占位符。

### 4.2 翻译 ID 规则

```text
aegra.nav.backup
aegra.backup.action.start
aegra.task.state.running
aegra.error.repository.not_configured
```

- ID 表达语义，不包含英文原文、页面位置、颜色或控件类型。
- 通用动作和状态只定义一次；页面专用文本归属页面命名空间。
- 删除或改义 ID 视为 Desktop 内部契约变更，由国际化 owner 统一处理。
- QML `objectName`、测试 ID、协议枚举和 `message_code` 不翻译。

### 4.3 国际化验收

- 五种语言都能在不重启 Service 的情况下切换，当前页面即时重译并持久化选择。
- 900x600、1080x720、150% DPI 下无文字截断、重叠和操作按钮位移。
- 增加伪本地化或最长字符串布局测试，覆盖导航、对话框、表格空状态和任务进度。
- CI 检查重复翻译 ID、缺少源字符串、未纳入资源的 `.qm` 和新增 QML 硬编码可见文本。
- 翻译缺失时有确定性英文回退；翻译文件损坏或不存在不影响 Desktop 启动。

### 4.4 旧 Desktop 迁移清单

| 旧界面或组件                                                             | 处理方式                                                   | 归属工作包      |
| ------------------------------------------------------------------ | ------------------------------------------------------ | ---------- |
| `Main.qml`、`Theme.qml`、窗口/侧栏                                       | 阶段 13C 已迁移基线；只由 integration owner 扩展导航和全局状态            | D0, D2, D8 |
| `RepositoryPage.qml`                                               | 已迁移只读页面；补连接、Verify、Delete 等真实操作                        | D5         |
| `HomePage.qml`                                                     | 按摘要、近期任务和快捷操作拆分重写                                      | D2         |
| `BackupPage.qml`                                                   | 按 source、target、options、schedule、confirm、progress 拆分重写 | D3         |
| `RestorePage.qml`                                                  | 按 recovery point、chain、target、confirm、progress 拆分重写    | D4         |
| `MountPage.qml`                                                    | 按 mount request 和 mounted session 列表拆分重写               | D6         |
| `EventLogPage.qml`                                                 | 按 filter、paged table 和 detail drawer 拆分重写              | D7         |
| `SettingsPage.qml`                                                 | 按 appearance、locale、service 和 repository settings 拆分重写 | D8         |
| `SplashScreen`、`LoadingOverlay`、`ToastBanner`                      | 建立无业务依赖的共享反馈组件                                         | D2         |
| `Card`、`ComboBoxIndicator`、`DatePickerField`、`LinkButton`          | 仅在页面确实需要时迁移视觉行为，并完成 i18n/键盘/无障碍                        | 页面 owner   |
| `DiskChart`、`DiskIcon`、`DriveItem`                                 | 改为只消费 Service Inventory model 的展示组件                    | D3, D4     |
| `BackupPasswordDialog`                                             | 不传递密码；改为创建或选择 `SecretRef` 的受控交互                        | D3         |
| `AddLocationPanel`、`AddRepositoryPanel`、`LocationManagementDialog` | 合并为新版 Repository connection 工作流                        | D5         |

旧 `LogsPage`、`ManagePage`、`SyncPage` 和 `ToolsPage` 不属于当前个人版导航，不自动迁移。只有产品范围和对应
Service capability 经过单独设计后才能加入计划。旧 C++ Backend、`TaskWebSocket`、`I18n.qml`、qmake 文件、
Makefile、`release/` 生成物和 `.bak` 文件全部禁止迁移。

### 4.5 旧 UI 显示一致性强制门禁

- 每个 Desktop 页面和共享 shell 开工前必须运行或检查旧 `D:\Work\OpenSource\backup\src\gui`，保存旧页面基线截图；
  不能只阅读旧 QML 后凭印象重做。旧截图必须覆盖 900x600、1080x720、150% DPI 和默认 `blueExtra` 主题。
- 新 Desktop 的首屏显示必须与旧 UI 对齐：32px 无边框标题栏、产品图标/标题/版本位置、160px/56px 可折叠侧栏、
  深蓝 `blueExtra` 色板、12px 页面边距、3px 蓝色标题强调条、旧 Card 标题栏、36px 表头、44px 表行、
  90% 右侧抽屉、8px 弹窗圆角、按钮高度/颜色/hover、toast/loading/splash 的位置和动画节奏都必须保持同类表现。
- 允许为了新版 Service 边界替换数据源、禁用未接入操作、隐藏敏感路径/Secret、使用翻译 ID 和拆分组件；不允许因为
  重写 QML 而改成新的仪表盘风格、营销式布局、大块空白、不同配色、不同导航层级或不同卡片/表格密度。
- 旧 UI 有对应页面时，新页面必须按旧页面的视觉层级落位。Backup 仍是 schedules list + Add 按钮 + 右侧 90%
  slide-in wizard；Home 仍是 This PC / Tasks 两段密集工作台；Repository 仍是卡片列表 + 右侧 Recovery Point 抽屉。
  Service 暂缺的数据用 unavailable/empty/disabled 表达，但控件位置和视觉层级不得随意重排。
- 页面交付报告必须同时包含旧 UI 截图、新 UI 截图和差异说明。没有旧/新并排视觉证据，或者肉眼明显不像旧
  `backup/src/gui`，对应 Desktop 工作包不得标记为已完成，即使后端功能和测试已经通过。

## 5. 工作包总览

| ID  | 状态   | 优先级  | 工作包                        | 主要产物                                                    | 前置             |
| --- | ---- | ---- | -------------------------- | ------------------------------------------------------- | -------------- |
| D0  | 已完成  | P0   | Desktop 国际化基础              | Linguist、LocaleController、语言设置、门禁测试                     | 无              |
| D1  | 已完成  | P0   | Desktop 客户端分层              | transport、request coordinator、models、协议适配边界             | 无              |
| S0  | 已完成  | P0   | Service 协议 V3              | commands/queries/events DTO、codec、ADR                   | 无              |
| S1  | 已完成  | P1   | 正式 Windows Service 边界      | SCM、Pipe ACL、调用方身份、安装生命周期                               | S0             |
| S2  | 已完成  | P1   | SQLite 控制面                 | schema、repository connection、job/schedule/event stores  | S0             |
| S3  | 已完成  | P1   | Worker Supervisor          | 启动、进度、取消、崩溃回收、结果持久化                                     | S0, S2         |
| S4  | 已完成  | P1   | Inventory 与 Repository API | source inventory、connection/test、recovery point queries | S0, S2         |
| D2  | 已完成  | P1   | Home 与全局任务体验               | Home、Splash、toast、task model、旧 UI 视觉返工                  | D0, D1, S0     |
| D3  | 已完成  | P1   | Backup 页面                  | source/target/options、启动/取消/进度                          | D0, D1, S3, S4 |
| S5  | 进行中  | P2   | Archive 深度操作               | authenticate、verify、显式链解析、删除计划                          | S2, S3, S4     |
| S6  | 等待前置 | P2   | Restore 编排                 | preflight、目标检查、任务监督、结果查询                                | S3, S5         |
| D4  | 等待前置 | P2   | Restore 页面                 | recovery point、链、目标、确认、进度                               | D0, D1, S6     |
| D5  | 等待前置 | P2   | Repository 管理              | add/import/test/default/delete/verify actions           | D0, D1, S4, S5 |
| S7  | 进行中   | P3   | Mount Host 编排              | mounted session、unmount、崩溃清理                            | S3, S5         |
| D6  | 进行中   | P3   | Mount 页面                   | mount/unmount/session table                             | D0, D1, S7     |
| S8  | 进行中   | P3   | Schedule 与 Event/Audit     | trigger engine（已落地）、history、paged queries             | S2, S3         |
| D7  | 等待前置 | P3   | Event Log 页面               | filter、分页、详情、导出入口                                       | D0, D1, S8     |
| D8  | 可开始  | P3   | Settings 页面                | language、theme、service/repository settings              | D0, D1, S1, S4 |
| R0  | 等待前置 | Gate | 发布工程                       | installer、upgrade/uninstall、recovery、diagnostics、E2E    | 全部发布范围         |

## 6. P0 基础工作包

### D0：Desktop 国际化基础

**状态：已完成。**

**允许修改**：`src/apps/desktop` 的 locale C++、CMake、翻译资源和专属测试；必要时更新 Desktop 文档。

**实现内容**：

- 新增无全局可变状态的 `LocaleController`，由 Desktop composition root 构造并注入 QML。
- 配置 `qt_add_translations` 或等价 Qt 6 CMake 流程，打包五种 locale。
- 建立稳定翻译 ID 目录、Service message-code 映射和 `QLocale` 格式化 helper。
- Settings 尚未迁移前提供最小可测试语言切换入口；D8 再把入口放入正式页面。
- 添加缺失 locale、无效保存值、运行时切换、回退和 QML 重译测试。

**完成标准**：Repository 页面和窗口 shell 不再含可见硬编码字符串；五种语言可切换；布局门禁通过。

### D1：Desktop 客户端分层

**状态：已完成。**

**允许修改**：`src/apps/desktop/src` 中新增的 transport、protocol、coordinator、model 文件和相应测试。

**实现内容**：

- 把当前 `ServiceClient` 拆成 framing transport、协议适配边界、请求协调器和按领域划分的
  QAbstractItemModel；V3 字段只以 S0 Contracts 与 ADR-0013 为准。
- 每个请求有唯一 correlation ID、deadline、取消/断线语义；重连后只恢复幂等 query subscription。
- 模型只暴露拥有生命周期的数据；QML 不解析 JSON、枚举数值或 message code。
- 为 task event 建立单线程 Qt 投递边界，防止回调在对象销毁后更新模型。

**完成标准**：现有 Repository 行为不回归；类和文件规模符合工程标准；协议损坏、乱序、断线和重连测试通过；
V3 codec 只在 S0 contract 合并后由 integration owner 接入。

### S0：Service 协议 V3

**状态：已完成。** 后续协议扩展仍由明确的 protocol owner 修改 Service 公共协议文件。

**实现内容**：

- 用 ADR 定义 V3 envelope、capabilities、request/response、command acknowledgement、分页和 task event。
- 定义 Repository connection、inventory、job、schedule、event、restore、verify 和 mount 的稳定 DTO/枚举。
- 定义幂等 command key、版本协商、最大 frame、分页 token、订阅恢复和 backpressure 行为。
- 旧 schema 2 尚未发布，直接替换，不增加双协议兼容路径。
- Service 只返回 `message_code` 和结构化参数；所有错误映射均测试。

**完成标准**：Contracts、codec 和 Host contract tests 完整；边界类型不含 Qt、JSON、Win32、SQLite 或 STL ABI 暴露。

## 7. P1 备份闭环工作包

### S1：Windows Service 与 IPC 安全

**状态：已完成。** 正式 Composition Root 已接入 SCM `ServiceMain`、授权 Host 和 Service ACL。决策见
[ADR-0014](../adr/0014-windows-service-ipc-security.md)。

- 提供 SCM 安装、启动、停止、恢复策略和受控卸载入口（`IWindowsServiceControlManager` + Fake/Win32）。
- 使用显式本地 Pipe ACL，拒绝远程 client，校验调用方 session/SID 和授权策略。
- Service 运行身份可访问所需系统能力，但不把高权限句柄交给 Desktop。
- 停止流程取消 accept/会话并在 deadline 内收口；计划触发与受监督任务取消属 S3/S8。
- 测试覆盖授权成功、未授权、多 session、stop、restart 与 installer rollback。

### S2：个人版 SQLite 控制面

**状态：已完成。** Adapter/ports 已独立验证并接入 Service composition root。

- 已增加 `ports/control_plane.h` 与 `Aegra::AdapterSqlite`，保存 Repository connection、SecretRef、Job、
  Schedule、Event/Audit 和 schema version。
- 使用迁移事务、外键、唯一约束和显式 UTC 时间；Service 单写者，读操作有一致快照语义。
- Job 状态机至少包含 queued/running/cancelling/succeeded/failed/cancelled/interrupted。
- command store 持久化幂等键、请求指纹、command/resource ID；同键同请求返回 replay，不同请求返回冲突。
- Service 启动时把遗留 queued/running/cancelling 任务收敛为 interrupted
  （`mark_active_as_interrupted`），
  并由上层策略决定是否允许重新提交。
- 数据库删除后可由 Repository 与配置重新建立索引；不得把 DB 当作 `.bkf` 或 Recovery Point 的权威来源。
- 不保存密码、Chunk Index、Manifest、完整 Archive metadata 或用户块数据。
- 详见 [control_plane_sqlite.md](../modules/control_plane_sqlite.md)。

### S3：Worker Supervisor

**状态：已完成。**

- 从受信任配置构造 Worker 命令行和随机 Pipe 名；默认 Worker 为 Service 可执行文件同目录的
  `aegra_personal_worker.exe`，`--worker-path` 可覆盖绝对路径。
- 完成 Job/Cancel、Progress/Result 会话，持久化状态转换和最终稳定结果。
- 支持 Desktop 取消、Service 停止、deadline、Worker crash、无 Result 退出和 Pipe 断开。
- 进程退出码只用于快速分类，权威结果来自合法 `WorkerResponse`；stderr 不作为协议。
- Service 重启后不尝试附着到未知 Worker；标记 interrupted，并清理遗留 IPC/临时状态。
- 每个 live Worker session 由一个拥有生命周期的 `std::jthread` 处理；Supervisor 序列化 submit/shutdown，
  会话完成后回收线程与进程状态。
- Worker listener 使用独立 `aegra-worker-*` namespace 和 1 MiB frame limit；真实 Worker 进程测试覆盖
  launch、协议交互、失败结果持久化与 session 回收。

### S4：Inventory 与 Repository Connection API

**状态：已完成。** Application/ports、Windows Inventory、Local Storage Factory、Host dispatch、capability 与
Service composition root 已接入。

- Inventory query 返回稳定 source ID、卷标签、容量、系统/只读/可选状态，不暴露可伪造的 Desktop 路径输入。
- Repository connection 支持 add/import/test/set-default/list/remove；持久化根定位符和 SecretRef，不保存明文凭据。
- Recovery Point list 继续从 Repository Catalog Scanner 获取，并带连接 ID；离线/未配置返回
  `not_configured`，损坏 catalog 返回结构化失败。
- 删除连接只删除控制面引用，不删除 `.bkf`。
- Application 测试覆盖未配置、离线、损坏 catalog、取消、分页与多 Repository。
- Repository descriptor 读取允许重复短读并限制为 1 MiB；source ID 解析为 Adapter 内部受信任稳定 key，
  重复 source ID 拒绝启动任务。

### D2：Home、Splash 与全局任务体验

**状态：已完成（按生产功能范围评估）。** 功能接线与 Review 指出的协议/任务缺口已闭环：Backup/Restore/Mount 在对应能力接线前强制禁用；
进度 payload 校验 schema_version / job_id / trace_id / message_code 与 `processed_bytes <= logical_bytes`；
进度百分比溢出安全；首次 Job 快照仅建立 toast 基线；Toast 使用可重启单一定时器；
超时、断线重查、轮询无重叠、Splash Retry、toast 去重均已具备。Home、Splash、Toast、Loading、Shell、
真实 Job 分页/轮询、全局任务状态和导航均已接入生产路径。本状态只评价产品功能是否具备；项目不再以测试
用例或视觉证据矩阵作为阶段完成条件，验证遵循 ADR-0015 和当前工程规范。

**必须完成的范围：**

- 必须新增真实 `JobModel`，通过 Service V3 `job.list` 分页读取 SQLite 中的权威 Job 摘要；必须支持
  continuation token、去重、最大结果数、断线清理和整批原子发布。QML 不得解析 JSON、message code 或枚举值。
- 必须让 `ServiceClient` 同时管理 Repository query 与 Job query，不得因单个 pending request 假设导致刷新互相
  阻塞、响应串线或丢失。请求 ID、deadline、取消、断线和重连行为必须继续由 coordinator 管理。
- 必须实现可用的 Home 页面，不是占位页，并且显示结构必须对齐旧 `HomePage.qml`：上半部保持 This PC
  信息/磁盘摘要卡片，下半部保持 Tasks 表格卡片，沿用旧 Card、表头、表行、进度条、More 链接和取消确认样式。
  Service 没有系统/磁盘容量数据时必须在旧布局位置显示 unavailable/empty/error，禁止改成新的 dashboard
  卡片网格、硬编码 `0`、静态数字或假数据伪装成功。
- 必须实现启动 Splash/遮罩状态机：只覆盖首次 handshake；成功后自动退出；超时、Service 未启动、协议拒绝或
  连接失败时进入带 Retry 的可恢复错误状态。禁止无限 loading、固定延时后假装成功或在后台错误时保持 Ready。
- 必须提供全局任务入口、任务列表、确定尺寸的 progress 展示、toast 和 loading overlay。当前 Service 尚未实现
  task event subscription，因此必须以 `job.list` 为权威，并对 queued/running/cancelling Job 做无重叠的有界轮询；
  不得伪造 event、在 QML 自行推进进度或宣称已接入 subscription。
- 必须对 terminal Job toast 做 `job_id + terminal state` 去重；重连和刷新不得重复弹出同一完成通知。所有 toast、
  状态、按钮和空态文本必须使用翻译 ID，并补齐五种 locale。
- 必须完成 Home 与 Repository 的真实导航切换；当前 `Main.qml` 不能继续固定只渲染 Repository 页面。缺少后端
  capability 的 Backup/Restore/Mount 等入口必须禁用或隐藏，但现有 Repository 页面不得回归。
- 键盘焦点、accessible name、900x600 最小窗口、1080x720、150% DPI 和五种 locale 的显示质量继续按
  第 4.5 节维护；旧/新并排截图属于非阻塞视觉质量证据，不参与 D2 功能完成判定。

**历史验证清单（不参与当前功能完成判定）：**

- Job codec/model：空页、单页、多页、重复 Job ID、token 不前进、超过累计上限、乱序响应和错误 payload。
- ServiceClient：Repository 与 Job 请求调度、刷新去重、deadline、断线、重连重查、Service unavailable 与 Retry。
- UI 状态：首次启动成功/失败、loading/empty/error/ready、running/cancelling/terminal Job、toast 去重、
  capability gate，以及旧 Home/Tasks/Splash/Toast/Loading/Shell 显示一致性。
- 当前项目不保留或运行测试用例；生产变更按工程规范执行 Debug/Release 构建、静态检查和聚焦人工验证。

**以下情况一律不算完成：** 只有静态 QML；使用硬编码演示数据；只测试 fake model 而没有真实 Service frame；
只支持成功路径；绕过 Service 读取 SQLite；或留下无法工作的按钮/导航。旧 UI parity 和截图矩阵作为后续视觉质量
工作，不影响 D2 生产功能完成状态。

### D3：Backup 页面

**状态：已完成（按生产功能范围评估）。** 已落地：Desktop Inventory/Connection codec 与 model、StartBackup/CancelJob
ServiceClient 门面（幂等 key、active job 观察、断线保留 job_id）、`BackupPage.qml` 表单分区
（source/target/options/schedule/credential 说明/confirm/progress）、五语言翻译、Main/Sidebar/Home 导航。
用户可从真实 Inventory 选择一个或多个 Volume，或选择 Disk 展开其全部 Volume；可选择 Repository、创建和运行
Schedule、启动一个多 Volume Backup Job、观察聚合进度并取消。VSS 与 raw fallback 由 Worker 决定。本状态只评价
生产功能是否具备；测试用例与视觉证据不作为阶段完成条件。

**必须完成的范围：**

- 必须把旧 Backup 页面按新 Desktop 边界重写为可维护组件，但显示结构必须与旧 `BackupPage.qml` 一致：
  主视图保持 schedules Card/table + 右上 Add 按钮，Add 打开 90% 宽右侧 slide-in wizard，wizard 保持
  source/destination、schedule/options、add location 的旧视觉层级、标题栏、scrim、按钮和表单密度。不得迁移旧
  Backend、HTTP/WebSocket、磁盘/VSS 直连、巨型 QML 或硬编码演示数据；不得改成全新的多卡片向导或营销式页面。
  单个 QML/C++ 文件仍受源码规模门禁约束。
- 必须新增或扩展真实 Desktop model/codec 来消费 Service V3 Inventory、Repository connection、Job 和 Backup
  command 响应。QML 只绑定领域 model 角色，不解析 JSON、枚举数值、message code、SecretRef、设备路径或
  Repository locator。
- Source 只能来自 Service Inventory model 的稳定 source ID，并展示 label、容量、可选/只读/系统等 Service
  已授权摘要。Desktop 不得让用户输入任意 Windows 路径、Volume GUID path、盘符或设备名来启动备份；无法备份的
  source 必须禁用并给出本地化原因。
- Target 只能来自 Repository connection model。未配置、离线、测试失败或 capability 不足时必须禁用 Start，
  并保留 Add/Import/Test 等尚未接线操作的禁用状态；不得直接读取本地目录或 SQLite。
- Backup options 必须支持全量备份的真实启动路径，并把 chunk、压缩、加密、deadline、trace 等受信任运行参数留在
  Service/Worker 配置中，不从 QML 提交。差异备份必须保持禁用；增量备份只有在 Service 返回明确的 eligible
  parent/capability 且 parent 属于同一 repository connection 与 source 时才能启用。
- Eligible parent 选择必须由 Service/Application 返回的受信任摘要驱动。Desktop 不得自行用 Recovery Point 列表拼链、
  猜测父 Archive path、提交对象 key、提交 Archive path 或把 Catalog 摘要成功解释为父点已认证成功；缺少能力时
  增量入口显示为 unavailable/disabled。
- 密码、口令创建和凭据选择必须通过受控 Credential/SecretRef 流程。QML model、Service command body、日志、
  toast、错误文本和测试 golden data 都不得包含明文 Secret；SecretRef 也不得作为可见 UI 文本展示。
- Start 必须发送 Service command，携带稳定幂等 key，并以 Service 返回的 `job_id` 进入观察状态。重复点击、重连或
  command replay 不得创建重复 Job；同一幂等 key 对不同请求必须形成冲突而不是静默覆盖。
- Backup 进度必须复用 D2 的全局 Job/Toast/Loading 规则，并在页面内展示 queued/running/cancelling/succeeded/
  failed/cancelled。页面内百分比、字节数和阶段文本只来自 validated Job/Progress model；不得用 QML 定时器自行推进。
- Cancel 必须调用 Service cancel command，并在收到 accepted 后进入 cancelling 过渡态；cancel 失败、job 已终止、
  Service 断线和 deadline 触发必须有稳定本地化状态。按钮状态不能出现 Start 与 Cancel 同时可用或 terminal 后仍可取消。
- Confirmation 必须展示 source、target、backup type、parent 摘要、估算/未知容量、credential 状态和不可逆提示；
  对缺失字段、能力不可用、credential required、repository offline、source disappeared、parent stale 等 preflight
  失败必须阻止提交。
- Schedule panel 必须通过 Service `schedule` capability 创建、读取、启用、禁用、删除和立即运行持久化 Schedule。
  一条 Schedule 保存完整有序 `source_ids[]`，运行时只创建一个 Job 和一个包含全部 Volume 的 Archive；Desktop
  不得写 QSettings、直接访问 SQLite、只保存首个 Source 或按 Volume 拆分 Schedule。
- 所有可见文本、状态、按钮、错误、空态和 accessible name 必须使用翻译 ID，补齐 `en_US`、`zh_CN`、`zh_TW`、
  `ja_JP`、`de_DE`；容量、时间和百分比必须通过现有 locale 格式化能力。
- 键盘焦点顺序、accessible name、900x600 最小窗口、1080x720、150% DPI 和五种 locale 的显示质量继续按
  第 4.5 节维护；旧/新并排截图属于非阻塞视觉质量证据。Backup/Repository/Home 导航不得回归。

**历史验证清单（不参与当前功能完成判定）：**

- Contract/codec/model：Inventory 空页/多 source、不可备份 source、Repository 未配置/离线、eligible parent
  空/多项/stale、Backup command accepted/replayed/conflict、Start failure、Cancel accepted/failure 和 malformed
  payload 拒绝。
- ServiceClient：StartBackup 与 CancelJob 的 request ID、deadline、幂等 key、断线清理、重连后 Job 观察、与
  Repository/Job 轮询并发时不串线、不丢响应、不重复提交。
- UI 状态：无 source、无 repository、credential required、full ready、incremental unavailable/ready、schedule
  disabled、confirmation blocked/ready、queued/running/cancelling/terminal、重试、toast 去重、capability gate，
  以及旧 Backup list/wizard 显示一致性。
- 安全与日志：明文 password/secret 字段拒绝；日志、model role、toast、message code mapping 和测试 golden
  输出不包含明文 Secret、Archive path、对象 key、设备路径或原始异常文本。
- 真实路径：至少一个测试必须经过真实 Service frame/command dispatch 并形成 SQLite Job 摘要，另一个覆盖真实
  cancel 到 Supervisor/Worker 边界或明确记录受环境限制的替代证据。
- 当前项目不保留或运行测试用例；生产变更按工程规范执行 Debug/Release 构建、静态检查和聚焦人工验证。

**以下情况一律不算完成：** 只有静态 QML；source/target 使用手写路径或假 model；Start 只在 UI 里切状态；
只支持成功提交不支持 replay/conflict/cancel；QML 接触 Secret 或解析 wire JSON；增量由 Desktop 自行拼父链；
schedule 看起来可用但没有 Service 能力；或留下可点击但无效的按钮。旧 UI parity 和截图矩阵作为后续视觉质量
工作，不影响 D3 生产功能完成状态。

## 8. P2 恢复闭环工作包

### S5：Archive 深度扫描、Verify 与删除计划

**状态：进行中。** Review P1 已修复：删除 tombstone 权威续跑、command intent 先于副作用、tombstone
内容冲突校验、Archive member generation 条件删除、pre-launch queued Job 启动收敛、Verify 不复用无映射
连接凭据，以及未完成 S5 capability 的 dispatch gate。仍缺：真实 Verify Worker E2E、per-file Archive
Credential 持久化映射表和 Local Storage 故障恢复门禁；完成前 capability 继续关闭。

**必须完成的范围：**

- 必须先补齐最小、版本化的 S5 Service contract，至少包含：受信任的 `repository_connection_id +
  recovery_point_id` 引用、链解析/可恢复性查询、`start verify`、删除计划查询和按 plan token 执行删除。Desktop
  不得提交 Archive path、对象 key、链数组或 Secret。现有 `StartVerify = ResourceRef` 如果无法表达连接归属，必须在
  未发布协议中直接更正并同步 codec、validator、contract test 和 ADR/协议文档，禁止增加兼容 payload。
- 必须实现 Application 用例，而不是把业务逻辑写进 `service_host.cpp`：按 connection 打开 Repository、扫描并定位
  Recovery Point、构建 `RecoveryPointGraph`、返回 base-first 显式链，并稳定区分 not found、offline、corrupt、
  incomplete、credential required、cancelled 和 internal。
- 必须验证重复 UUID、自环、环、跨 Backup Set、缺父、parent mismatch、非法 backup type 和最大链深度；Catalog
  摘要成功不能把 authentication 或 verification 标为成功。Restore/Mount eligibility 只能由 structural、
  authentication 与 chain 三个独立状态共同决定。
- 必须真正打通 `Service request -> Application resolution -> WorkerJobService -> WorkerSupervisor ->
  aegra_personal_worker Verify task -> SQLite terminal Job`。不得只返回 command accepted、只写一条 Job 或使用 fake
  Worker 代替 composition。Worker source 必须由 Service/Adapter 从受信任 locator 和已验证 Repository-relative key
  构造，Application 不得手工拼接 Windows 路径。
- 必须定义 Archive CredentialRef 选择规则并实现测试。当前 Service 创建的 Archive 可以使用持久化的 Repository
  credential；导入 Archive 若没有明确映射必须返回 `credential_required`，禁止尝试空口令、复用无关连接凭据或把
  SecretRef/明文放进响应、日志、Job message 和错误文本。
- 必须实现 `personal_repository` 删除计划核心：选择一个 Recovery Point 后计算所有受影响后代、后代优先顺序、
  Catalog generation、Archive 主卷/分卷/Sidecar 成员和稳定 operation UUID。计划结果必须由 Service 返回可展示摘要，
  不能让 Desktop 自己推导影响范围。
- 必须实现 plan token 的持久化或等价的可重放验证。执行删除时必须重新扫描并校验 repository UUID、目标集合、
  generation 和链图；任何变化必须返回 conflict 并要求重新计划。执行命令必须同时要求 plan token、幂等 key 和
  显式确认字段，缺一不可。
- 必须按 personal repository 规则执行删除：先发布 Deletion Tombstone，再按后代优先删除 Archive members 和
  Catalog entry；对象不存在视为幂等成功，单对象 `kOutcomeUnknown` 必须通过属性查询对账，部分失败必须可用同一
  operation 继续。失败或重启后 Catalog 查询不得把已 tombstone 的点继续显示为可恢复。
- 必须扩展最小 Storage Port/Factory 能力以支持 staged tombstone、conditional publish 和 idempotent delete；
  `personal_repository` 不得依赖 Local Storage/Windows/SQLite，实现层不得把万能 Storage Backend 引入 Application。
- 必须接入 Service composition、capability、Host dispatch、SQLite command/job/audit 投影和模块文档。Service
  重启后必须能看到 Verify terminal state，并能继续未完成的删除 operation；不得依赖进程内 map 作为唯一状态。

**必须覆盖的测试：**

- Contract/codec：所有新增 request/response、错误 payload、幂等 replay、同 key 不同请求冲突和未知字段拒绝。
- Chain：full/incremental、分叉、断链、环、跨 set、重复 UUID、parent mismatch、最大深度和取消。
- Verify：正确/错误 credential、offline、损坏 Header/Footer/payload、缺卷、取消、deadline、Worker crash、无 Result、
  Service 重启后的 terminal Job 查询，以及至少一个真实 Service -> Worker 进程测试。
- Delete plan/execute：叶子、带后代、整链、generation 变化、重复执行、对象不存在、tombstone publish 失败、成员
  部分删除、`kOutcomeUnknown` 对账、重启继续和 Catalog 重建后隐藏 deleting/deleted 点。
- 受影响 target 的 Debug/Release 构建、全套 `ctest`、`architecture.source_limits`、格式与 `git diff --check`
  必须全部通过。

**以下情况一律不算完成：** 只实现 chain helper；只让 `StartVerify` 返回 accepted；只做 fake-process 测试；
删除时直接 `Remove-Item`；Desktop 可传路径/key；删除计划不校验 generation；失败后留下可恢复假象；未接
composition/capability；或只跑 Debug/单个测试。

### S6：Restore 编排

**状态：等待前置 S5，详细设计已确定。** 当前已有 Restore V3 DTO、durable preflight record/store、
`RestorePreflightService` 编排边界、Restore Worker/Pipeline、`WindowsBlockSink` 和 S3 Supervisor。仍缺生产可用的
S5 链认证/逐层 Credential 映射、可信 Archive/目标解析、Start 编排、TOCTOU 重验证和 composition/capability。
这些能力全部闭环前，`restore.preflight` / `restore.start` capability 必须保持关闭。

**任务目标：** 完成非系统 Windows Volume 的在线 Restore 控制面闭环：Desktop 只提交受信任的
`repository_connection_id + recovery_point_id + target_source_id`，Service 生成短期 preflight，用户显式确认后
启动一个 durable Restore Job，真实 Worker 使用完整 base-first Archive 链写入目标卷。S6 不恢复系统卷、不创建或
修改分区、不支持 PhysicalDrive、不实现 WinPE/裸机恢复，也不允许 Desktop 提交 Archive path、Repository key、
Volume GUID、链数组、SecretRef 或任意设备路径。

**开始前必须完成：**

1. 完整阅读 `CPP_ENGINEERING_STANDARD.md`、`MODULAR_ARCHITECTURE.md`、`contracts.md`、`ports.md`、
   `application.md`、`service_host.md`、`worker_host.md`、`windows_personal_restore.md`、`windows_adapters.md`、
   `control_plane_sqlite.md`、ADR-0008、ADR-0009、ADR-0013 和 Personal Repository/Backup Format 文档。
2. 列出现有可复用能力及其明确缺口：S5 chain/authentication、Archive Credential mapping、
   `SourceInventoryQuery`、`WorkerJobService`、`WorkerSupervisor`、`PersonalArchiveChainReader`、
   `PersonalArchiveRestoreTask`、`WindowsBlockSink`、Restore Pipeline 和 SQLite Job Store。禁止复制 Pipeline、
   在 `service_host.cpp` 拼链，或另建一套 Restore Worker 协议。
3. 先形成 contract、port、SQLite schema 和 composition 变更清单。产品未发布，发现 V3 Restore DTO 不足时
   直接修正 schema、codec、validator、ADR 和文档，不增加旧 payload fallback。

**必须完成的范围：**

- 必须修正最小 Restore contract。`RestorePreflightRequest` 至少包含 `repository_connection_id`、
  `recovery_point_id`、`target_source_id`；`RestorePreflight` 必须返回相同资源归属、逻辑大小、目标容量、链深度、
  过期时间和可展示的结构化 safety/eligibility 状态；`StartRestoreCommand` 必须包含 `preflight_token` 和显式
  `confirmed=true`。查询不携带幂等键，Start 命令必须携带幂等键。未知字段、空 ID、false confirmation、超限整数
  和错误 kind/payload 必须拒绝。Desktop 可见 DTO 不得包含路径、key、SecretRef 或底层错误文本。
- 必须新增 Application Restore 编排用例，而不是把业务逻辑塞进 Host 或 Worker Job Service。Prepare 流程按
  connection 打开 Repository，调用 S5 解析并认证完整 base-first 链，获取每层稳定 CredentialRef，解析目标
  Inventory ID，计算逻辑容量并返回结构化 preflight。稳定区分 not found、repository offline、chain incomplete、
  credential required、archive corrupt、target unavailable/system/read-only/too small、cancelled 和 internal。
- 必须为 `IRestoreChainInspector` 提供生产实现，复用 S5 的 Repository scanner、chain graph、Archive reader 和
  per-file Credential 映射。输出必须是已认证的 base-first 层描述、每层稳定 Archive key/generation、逻辑容量、
  Backup Set/父链身份和内部 CredentialRef；不得把这些内部字段返回 Desktop。
- 必须增加最小目标解析能力，把 opaque `target_source_id` 重新解析为当前 Inventory 记录和 canonical Volume GUID，
  同时返回 stable identity、容量、系统/只读状态及来源磁盘信息。解析器属于 Windows Adapter/Application 组合边界，
  Contracts、Ports 和 Application 不得依赖 Win32 类型或接受 Desktop 提供的设备路径。
- 必须实现短期、不可伪造且可重启读取的 durable preflight record。建议在 control-plane port/SQLite 中增加细粒度
  `IRestorePreflightStore`，保存 opaque random token、Repository/Recovery Point/target ID、Repository UUID、
  链身份或 generation 摘要、逻辑大小、目标容量快照、创建/过期时间；不得保存明文 Secret、SecretRef、
  Archive path、Volume GUID、Manifest 或 Chunk Index。token 默认 TTL 必须有界，过期后 Start 返回稳定 conflict
  并要求重新 Prepare。
- preflight 只是安全快照，不是写盘授权的永久事实。Start 必须重新读取 Repository connection、重扫并认证链、
  重新解析每层 CredentialRef、重新解析 target ID，并校验 Repository UUID、Recovery Point/链身份、generation、
  逻辑大小、目标 stable identity、容量和 safety 状态均未变化；任一变化返回 conflict，不得静默刷新旧 token。
- 必须保证一个 preflight 最多创建一个 Restore Job。SQLite 对非空 `jobs.preflight_token` 建立唯一约束，并提供按
  token 查询；queued Job intent 与 token 占用必须在 Worker launch 前提交。相同幂等键/同一请求返回同一 Job 的
  `Replayed`，同键不同请求或不同幂等键复用已占用 token 返回 Conflict；并发 Start 也只能有一个 winner。
- 必须由受信任 Service/Adapter 把 Repository-relative Archive keys 和 target source ID 转成 Worker 所需的
  base-first absolute Archive paths、逐层 CredentialRef 与 canonical Volume GUID。Application 和 Desktop 不得手工
  拼 Windows 路径；如果现有 Storage/Inventory port 无法安全表达该转换，只增加最小 resolver/inspector port，禁止
  引入万能 Storage Backend 或让核心模块依赖 Windows。
- 必须新增独立的 Restore command Application/Service 编排对象，负责读取 preflight、执行 Start 重验证、生成
  幂等 fingerprint、占用 token、构造 `WorkerJobRequest` 并提交 Supervisor。Host 只做 capability、协议校验和调用，
  不读取 SQLite、不扫描 Repository、不拼 Archive 路径。
- 必须复用 `WorkerSupervisor` 启动真实 `JobOperation::kRestore`：Job 的 `source_refs` 是完整 base-first 链，
  `credential_refs` 与层一一对应，`target_ref` 只来自 Service 解析结果；SQLite Job 保存
  `source_id = recovery_point_id`、`repository_connection_id`、`target_source_id`、`preflight_token` 和
  `idempotency_key`。command accepted 只表示 queued intent 已持久化，Worker launch 失败必须留下 terminal failed
  Job，Service crash 前的 queued/running/cancelling Job 启动时收敛为 interrupted，禁止自动重试破坏性 Restore。
- Worker 必须继续作为最终写盘安全边界。打开目标写句柄前重新认证全部层并验证 full-first、直接父链、Backup Set、
  Volume geometry 和最大链深度；重新拒绝系统卷、只读卷、容量不足、Archive 与目标同卷或来源卷无法确认；只有
  canonical Volume GUID 可以进入生产 Sink，且必须成功 lock + dismount 后才允许首个 write。Service preflight
  成功不能绕过 ADR-0009 的这些检查。
- 必须定义取消、deadline 和断线语义。Desktop 断线或窗口关闭不取消 accepted Job；显式 Cancel 复用 S3 command。
  首个 write 前取消不得修改目标；首个 write 后取消、I/O 失败、Worker crash 或 Service shutdown 必须形成稳定 terminal
  state 和 `restore.target_may_be_partial` warning/message，不能把目标重新标为安全或自动上线。成功以 Worker Result
  和 `FlushFileBuffers` 完成为唯一边界。
- 必须把 Worker 的稳定 `TaskResult` 映射到 SQLite Job 与 Audit Event，至少覆盖 accepted、preflight rejected、
  running、succeeded、failed、cancelled、interrupted 和 target-may-be-partial；日志、message arguments、Job、Audit
  和协议响应不得包含密码、密钥、Credential、SecretRef、Authorization、Cookie、令牌或其他认证材料。日志可记录
  诊断所需的 Archive path、Volume GUID 和其他用户数据；协议响应仍只返回受信任资源 ID 和稳定 message code。
- 必须接入 Service Host query/command handler、runtime composition、CMake、capability gate 和模块文档。
  `PrepareRestore` 与 `StartRestore` 要分别检查 capability，handler 未满足全部门禁时必须在调用前返回
  `service.capability_unavailable` 且零副作用；只有本节全部生产功能与人工安全验证完成后才能同时开放
  `restore.preflight` 与 `restore.start`。

**必须完成的生产验证：**

- 人工检查 Prepare/Start codec 的 strict keys、资源归属、confirmed、整数边界、token 过期、capability gate、
  幂等 replay 和同 key 不同请求冲突，确认 Desktop 无法提交路径、key、链或 SecretRef。
- 使用真实 Repository 人工覆盖 full 与多层增量链、不同层 CredentialRef、缺父/环/跨 set、generation 漂移、
  错误凭据、Repository offline、Archive corrupt，以及目标 missing/system/read-only/too small/identity changed。
- 使用隔离的非系统 VHD 或专用测试卷执行一次真实成功路径：`Service request -> preflight -> start -> Supervisor ->
  production Worker -> Restore task -> terminal SQLite Job`，并验证恢复后数据可读、Job 指标正确、目标已 flush。
- 人工覆盖首写前取消、首写后取消、I/O 失败、Worker crash、Service restart 和重复 Start；确认 destructive failure
  返回 `restore.target_may_be_partial`，且不会自动重试或把目标重新标为安全。
- 使用 VS 2026 Insiders 完成 Debug 与 Release 生产构建，运行源码规模、格式、架构边界、秘密扫描和
  `git diff --check`。遵循项目测试策略，不新增或运行项目测试用例、CTest、测试脚本或测试 executable。

**文件所有权：** S6 agent 独占新增 Restore Application use case、preflight port/store、专属 SQLite 文件和
Restore Service handler。公共 Contracts/codec、`service_main.cpp`、Service/Worker 顶层 CMake 和 SQLite schema
registry 由 integration owner 统一接线；不得修改 Desktop QML，也不得覆盖 D2/D3/S5 的
并行改动。

**实施顺序是强制的：** entry gate/S5 验收 -> contract/ADR -> preflight port + SQLite -> Application preflight ->
Start 幂等与 queued intent -> trusted path/target resolution -> Supervisor/Worker 接线 ->
故障/重启恢复 -> 隔离 VHD 人工门禁 -> Debug/Release 全量验证 -> 文档/capability。禁止先开放 capability，
禁止先写 Host 大分支再补 Application，禁止用 fake success 代替真实 terminal Job。

**以下情况一律不算完成：** 只返回 preflight token；token 仅存在内存；Start 不显式确认；Desktop 可传路径、key、
链或 SecretRef；只在 Service 检查系统卷而 Worker 不复查；同 token 可创建多个 Job；accepted 后没有 durable queued
Job；断线取消任务；写入失败后仍显示目标安全；只验证 fake Worker；给生产 Worker 增加测试路径后门；未接
composition/capability；未完成 Release 构建或隔离非系统卷人工 Restore 门禁。

**交付报告必须包含：** 修改文件清单、contract/schema 变化、preflight TTL/重放/占用语义、目标身份与 TOCTOU
处理、完整错误/状态矩阵、真实进程和隔离 VHD 人工验证证据、Debug/Release 命令与结果、秘密扫描结果和剩余项。
本节任一“必须”条目缺失时，S6 状态必须保持进行中或等待前置，不得标记已完成。

### D4：Restore 页面

**状态：等待前置 S6。**

- 拆分 recovery-point picker、chain validation、target selector、危险操作确认和 progress/result。
- 认证、链完整性和 target safety 均由 Service 决定；QML 只展示结构化状态。
- 未通过 preflight 时禁止 Start；恢复中的窗口关闭不等于取消任务。

### D5：Repository 管理

**状态：等待前置 S4、S5。**

- 完成 add/import/test/default/remove 和 Recovery Point verify/delete 操作。
- 破坏性删除使用独立确认对话框，展示 Service 生成的删除计划，不从 UI 猜测影响范围。
- Repository 卡片和 Recovery Point 抽屉保持阶段 13C 视觉基线。

## 9. P3 Mount、计划与设置工作包

### S7 / D6：Mount

**状态：S7/D6 实现中（MVP 接线完成，待人工挂载验收）。**

- 隔离 `aegra_mount_host` + Service `MountSupervisor`；会话权威状态在 Service 内存表。
- Desktop 不直接调用 Dokan；经 kinds 8/41/42 与 `mount.list|start|unmount` 能力。
- Mount 页面：checkpoint → layout 源盘勾选 → mount/unmount；会话表展示挂载点、状态、开始时间。
- 待验收：Dokan 可用环境整盘只读、强制 unmount、Service 停止清理 overlay、加密 Archive 口令路径。

### S8 / D7：Schedule 与 Event Log

**状态：S8 进行中（Schedule trigger engine 已落地；Event/Audit 查询与 D7 仍待）；D7 等待前置 S8 Event 部分。**

- **已完成 — Schedule engine**：`ScheduleEngine` 15s 轮询 enabled 且 `next_run_utc_ms <= now` 的计划，经
  `WorkerJobService::start_backup` 启动 Incremental（缺父 demote Full）；幂等键
  `schedule-fire|<schedule_id>|<due_next_run_utc_ms>`；Accepted/Replayed 后 CAS 推进 next_run；
  missed run = 该 due 槽只跑一次并跳到下一未来候选；Conflict/容量满不推进、下轮重试。见
  [service_host.md](../modules/service_host.md#schedule-触发引擎s8)。
- **未完成 — timezone 精细化**：当前与 Upsert 一致使用 UTC 日网格解释 `local_minutes_of_day`；
  完整 IANA timezone 解释仍可后续加强。
- Event/Audit 使用稳定类型、severity、message code、参数和 correlation ID，支持过滤与 cursor 分页。
- Event Log 页面不展示原始异常、SecretRef 或敏感路径；详情和导出遵循脱敏策略。

### D8：Settings

**状态：可开始。**

- 迁移语言、主题、Service 状态、默认 Repository 和诊断入口。
- 语言和纯 UI 主题保存在 Desktop `QSettings`；影响 Service 行为的设置必须通过 Service API。
- 旧项目 Settings 中没有新版能力支撑的选项不迁移或保持禁用。

## 10. 页面与 Service 能力映射

| Desktop 页面 | 只读 Query                                       | Command / Event                       | 缺失时行为                        |
| ---------- | ---------------------------------------------- | ------------------------------------- | ---------------------------- |
| Shell/Home | service info、capabilities、recent jobs          | task event subscription               | 显示 Service unavailable/retry |
| Backup     | source inventory、repositories、eligible parents | start/cancel backup、task events       | 禁止 Start                     |
| Restore    | recovery points、chain/preflight、targets        | start/cancel restore、task events      | 禁止 Start                     |
| Mount      | mountable points、mounted sessions              | mount/unmount、session events          | 隐藏或禁用页面                      |
| Repository | connections、recovery points、delete plan        | add/import/test/default/verify/delete | 每项按 capability 禁用            |
| Event Log  | paged events、filters                           | export request（后续）                    | 显示空状态，不伪造事件                  |
| Settings   | service config、repository defaults             | update service settings               | UI-only 设置仍可用                |

## 11. Agent 分工与文件所有权

### 11.1 领取规则

- 一个 agent 一次只领取一个工作包，并在开始前记录 owner、基线 commit、允许修改目录和前置版本。
- `S0` owner 独占 Contracts/Service protocol/ADR；其他 agent 不并发修改这些文件。
- `D0` owner 管理翻译 ID 目录和 locale 基础；页面 agent 提交所需 ID 清单或独立翻译 patch。
- `Main.qml`、`resources.qrc`、`qmldir`、Desktop 顶层 CMake 和导航注册由 Desktop integration owner 管理。
- Service composition root、schema migration registry 和顶层 CMake 由 Service integration owner 管理。
- 页面 agent 只修改自己的 `pages/<Area>`、`components/<Area>` 和专属测试；不得顺手改公共 shell。
- 共享文件不可避免时，先让 owner 提供扩展点；不得通过复制公共组件规避冲突。

### 11.2 页面迁移规则

- 先用旧 UI 截图/运行结果确认视觉基线，再按新组件边界重写；显示效果必须对齐旧 UI，不逐文件复制巨大 QML。
- 每个页面先完成静态视觉、responsive layout、键盘/无障碍和 i18n，再连接 Service model。
- 单个 QML/CPP 文件不接近 1500 行上限；复杂表单、抽屉、表格和对话框拆成有领域名称的组件。
- 不引入假的成功数据来补 Service 缺口；使用 capability、loading、empty、error 和 disabled 状态。
- 页面完成时附旧/新并排的 900x600、1080x720、150% DPI、五种 locale 截图或自动化证据。

### 11.3 推荐并行波次

```text
Wave 0: D0 + D1 + S0
Wave 1: S1 + S2（已完成）
Wave 2: S3 + S4（已完成）
Wave 3: D2 + D3；S5 可与 Desktop 基础工作并行
Wave 4: S5 + S6；随后 D4 + D5
Wave 5: S7 + S8；随后 D6 + D7 + D8
Wave 6: R0 发布门禁与全链路回归
```

同一 wave 表示允许并行，不表示可以忽略表中前置依赖。每个 Service API 先合并 contract 和 fake-backed
contract test，Desktop agent 才开始真实接入。

### 11.4 下一轮与后续 Agent 安排

**Agent A：S5 Archive 深度操作**

**任务目标：** 完整交付第 8 节 S5，不是提交一个局部 helper。最终必须让 Service 可以按
`repository_connection_id + recovery_point_id` 解析链、启动真实 Verify Worker、生成删除影响计划并安全执行删除。

**开始前必须完成：**

1. 完整阅读 `CPP_ENGINEERING_STANDARD.md`、`MODULAR_ARCHITECTURE.md`、`personal_repository.md`、
   `personal_archive_verify.md`、`application.md`、`service_host.md`、`ports.md`、`adapters.md`、
   `control_plane_sqlite.md`、ADR-0010、ADR-0013 和个人版 Repository V1 格式文档。
2. 先列出现有可复用能力和缺口：`RecoveryPointGraph`、Catalog Scanner、Verify Worker Task、Supervisor、
   SQLite stores、Storage Port。禁止复制已有实现或在 Service Host 中重新实现 Pipeline。
3. 在编码前给 integration owner 提交 contract/port 变更清单。新增 durable delete-plan 结构或协议语义时必须同步
   ADR/格式文档；产品未发布，直接修正 schema，不加 legacy fallback。

**必须交付的代码：**

- S5 Contracts、validator、JSON codec 和 contract tests；
- Application chain/eligibility/verify/delete-plan use case 与纯 fake-port 测试；
- `personal_repository` 删除计划与执行核心，以及 Memory/Local Storage contract/integration tests；
- Verify 的 `WorkerJobService` 接线、真实 Supervisor/Worker 进程测试和 SQLite Job/Command/Audit 持久化；
- Service Host dispatch、capability、composition root、CMake、模块文档和计划状态更新。

**文件所有权：** Agent A 独占新增 S5 Application、personal_repository、Service handler 和 Storage Port 扩展。
协议公共文件、`service_main.cpp`、Service 顶层 CMake 与 SQLite schema registry 属 integration
owner；Agent A 可以准备 patch，但合并前必须统一接线，不能复制一套旁路 composition。不得修改 Desktop QML。

**实施顺序是强制的：** contract/port -> 核心 chain/delete-plan -> Application -> fake integration -> Service/Worker
真实接线 -> Local Storage 故障恢复 -> Debug/Release 全量验证 -> 文档。禁止先把 capability 宣告为可用，再补实现。

**交付报告必须包含：** 修改文件清单、每个 durable/IPC 语义、全部测试命令与结果、真实进程测试证明、失败恢复
说明、仍未完成项。第 8 节 S5 任一“必须”条目缺失时，状态必须保持进行中，不得标记已完成。

**Agent B：D2 Home 与全局任务体验**

**任务目标：** 完整交付第 7 节 D2，形成可日常使用的 Home、首次连接恢复流程和全局 Job 体验；不是只画一个页面。

**开始前必须完成：**

1. 完整阅读 `CPP_ENGINEERING_STANDARD.md`、`MODULAR_ARCHITECTURE.md`、`desktop.md`、`service_host.md`、
   ADR-0011、ADR-0013，以及现有 `ServiceClient`、request coordinator、Repository model 和五语言实现。
2. 运行旧 `D:\Work\OpenSource\backup\src\gui` 和当前 Desktop，记录 Home、Splash、Toast、Loading、Repository
   页和窗口 shell 的旧/新截图。旧 UI 的显示效果、视觉密度和主要交互必须复刻；不得复制旧 Backend、
   HTTP/WebSocket、巨大 QML 或硬编码数据。
3. 明确当前后端边界：可用的是 `service.info`、capabilities、`job.list` 和 Repository query；task event subscription
   尚不可用。设计必须基于此事实，并保留将来切换事件订阅的扩展点。

**必须交付的代码：**

- Desktop 私有 `job.list` codec、分页 coordinator 接线、`JobModel` 与确定性 C++ 测试；
- 与旧 UI 显示一致的 Home page、Splash/error/retry、全局 task surface、toast/loading 组件和实际导航；
- ServiceClient 对 Job 刷新/有界轮询/重连/terminal 去重的实现，不破坏 Repository query；
- 五语言翻译、message-code 映射、资源/qmldir/CMake 接线和 Desktop 模块文档；
- 旧/新并排的 900x600、1080x720、150% DPI、五语言截图或自动化视觉证明。

**文件所有权：** Agent B 独占新增 Home/Task QML、Desktop Job model/codec 和专属测试。`Main.qml`、共享导航、
`resources.qrc`、`qmldir`、翻译目录与 Desktop 顶层 CMake 属 Desktop integration owner；需要修改时必须集中接线，
不得复制 Sidebar、ServiceClient 或翻译目录规避冲突。不得修改 Service/Application/SQLite 生产代码。

**实施顺序是强制的：** 旧 UI 基线截图 -> codec/model tests -> ServiceClient 多请求调度 ->
Home/Splash/task components 视觉返工 -> navigation/i18n -> 断线与错误恢复 -> 多 viewport/locale 旧/新并排验证 ->
Debug/Release 全量验证 -> 文档。禁止先提交静态页面后宣称后端待接。

**交付报告必须包含：** 修改文件清单、UI 状态矩阵、请求/轮询策略、生产构建与人工验证结果、已禁用能力清单。
viewport/locale 视觉证据作为视觉质量工作维护，不参与 D2 生产功能状态判定。

**Agent C：D3 Backup 页面**

**任务目标：** 完整交付第 7 节 D3，让用户可以从真实 Service Inventory 与 Repository connection 中选择
source/target，启动全量 Backup，观察进度并取消任务；增量、差异和 schedule 必须严格受 capability gate 约束。

**开始前必须完成：**

1. 完整阅读 `CPP_ENGINEERING_STANDARD.md`、`MODULAR_ARCHITECTURE.md`、`desktop.md`、`contracts.md`、
   `service_host.md`、`worker_host.md`、`windows_personal_backup.md`、ADR-0013，以及现有 `ServiceClient`、
   `JobModel`、Repository/RecoveryPoint model、Inventory/Backup V3 codec 和 D2 全局任务组件。
2. 运行旧 `D:\Work\OpenSource\backup\src\gui` 和当前 Desktop，记录 Backup、Home、Repository、全局 task/toast
   和窗口 shell 的旧/新截图。旧 Backup 页面是显示一致性基线，不是可选信息架构参考；不得复制旧 Backend、
   路径输入、HTTP/WebSocket 或超大 QML。
3. 明确当前 Service 能力：可用 source inventory、repository connection、job list、start backup、cancel job；
   eligible parent、schedule 或 SecretRef 注册若未由 Service 明确声明 capability，UI 必须禁用或展示 unavailable。

**必须交付的代码：**

- Desktop 私有 Inventory/Repository/eligible parent/Backup command codec 与 model 接线，以及确定性 C++ 测试；
- 与旧 BackupPage 显示一致的 Backup page 领域 QML 组件：schedule list、Add、90% slide-in wizard、source selector、
  repository selector、backup options、schedule panel、confirmation、active progress、completion/error summary；
- StartBackup/CancelJob 的 ServiceClient 门面、幂等 key、job_id 观察、断线恢复和 D2 全局 Job/Toast 复用；
- Credential/SecretRef 受控交互入口，明文 Secret 不进入 QML model、command body、日志、toast 或 golden data；
- 五语言翻译、message-code 映射、资源/qmldir/CMake 接线、Desktop 模块文档和 D3 视觉证据。

**文件所有权：** Agent C 独占新增 Backup 领域 QML、Desktop Backup/Inventory/parent model/codec 和专属测试。
`Main.qml`、Sidebar、共享 D2 task/toast 组件、`resources.qrc`、`qmldir`、翻译目录、Desktop 顶层 CMake 和 Service
command/capability 接线属 integration owner；需要修改时集中接线，不得复制公共组件或绕过 Service。不得修改
Pipeline、Worker 备份算法、Windows Disk/VSS Adapter、Repository 格式或 SQLite schema，除非 D3 发现已合并
Service contract 存在阻塞性 bug，并由 owner 同意。

**实施顺序是强制的：** 旧 Backup UI 基线截图 -> codec/model tests -> ServiceClient Start/Cancel/observe ->
Backup list/wizard 静态视觉返工 -> i18n/accessibility -> credential gate -> full backup command path ->
cancel/error/reconnect -> 多 viewport/locale 旧/新并排视觉验证 -> Debug/Release 全量验证 -> 文档。禁止先把 Start
按钮做成可点击，再把旧 UI parity、后端、幂等、取消和安全作为后续项。

**交付报告必须包含：** 修改文件清单、UI 状态矩阵、capability gate 清单、Start/Cancel 幂等与重连策略、认证信息
不泄漏检查、生产构建与人工验证结果、真实 Service/SQLite Job 证据。viewport/locale 视觉证据作为视觉质量工作
维护，不参与 D3 生产功能状态判定。

**集成交接**：Service integration owner 负责 S5 capability/composition 接线，Desktop integration owner 负责
导航、共享翻译目录和顶层 CMake。两项可并行，但不得并发修改对方领域文件。S5 的 Service contract 如影响 Desktop，
Agent B 本轮不得提前接入半成品；S5 合并后再由后续 D3/D5 消费真实 Verify/删除能力。

### 11.5 Agent 共同强制门禁

- “能编译”不等于完成。必须存在 production composition、真实请求路径和与风险匹配的失败测试。
- 禁止用 hardcoded/fake/demo 数据补齐产品能力；Fake 只用于单元测试，至少一个测试必须经过真实 Adapter/进程边界。
- 禁止把未实现的 capability 暴露为 available，禁止留可点击但无效的 UI 命令，禁止吞掉错误后返回成功。
- 禁止把 Desktop 页面做成不同于旧 `backup/src/gui` 的新 UI 风格；没有旧/新并排视觉证据的 Desktop 页面不得完成。
- 禁止新增旧协议、旧格式、旧 UI Backend 兼容层；确需 durable/IPC 决策必须同步 ADR。
- 所有 command 必须有幂等、原子性和重启语义；所有长操作必须覆盖取消/deadline；日志和响应不得泄漏 SecretRef、
  明文凭据、Archive path、对象 key、设备路径或原始异常。
- 所有生产函数、lambda、文件和依赖方向必须通过工程限制；不得用压缩格式或巨型 `service_host.cpp`/`Main.qml`
  规避拆分。
- Windows 必须使用 VS 2026 Insiders。每个 agent 至少运行直接 affected Debug/Release target 和专属测试；integration
  owner 在两项合并后必须运行 Debug/Release 全量 `ctest`、`architecture.source_limits`、格式和 `git diff --check`。
- Agent 只有在所有强制条目有代码、测试和文档证据时才能更新状态为已完成。任何“后续再接 composition/test/i18n/
  Release”的交付都只能标记进行中，并在交接中列出明确剩余项。

**最低验收命令（必须在 VS 2026 Insiders Developer Command Prompt 环境运行）：**

```powershell
cmake --build --preset vs2026-debug --config Debug --parallel
ctest --test-dir out/build/vs2026-debug -C Debug --output-on-failure
cmake --preset vs2026-release
cmake --build --preset vs2026-release --config Release --parallel
ctest --test-dir out/build/vs2026-release -C Release --output-on-failure
ctest --test-dir out/build/vs2026-debug -C Debug -R "architecture\.source_limits" --output-on-failure
git diff --check
```

Agent 可以在开发过程中先运行直接 target，但最终交付报告必须包含上述全量结果；若环境原因无法执行某条命令，
不得写“完成”，必须标记进行中并记录失败命令、完整错误和需要 integration owner 处理的具体条件。格式检查必须对
所有本次修改/新增 C++ 文件使用仓库 `.clang-format`，不能只格式化已跟踪文件。

## 12. 测试与发布门禁

每个工作包至少运行直接 affected target 的 Debug/Release 构建和测试，并执行源码规模、依赖边界、格式、
秘密扫描和协议损坏输入测试。Windows 构建使用项目规定的 Visual Studio 2026 Insiders 与 Qt 6.8.3。

个人版发布前必须完成：

- Desktop 五语言、DPI、键盘导航、Service 重启重连和长时间 task event soak。
- Desktop -> Service -> Worker -> Repository 的真实 Backup、Verify、Restore E2E。
- 非管理员 Desktop + 正式 Service 的 SCM/ACL/客户端身份测试。
- Service/Worker crash、机器重启、取消、deadline、Repository 离线和 SQLite 损坏恢复测试。
- installer 的安装、升级、卸载、Service recovery policy、日志轮转和崩溃诊断。
- 确认数据库、日志、IPC 和 UI model 中均无明文秘密或客户数据泄漏。

## 13. 工作包 Definition of Done

每个 agent 交付时必须同时满足：

1. 前置 contract 已合并，依赖方向符合模块架构；
2. 生产实现为 C++20，Qt 只位于 Desktop；
3. 函数、嵌套和源码文件规模符合工程规范；
4. 正常、失败、取消、超时、损坏输入和重启路径有与风险相称的测试；
5. UI 可见文本全部可翻译，Service 只返回稳定 code；
6. 直接 target 与架构/静态门禁通过；
7. 模块文档、ADR、协议或状态说明随实现同步更新；
8. 交接说明列出修改文件、测试命令、未完成项和下一工作包的明确前置条件。

## 14. 明确不进入本计划

- 企业版 PostgreSQL 控制面、Gateway、全局去重 CAS、Chunk Index 服务和多节点维护任务。
- 虚拟机备份/恢复的 hypervisor connector 与企业调度策略。
- 旧项目网络 Backend、旧配置迁移、旧协议兼容和未发布 `.bkf` 兼容读取。
- 把 Recovery Point、Chunk Index 或 Archive metadata 复制到 SQLite 作为权威事实。

这些能力应在个人版闭环和 Service 安全边界稳定后，以独立 ADR、模块计划和 contract 版本推进。
