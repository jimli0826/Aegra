# Desktop GUI 开发文档

## 目标与迁移范围

`apps/desktop` 是普通用户 Qt/QML 客户端，只通过版本化 Service IPC 获取状态和发起用例。阶段 13B 在连接、
握手和重连骨架上增加个人版 Repository 与 Recovery Point 列表。

旧 `D:\Work\OpenSource\backup\src\gui` 是 Desktop 的强制显示基线。迁移品牌 PNG/ICO、默认
`blueExtra` 调色板、无边框标题栏、可折叠侧栏、页面间距、卡片、表格、右侧抽屉、toast/loading/splash
和页面主要交互路径。允许按新模块边界拆分和重写 QML，但同一页面已存在的布局、视觉密度、颜色、控件尺寸、
状态表达和动画节奏必须保持一致；旧 UI 不是“灵感参考”，而是新 Desktop 的视觉验收标准。

不迁移 qmake 生成物、构建产物、超过源码限制的 Backend、HTTP/WebSocket 通信、TLS 校验豁免或让 GUI
直接执行备份、恢复、挂载和 Repository I/O 的代码。旧 Backend 的所有数据访问改写为版本化 Service IPC；
旧 UI 需要但 Service 尚未提供的操作保持禁用，不用本地直连临时补齐。不迁移旧 `I18n.qml` JavaScript 字典。

进程模型对齐旧 GUI：`main.cpp` 使用 per-user `QLockFile` + `QLocalServer` 保证 **仅一个 Desktop
实例**。二次启动不打开第二窗口，而是向首实例发送 raise 并退出。

## 旧 UI 显示一致性

- 开发或返工任何 Desktop 页面前，必须运行或检查旧 `D:\Work\OpenSource\backup\src\gui` 对应页面，并保存旧 UI
  基线截图。提交时提供旧/新并排证据；没有并排证据不能把页面标记为完成。
- 新实现必须复刻旧 shell 的 32px 标题栏、产品图标/标题/版本位置、160px/56px 侧栏、40px 菜单项、默认
  `blueExtra` 深蓝色板、12px 页面边距、3px 标题强调条、Card 标题栏、36px 表头、44px 表行、90% 右侧抽屉、
  8px 弹窗圆角、按钮/hover/progress/toast/loading/splash 的同类视觉表现。
- 可变的是数据来源和安全边界：旧 Backend、HTTP/WebSocket、直接路径输入、明文密码和本地 I/O 必须替换为
  Service IPC、领域 model、capability gate、SecretRef 和 disabled/unavailable/empty/error 状态。不可变的是用户看到的
  页面层级、密度和主要操作路径。
- Home 保持旧 This PC + Tasks 两段工作台；Backup 保持旧 schedules list + Add + 90% slide-in wizard；Repository
  保持旧卡片列表 + Recovery Point 右侧抽屉。Service 暂缺能力时在原位置禁用或显示 unavailable，不得改成新布局。

## 依赖与 Target

```text
src/apps/desktop/
├── CMakeLists.txt
├── translations/          # .ts 源与 lrelease 生成的 .qm
├── resources/
├── qml/
└── src/
    ├── main.cpp
    ├── locale/            # LocaleController、格式化、message_code 映射
    └── client/            # transport、protocol、coordinator、models、ServiceClient 门面
```

`aegra_desktop` 只依赖 Qt Core/Gui/Qml/Quick/QuickControls2/Network/LinguistTools。Qt 通过
`AEGRA_QT_ROOT` 或标准 `CMAKE_PREFIX_PATH` 提供，不在仓库写入开发机绝对 include/lib 路径。Desktop
不链接 Engine、Pipeline、Repository、Windows Disk/VSS、数据库或 Worker 实现。

## 国际化（D0）

- QML 可见文本使用稳定 `qsTrId("aegra.<area>.<meaning>")`；Desktop C++ 使用 `qtTrId` 与 `//%` 源文。
- 首批语言：`en_US`（源语言）、`zh_CN`、`zh_TW`、`ja_JP`、`de_DE`。
- `LocaleController` 由 composition root 构造并注入 QML；负责发现语言、加载 `QTranslator`、
  `QQmlEngine::retranslate()`，并把选择写入 Desktop 本地 `QSettings`（`ui/language`）。
- 首次启动跟随系统 locale；无效保存值回退 `en_US`。缺失或损坏的 `.qm` 不得阻止启动。
- Service 只返回稳定 `message_code`；Desktop 通过 `message_code_map` 转为翻译 ID，未知 code 使用通用安全文本。
- 日期、容量等通过 `LocaleFormat`/`QLocale` 格式化，不在 QML 拼接固定英文单位。
- Settings 页面（D8）落地前，标题栏提供最小语言切换入口。
- 构建时用 `lrelease` 从 `translations/*.ts` 生成 `.qm`，并由 `resources.qrc` 嵌入 `:/Aegra/i18n/`。

## 客户端分层（D1）

```text
ServiceClient (QML 门面)
  ├── IpcFrameTransport          # 长度前缀 framing、连接/重连
  ├── service_protocol           # Service V3 私有 Qt 编解码
  ├── ServiceRequestCoordinator  # correlation ID、deadline、断线清理、分页 continue
  └── RecoveryPointModel         # 领域 QAbstractListModel
```

- 每个请求有唯一 correlation ID 与 deadline；协议损坏、超时断开并重连。
- 重连后只重新握手并恢复幂等 query（当前为 Repository catalog 分页查询）。
- 模型只暴露拥有生命周期的数据与展示用角色；QML 不解析 JSON、Service 枚举数值或 message code。
- `post_to_object` 提供单线程 Qt 投递边界，供后续 task event 在对象销毁后安全丢弃更新。
- V3 字段以 Contracts 与 ADR-0013 为准；Desktop 私有 codec 不独立扩展 wire schema。

## ServiceClient 行为

- 使用 `QLocalSocket` 连接 `aegra-service-control`。
- 发送和接收与 ADR-0011 相同的 4 字节长度帧，最大 64 KiB。
- 连接后生成新的 request ID 并发送 schema 3 `GetServiceInfo`，协商 API V3 后分页发送
  `ListRecoveryPoints`。
- 只接受 schema、kind、request ID、字段类型和范围全部合法的响应。
- 每页最多 100 项，跨页 `file_uuid` 必须严格递增，token 必须前进，最多累计 10,000 项；全部页面完成后
  才原子发布给 `RecoveryPointModel`。
- Repository 查询失败只更新 Repository 错误状态，不把已经 Ready 的 Service 误报为断开；协议损坏仍断开
  并重连。
- `refreshRepository()` 只在 Service Ready 且没有 pending request 时启动新分页查询；重复点击不会并发发送
  第二组请求。
- 断线或错误进入 Disconnected 状态，并以有界固定间隔重连；同一时刻最多一个连接尝试。
- QML 只观察 `connected`、`statusText`、`serviceVersion`、`apiVersion`、`errorText`、
  `repository*` 与 `recoveryPoints` model 等拥有数据的属性。
- 日志不输出 frame body、路径、凭据或 Service 原始错误文本。

## 窗口与导航

- 默认窗口为 1080x720，最小 900x600，使用 32px 自绘标题栏和产品图标。
- 标题栏提供拖动、双击最大化、最小化、最大化/还原和关闭；窗口按钮尺寸保持 36x32。
- 左侧导航展开宽度 160px，折叠宽度 56px，菜单项高度 40px；顺序保持 Home、Backup、Restore、Mount、
  Repository、Event Log，底部保留 Settings、Feedback 和折叠开关。
- 未接入页面和命令可以显示但必须禁用；当前 Repository 页面保持选中。
- 默认采用旧版 `blueExtra` 深蓝调色板，不自行切换为浅色工作区。后续 Theme 设置接入 Service 前不持久化
  用户主题选择。

## Repository 页面

页面沿用旧版 Repository 交互层级：Repository connection 卡片列表为主视图，点击恢复点数量打开覆盖主视图
90% 宽度的右侧 Recovery Point 抽屉。列表直接绑定 Service V3 `repository.connection` 分页结果，可展示多个
connection 的显示名、稳定 ID、状态、默认标记与 capabilities；不得在断开、加载或空列表状态回退到演示数据，
也不得伪造根路径、Repository UUID、锁或密码状态。

Recovery Point 列表展示备份类型、创建时间、逻辑/存储大小和链完整性（展示文本由 model/翻译提供）。
Catalog-only 数据不表示 Archive 结构或认证完成，因此 Restore、Verify 和 Delete 图标保持禁用。

Refresh 通过当前 Service session 重新查询 connection 与选中 connection 的 Recovery Point Catalog；查询或命令
错误仍留在页面内，不破坏 Service Ready 状态。Add、Import、Set Default、Test 与 Remove 调用 Service V3
命令，成功后自动刷新列表；Remove 只删除控制面连接引用，不删除备份数据。Lock、Unlock、Rebuild、Export 与
Password 在 Service 没有对应能力时不显示。布局必须在 900x600、1080x720 和更大窗口下不重叠。

## 验证与完成标准

- 人工协议验证覆盖分页、opaque token、跨页乱序/重复拒绝、NotConfigured、Repository RequestFailed、
  手动刷新、断线清空和重连重查。
- 人工本地化验证覆盖 message-code 映射、格式化、无效语言、保存回退和五种语言包加载。
- Service 未运行、启动后连接、Service 退出和重启均能正确更新状态。
- QML 启动不引用旧 Backend，不绕过 Service 直接操作系统资源。
- 品牌资源来源和迁移范围可追溯，旧生成物不进入新项目。
- VS 2026 Insiders + Qt 6.8 构建通过，Desktop 能真实连接 `aegra_service.exe`。

## 后续迁移

剩余 Home、Backup、Restore、Mount、Event Log 和 Settings 页面、共享组件、Service 前置能力以及 agent
文件所有权见 [Desktop 迁移与个人版 Service 完成计划](../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md)。
旧页面必须按表单、表格、抽屉和领域组件拆分，不得原样迁移超大 QML 或旧 Backend。

## 当前状态

- 阶段 13C：Repository 纵向切片视觉迁移完成。
- D0：Linguist 五语言、`LocaleController`、message-code 映射、`LocaleFormat`、最小语言切换入口。
- D1：transport / protocol / coordinator / `RecoveryPointModel` 分层；Repository 行为已接入 Service V3。
- S0 integration：Desktop 私有 codec 使用 V3 Request/Response envelope，仍只消费已声明 capability。
- D2（已完成，按生产功能范围）：Home 页面、Splash/Retry、Toast、Loading overlay、`JobModel` + `job.list` 分页与有界轮询；
  `ServiceRequestCoordinator` 支持并发 Repository/Job 请求；Home↔Repository 导航可用；
  Backup/Restore/Mount 在 D3/D4/D6 接线前强制禁用（不得仅凭 capability 呈现无操作按钮）；
  进度协议严格校验与溢出安全百分比；首次 Job 快照 toast 基线与可重启 Toast 定时器；
  后台 Job 轮询不触发全屏 Loading overlay，Service Job 状态码映射到五语言稳定文案；
  Home/Splash/Toast/Loading/Shell 已接入真实生产状态和导航。
- D3（已完成，按生产功能范围）：Backup 页面与 Inventory/Connection model/codec、`StartBackup`/`CancelJob` 门面；
  Source 仅绑定 Service Inventory 稳定 ID；Target 仅绑定 Repository connection；全量备份真实启动；
  增量/差异禁用；Backup Options 支持无密码（不加密 Archive）与加密（向导创建 Schedule 时密码
  1–32 字符交给 UpsertSchedule；`StartBackup` 仅传 `schedule_id` + `backup_type`）；页面进度复用 D2 Job 观察；
  五语言翻译与 Home↔Backup↔Repository 导航已接线。
  Schedule 向导允许选择多个 Volume；Desktop 把有序、去重的稳定 `source_ids[]` 保存为一条 Schedule。
  Run 一次提交完整 Source 列表，只生成一个 Job 和一个包含全部 Volume 的 Archive；不得回退到首个可选
  Source，也不得为每个 Volume 拆分 Schedule、Job 或 Archive。单卷可以独立选择；选中 Disk 时必须
  选中并提交该 Disk 下所有具备稳定 identity 和可靠非零容量的 Volume。系统卷、只读卷、EFI/FAT、RAW
  和未知文件系统卷都可勾选；读取方式由 Worker 决定，Desktop 不按 VSS 能力过滤。
  **Schedule 创建后变更规则**（与 Service 不变量一致）：
  - 创建时若开启加密并设置密码：之后不可关闭加密、不可改/清空密码；更新命令不得再带 `archive_password`。
  - 备份源 `source_ids[]` 创建后不可变。
  - Repository connection 可改为其它已连接目标。
  - Schedule settings（频率、时间、星期等）可修改；`enabled` 可切换。
  - Backup options 中除 “完成后关机”（shutdown）可改外，其余（含 exclude pagefile、加密、去重/分卷/压缩
    等向导选项）创建后不可改；Desktop 更新路径必须回传已有冻结字段，不得静默改写加密标志。
  Backup list、Add、slide-in wizard、真实 Service/SQLite Job、Schedule、多 Volume 单 Archive、取消和聚合进度
  均已具备生产功能。
- Restore Source Disks：选中 checkpoint 后调用 Service V3 `GetRecoveryPointLayout`（kind 12）。payload 为
  hierarchical `disks[]` + `volumes[]`（与旧 `GET /api/v1/backups/layout` 一致）。Desktop 按物理
  `disk_number` 渲染一行 Source Disk，分区条由 `partitions[]` 顺序 + `volumes[].extents[]` 绑定盘符/卷标，
  并过滤 MSR/EFI/Recovery（对齐旧 `RestoreBackend::volumesForSourceDisk`）。无 `disks[]` 的旧 Archive
  返回 layout 失败（需重新备份），不合成假 Disk 0。
