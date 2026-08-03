# Desktop GUI 开发文档

## 目标与迁移范围

`apps/desktop` 是普通用户 Qt/QML 客户端，只通过版本化 Service IPC 获取状态和发起用例。阶段 13B 在连接、
握手和重连骨架上增加个人版 Repository 与 Recovery Point 列表。

旧 `backup/src/gui` 是 Desktop 的视觉和交互基线。迁移品牌 PNG/ICO、默认 `blueExtra` 调色板、无边框标题栏、
可折叠侧栏、页面间距、卡片、表格和右侧抽屉结构。允许按新模块边界拆分和重写 QML，但同一页面已存在的
布局与状态表达必须保持一致。

不迁移 qmake 生成物、构建产物、超过源码限制的 Backend、HTTP/WebSocket 通信、TLS 校验豁免或让 GUI
直接执行备份、恢复、挂载和 Repository I/O 的代码。旧 Backend 的所有数据访问改写为版本化 Service IPC；
旧 UI 需要但 Service 尚未提供的操作保持禁用，不用本地直连临时补齐。

## 依赖与 Target

```text
src/apps/desktop/
├── CMakeLists.txt
├── resources/
├── qml/
└── src/
    ├── main.cpp
    ├── service_client.h
    └── service_client.cpp
```

`aegra_desktop` 只依赖 Qt Core/Gui/Qml/Quick/QuickControls2/Network。Qt 通过 `AEGRA_QT_ROOT` 或标准
`CMAKE_PREFIX_PATH` 提供，不在仓库写入开发机绝对 include/lib 路径。Desktop 不链接 Engine、Pipeline、
Repository、Windows Disk/VSS、数据库或 Worker 实现。

## ServiceClient

- 使用 `QLocalSocket` 连接 `aegra-service-control`。
- 发送和接收与 ADR-0011 相同的 4 字节长度帧，最大 64 KiB。
- 连接后生成新的 request ID 并发送 schema 2 `GetServiceInfo`，握手成功后分页发送
  `ListRecoveryPoints`。
- 只接受 schema、kind、request ID、字段类型和范围全部合法的响应。
- 每页最多 100 项，跨页 `file_uuid` 必须严格递增，token 必须前进，最多累计 10,000 项；全部页面完成后
  才原子发布给 QML。
- Repository 查询失败只更新 Repository 错误状态，不把已经 Ready 的 Service 误报为断开；协议损坏仍断开
  并重连。
- `refreshRepository()` 只在 Service Ready 且没有 pending request 时启动新分页查询；重复点击不会并发发送
  第二组请求。
- 断线或错误进入 Disconnected 状态，并以有界固定间隔重连；同一时刻最多一个连接尝试。
- QML 只观察 `connected`、`statusText`、`serviceVersion`、`apiVersion` 和 `errorText` 等拥有数据的属性。
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

页面沿用旧版 Repository 交互层级：Repository 卡片列表为主视图，点击恢复点数量打开覆盖主视图 90% 宽度的
右侧 Recovery Point 抽屉。当前 Service 只配置一个 Repository，因此列表最多一张卡片；不得伪造根路径、
默认库、锁或密码状态。

Recovery Point 列表展示备份类型、创建时间、逻辑/存储大小和链完整性。Catalog-only 数据不表示 Archive
结构或认证完成，因此 Restore、Verify 和 Delete 图标保持禁用。

Refresh 通过当前 Service session 重新分页查询；Repository 查询错误仍留在页面内，不破坏 Service Ready
状态。Add、Import、Set Default、Test、Lock、Unlock、Rebuild、Export、Password 和 Delete 在对应 Service
Use Case 接入前显示为禁用。布局必须在 900x600、1080x720 和更大窗口下不重叠。

## 测试与完成标准

- codec/framing 使用 Qt 单元测试覆盖拆包、粘包、超限和错误 request ID。
- Service 未运行、启动后连接、Service 退出和重启均能正确更新状态。
- QML 启动不引用旧 Backend，不绕过 Service 直接操作系统资源。
- 品牌资源来源和迁移范围可追溯，旧生成物不进入新项目。
- VS 2026 Insiders + Qt 6.8 构建通过，Desktop 能真实连接 `aegra_service.exe`。

## 当前状态

阶段 13C 已完成当前 Repository 纵向切片的视觉迁移。Desktop 使用独立私有 Qt codec 严格解析 schema 2，
握手后自动分页聚合 Recovery Point；自动化测试覆盖两页合并、opaque token、跨页乱序/重复拒绝、
NotConfigured、Repository RequestFailed、手动刷新、断线清空和重连重查。

QML 已采用旧项目 `blueExtra` 调色板、32px 无边框标题栏、160/56px 侧栏、Repository 卡片、底部操作栏和
90% 宽 Recovery Point 抽屉。未接入的页面与命令保留旧版视觉层级但禁止交互；Repository 查询和刷新仍只
通过 Service。布局已在 150% DPI 下按 1080x720 与最小 900x600 逻辑窗口验证，表格在最小宽度自动隐藏
`stored_size_bytes` 列并保持其余列无重叠。
