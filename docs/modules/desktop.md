# Desktop GUI 开发文档

## 目标与迁移范围

`apps/desktop` 是普通用户 Qt/QML 客户端，只通过版本化 Service IPC 获取状态和发起用例。阶段 13B 在连接、
握手和重连骨架上增加个人版 Repository 与 Recovery Point 列表。

旧 `backup/src/gui` 只作为视觉和交互参考。迁移品牌 PNG/ICO、窗口结构和必要 QML 组件；不迁移 qmake
生成物、构建产物、超过源码限制的 Backend、HTTP/WebSocket 通信、TLS 校验豁免或让 GUI 直接执行备份、
恢复、挂载和 Repository I/O 的代码。

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
- 断线或错误进入 Disconnected 状态，并以有界固定间隔重连；同一时刻最多一个连接尝试。
- QML 只观察 `connected`、`statusText`、`serviceVersion`、`apiVersion` 和 `errorText` 等拥有数据的属性。
- 日志不输出 frame body、路径、凭据或 Service 原始错误文本。

## 首屏

首屏是实际 Desktop 工作区，不是营销页。左侧保留旧 GUI 的紧凑导航结构，但未接入的功能明确禁用；主区域
显示 Service 状态、Repository 状态、UUID 和 Recovery Point 摘要。连接失败时提供重试命令，不用说明性
大段文字替代状态。

Recovery Point 列表展示备份类型、创建时间、逻辑/存储大小和链完整性。Catalog-only 数据不表示 Archive
结构或认证完成，因此 Restore、Verify 和 Delete 图标保持禁用。

颜色从旧界面演进为中性浅色工作区，蓝色只作为操作强调，绿色/红色分别表达 Ready 和错误，避免单一深蓝
主题。布局必须在 1024x640 和较小桌面窗口下不重叠。

## 测试与完成标准

- codec/framing 使用 Qt 单元测试覆盖拆包、粘包、超限和错误 request ID。
- Service 未运行、启动后连接、Service 退出和重启均能正确更新状态。
- QML 启动不引用旧 Backend，不绕过 Service 直接操作系统资源。
- 品牌资源来源和迁移范围可追溯，旧生成物不进入新项目。
- VS 2026 Insiders + Qt 6.8 构建通过，Desktop 能真实连接 `aegra_service.exe`。

## 当前状态

阶段 13B 已完成。Desktop 使用独立私有 Qt codec 严格解析 schema 2，握手后自动分页聚合 Recovery Point；
自动化测试覆盖两页合并、opaque token、跨页乱序/重复拒绝、NotConfigured、Repository RequestFailed、断线
清空和重连重查。QML 首屏已改为 Recovery Point 工作区，且不链接 Repository、Storage 或 Application。
