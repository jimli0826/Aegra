# Desktop 迁移与个人版 Service 完成计划

## 1. 文档目的

本文把旧项目 `backup/src/gui` 中仍有价值的 Desktop 体验迁移到 Aegra，并收敛个人版 Service 剩余控制面工作。
它是后续 agent 领取任务、确定文件所有权和验收合并的执行依据。

当前基线为阶段 13C + D0/D1/S0：Desktop 已能通过 schema 3 Named Pipe 与 Service 握手、自动重连并分页查询个人版
Recovery Point；Repository 页面、无边框窗口、折叠侧栏和 `blueExtra` 主题已迁移。Worker 已能执行 Backup、
Verify 和非系统卷 Restore，但 Service 尚未监督 Worker，也没有任务、计划、事件和 Repository 连接状态的
持久化控制面。

工作包状态最后更新于 2026-08-03。状态定义如下：

- `已完成`：满足本工作包 Definition of Done，并已同步实现、测试和文档；
- `进行中`：已有 owner 领取并产生实现工作，尚未完成全部验收；
- `可开始`：全部前置工作包已完成，可以立即领取；
- `等待前置`：至少一个前置工作包尚未完成；
- `阻塞`：前置已满足，但存在需要明确解决的技术或外部条件阻塞。

第 5 节总览表是工作包状态的唯一权威来源。Agent 开始或完成工作时必须同步更新该表；不能仅根据代码文件
存在就把工作包标记为已完成。

本计划不要求兼容未发布旧项目的 UI Backend、通信协议、配置或数据。旧 UI 只作为视觉和工作流参考。

## 2. 目标与边界

### 2.1 目标

- Desktop 保持旧项目的页面结构、主要操作路径和视觉密度，同时完全改用新版 Service IPC。
- Desktop 首批支持 `en_US`、`zh_CN`、`zh_TW`、`ja_JP` 和 `de_DE`，并可在运行时切换语言。
- Service 成为个人版唯一控制面，负责 Repository 连接、任务、计划、事件和 Worker 生命周期。
- 形成 Backup、Verify、Restore 和 Mount 的 `Desktop -> Service -> Worker/Mount Host -> Repository` 闭环。
- 每个工作包有独立文件边界、前置条件、测试和 Definition of Done，可由不同 agent 顺序或并行完成。

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

| 旧界面或组件 | 处理方式 | 归属工作包 |
| --- | --- | --- |
| `Main.qml`、`Theme.qml`、窗口/侧栏 | 阶段 13C 已迁移基线；只由 integration owner 扩展导航和全局状态 | D0, D2, D8 |
| `RepositoryPage.qml` | 已迁移只读页面；补连接、Verify、Delete 等真实操作 | D5 |
| `HomePage.qml` | 按摘要、近期任务和快捷操作拆分重写 | D2 |
| `BackupPage.qml` | 按 source、target、options、schedule、confirm、progress 拆分重写 | D3 |
| `RestorePage.qml` | 按 recovery point、chain、target、confirm、progress 拆分重写 | D4 |
| `MountPage.qml` | 按 mount request 和 mounted session 列表拆分重写 | D6 |
| `EventLogPage.qml` | 按 filter、paged table 和 detail drawer 拆分重写 | D7 |
| `SettingsPage.qml` | 按 appearance、locale、service 和 repository settings 拆分重写 | D8 |
| `SplashScreen`、`LoadingOverlay`、`ToastBanner` | 建立无业务依赖的共享反馈组件 | D2 |
| `Card`、`ComboBoxIndicator`、`DatePickerField`、`LinkButton` | 仅在页面确实需要时迁移视觉行为，并完成 i18n/键盘/无障碍 | 页面 owner |
| `DiskChart`、`DiskIcon`、`DriveItem` | 改为只消费 Service Inventory model 的展示组件 | D3, D4 |
| `BackupPasswordDialog` | 不传递密码；改为创建或选择 `SecretRef` 的受控交互 | D3 |
| `AddLocationPanel`、`AddRepositoryPanel`、`LocationManagementDialog` | 合并为新版 Repository connection 工作流 | D5 |

旧 `LogsPage`、`ManagePage`、`SyncPage` 和 `ToolsPage` 不属于当前个人版导航，不自动迁移。只有产品范围和对应
Service capability 经过单独设计后才能加入计划。旧 C++ Backend、`TaskWebSocket`、`I18n.qml`、qmake 文件、
Makefile、`release/` 生成物和 `.bak` 文件全部禁止迁移。

## 5. 工作包总览

| ID | 状态 | 优先级 | 工作包 | 主要产物 | 前置 |
| --- | --- | --- | --- | --- | --- |
| D0 | 已完成 | P0 | Desktop 国际化基础 | Linguist、LocaleController、语言设置、门禁测试 | 无 |
| D1 | 已完成 | P0 | Desktop 客户端分层 | transport、request coordinator、models、协议适配边界 | 无 |
| S0 | 已完成 | P0 | Service 协议 V3 | commands/queries/events DTO、codec、ADR | 无 |
| S1 | 进行中 | P1 | 正式 Windows Service 边界 | SCM、Pipe ACL、调用方身份、安装生命周期 | S0 |
| S2 | 进行中 | P1 | SQLite 控制面 | schema、repository connection、job/schedule/event stores | S0 |
| S3 | 等待前置 | P1 | Worker Supervisor | 启动、进度、取消、崩溃回收、结果持久化 | S0, S2 |
| S4 | 等待前置 | P1 | Inventory 与 Repository API | source inventory、connection/test、recovery point queries | S0, S2 |
| D2 | 可开始 | P1 | Home 与全局任务体验 | Home、Splash、toast、task model | D0, D1, S0 |
| D3 | 等待前置 | P1 | Backup 页面 | source/target/options、启动/取消/进度 | D0, D1, S3, S4 |
| S5 | 等待前置 | P2 | Archive 深度操作 | authenticate、verify、显式链解析、删除计划 | S2, S3, S4 |
| S6 | 等待前置 | P2 | Restore 编排 | preflight、目标检查、任务监督、结果查询 | S3, S5 |
| D4 | 等待前置 | P2 | Restore 页面 | recovery point、链、目标、确认、进度 | D0, D1, S6 |
| D5 | 等待前置 | P2 | Repository 管理 | add/import/test/default/delete/verify actions | D0, D1, S4, S5 |
| S7 | 等待前置 | P3 | Mount Host 编排 | mounted session、unmount、崩溃清理 | S3, S5 |
| D6 | 等待前置 | P3 | Mount 页面 | mount/unmount/session table | D0, D1, S7 |
| S8 | 等待前置 | P3 | Schedule 与 Event/Audit | trigger engine、history、paged queries | S2, S3 |
| D7 | 等待前置 | P3 | Event Log 页面 | filter、分页、详情、导出入口 | D0, D1, S8 |
| D8 | 等待前置 | P3 | Settings 页面 | language、theme、service/repository settings | D0, D1, S1, S4 |
| R0 | 等待前置 | Gate | 发布工程 | installer、upgrade/uninstall、recovery、diagnostics、E2E | 全部发布范围 |

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

**状态：库级完成（Composition Root 接线待 integration owner）。** 决策见
[ADR-0014](../adr/0014-windows-service-ipc-security.md)。

- 提供 SCM 安装、启动、停止、恢复策略和受控卸载入口（`IWindowsServiceControlManager` + Fake/Win32）。
- 使用显式本地 Pipe ACL，拒绝远程 client，校验调用方 session/SID 和授权策略。
- Service 运行身份可访问所需系统能力，但不把高权限句柄交给 Desktop。
- 停止流程取消 accept/会话并在 deadline 内收口；计划触发与受监督任务取消属 S3/S8。
- 测试覆盖授权成功、未授权、多 session、stop、restart 与 installer rollback。

### S2：个人版 SQLite 控制面

**状态：Adapter/ports 已完成（独立可测）；Service composition 接入待 integration owner。**

- 已增加 `ports/control_plane.h` 与 `Aegra::AdapterSqlite`，保存 Repository connection、SecretRef、Job、
  Schedule、Event/Audit 和 schema version。
- 使用迁移事务、外键、唯一约束和显式 UTC 时间；Service 单写者，读操作有一致快照语义。
- Job 状态机至少包含 queued/running/cancelling/succeeded/failed/cancelled/interrupted。
- Service 启动时把遗留 running/cancelling 任务收敛为 interrupted（`mark_active_as_interrupted`），
  并由上层策略决定是否允许重新提交。
- 数据库删除后可由 Repository 与配置重新建立索引；不得把 DB 当作 `.bkf` 或 Recovery Point 的权威来源。
- 不保存密码、Chunk Index、Manifest、完整 Archive metadata 或用户块数据。
- 详见 [control_plane_sqlite.md](../modules/control_plane_sqlite.md)。

### S3：Worker Supervisor

**状态：等待前置 S2 集成到 Service composition root。**

- 从受信任配置构造 Worker 命令行和随机 Pipe 名，启动单任务低权限/适当权限 Worker。
- 完成 Job/Cancel、Progress/Result 会话，持久化状态转换和最终稳定结果。
- 支持 Desktop 取消、Service 停止、deadline、Worker crash、无 Result 退出和 Pipe 断开。
- 进程退出码只用于快速分类，权威结果来自合法 `WorkerResponse`；stderr 不作为协议。
- Service 重启后不尝试附着到未知 Worker；标记 interrupted，并清理遗留 IPC/临时状态。
- 添加 fake-process 单元测试和真实 Worker 进程测试，覆盖成功、拒绝、取消、超时和崩溃。

### S4：Inventory 与 Repository Connection API

**状态：等待前置 S2。**

- Inventory query 返回稳定 source ID、卷标签、容量、系统/只读/可选状态，不暴露可伪造的 Desktop 路径输入。
- Repository connection 支持 add/import/test/set-default/list/remove；持久化根定位符和 SecretRef，不保存明文凭据。
- Recovery Point list 继续从 Repository Catalog Scanner 获取，并带连接 ID、能力和 stale/scan 状态。
- 删除连接只删除控制面引用，除非独立 destructive command 明确确认，不删除 `.bkf`。
- 测试未配置、离线、损坏 catalog、取消、分页稳定性和多个 Repository。

### D2：Home、Splash 与全局任务体验

**状态：可开始。**

- 迁移 Home 的状态摘要、近期任务、Repository 容量摘要和常用动作，不复制旧 Backend 数据源。
- Splash 只覆盖 Desktop 启动和首次 Service handshake；Service 不可用时进入可恢复错误页面，不能无限阻塞。
- 建立全局 task list/progress、toast 和 loading overlay 组件；消息全部来自翻译 ID。
- Home 不伪造缺失指标；Service capability 不存在时隐藏或禁用对应动作。

### D3：Backup 页面

**状态：等待前置 S3、S4。**

- 将旧 2480 行页面拆为 source selector、repository selector、backup options、schedule editor、confirmation 和 progress。
- source 只来自 Service Inventory model；target 只来自 Repository connection model。
- 提交 command 后按 job ID 观察进度；重复点击使用幂等 key，取消必须显示 cancelling 过渡态。
- 密码通过受控 Credential/SecretRef 流程登记，不进入 QML model、日志或 command body。
- 全量备份先完成；增量只在 Service 返回可用 parent capability 时启用；差异备份保持禁用。

## 8. P2 恢复闭环工作包

### S5：Archive 深度扫描、Verify 与删除计划

**状态：等待前置 S2、S3、S4。**

- 区分 Catalog 摘要扫描与需要凭据的 Archive 认证；列表成功不代表可恢复。
- 由 `personal_repository` 解析显式 base-first 链、缺层、循环、parent mismatch 和 credential mapping。
- Verify 作为受监督 Worker job 暴露，进度和结果进入统一任务模型。
- 删除先生成影响计划，列出依赖恢复点和将删除的对象；执行必须携带计划 token、幂等 key 和用户确认。
- 失败不得让 catalog 宣称对象已删除；tombstone、对象删除和 catalog 重建语义遵循 personal repository 文档。

### S6：Restore 编排

**状态：等待前置 S3、S5。**

- API 接受 recovery point ID 和 Service Inventory 中的 target ID，不接受 Desktop 提供任意 Archive 链或设备路径。
- Service 解析链和每层 SecretRef，完成认证/preflight 后构造现有 Restore Worker job。
- 强制非系统卷、安全容量、磁盘身份和确认前置；把 Worker 稳定结果映射到任务状态。
- Desktop 断线不取消已接受任务；重新连接后按 job ID 查询/订阅。

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

**状态：S7 等待前置 S3、S5；D6 等待前置 S7。**

- 新增隔离 Mount Host 与 Service supervisor，Service 维护 mounted session 的权威状态。
- crash、用户注销、Service 停止和强制 unmount 都必须清理挂载点；Desktop 不直接调用 Dokan。
- Mount 页面展示 recovery point、挂载点、只读状态、开始时间和 unmount 结果。

### S8 / D7：Schedule 与 Event Log

**状态：S8 等待前置 S2、S3；D7 等待前置 S8。**

- Schedule engine 使用持久化 next-run 和 timezone/UTC 规则；机器休眠、时钟跳变和 missed run 有确定策略。
- 同一 schedule 触发使用幂等 key，避免 Service 重启重复启动备份。
- Event/Audit 使用稳定类型、severity、message code、参数和 correlation ID，支持过滤与 cursor 分页。
- Event Log 页面不展示原始异常、SecretRef 或敏感路径；详情和导出遵循脱敏策略。

### D8：Settings

**状态：等待前置 S1、S4。**

- 迁移语言、主题、Service 状态、默认 Repository 和诊断入口。
- 语言和纯 UI 主题保存在 Desktop `QSettings`；影响 Service 行为的设置必须通过 Service API。
- 旧项目 Settings 中没有新版能力支撑的选项不迁移或保持禁用。

## 10. 页面与 Service 能力映射

| Desktop 页面 | 只读 Query | Command / Event | 缺失时行为 |
| --- | --- | --- | --- |
| Shell/Home | service info、capabilities、recent jobs | task event subscription | 显示 Service unavailable/retry |
| Backup | source inventory、repositories、eligible parents | start/cancel backup、task events | 禁止 Start |
| Restore | recovery points、chain/preflight、targets | start/cancel restore、task events | 禁止 Start |
| Mount | mountable points、mounted sessions | mount/unmount、session events | 隐藏或禁用页面 |
| Repository | connections、recovery points、delete plan | add/import/test/default/verify/delete | 每项按 capability 禁用 |
| Event Log | paged events、filters | export request（后续） | 显示空状态，不伪造事件 |
| Settings | service config、repository defaults | update service settings | UI-only 设置仍可用 |

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

- 先用旧 UI 截图/运行结果确认视觉基线，再按新组件边界重写；不逐文件复制巨大 QML。
- 每个页面先完成静态视觉、responsive layout、键盘/无障碍和 i18n，再连接 Service model。
- 单个 QML/CPP 文件不接近 800 行上限；复杂表单、抽屉、表格和对话框拆成有领域名称的组件。
- 不引入假的成功数据来补 Service 缺口；使用 capability、loading、empty、error 和 disabled 状态。
- 页面完成时附 900x600、1080x720、150% DPI、五种 locale 的截图或自动化证据。

### 11.3 推荐并行波次

```text
Wave 0: D0 + D1 + S0
Wave 1: S1 + S2 + S4；S0 合并后启动 D2 静态部分
Wave 2: S3；随后 D3 接入并完成备份闭环
Wave 3: S5 + S6；随后 D4 + D5
Wave 4: S7 + S8；随后 D6 + D7 + D8
Wave 5: R0 发布门禁与全链路回归
```

同一 wave 表示允许并行，不表示可以忽略表中前置依赖。每个 Service API 先合并 contract 和 fake-backed
contract test，Desktop agent 才开始真实接入。

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
