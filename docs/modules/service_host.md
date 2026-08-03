# 本地 Management Service Host 开发文档

## 目标与非目标

`apps/service` 是个人版本地控制面的进程入口和协议 Host。阶段 13B 提供 Desktop 握手和个人版 Repository
Catalog 分页查询，建立后续任务和策略 API 的稳定入口。

Service Host 负责 framing、dispatch、会话生命周期、取消收口，以及 S1 起的正式 Windows Service 安全边界
（授权 accept、SCM 安装/恢复/卸载入口）。不执行备份/恢复、不直接读取 `.bkf`、不访问 SQLite/PostgreSQL、
不启动 Worker。Repository 路径只来自受信任进程参数；Desktop 请求不能指定路径。

## 依赖与 Target

```text
src/apps/service/
├── CMakeLists.txt
├── include/aegra/apps/service/
│   ├── service_host.h
│   ├── service_protocol.h
│   ├── service_security_host.h
│   ├── windows_service_control.h
│   └── windows_service_scm_host.h
└── src/
    ├── service_host.cpp
    ├── service_main.cpp
    ├── service_protocol.cpp
    ├── service_protocol_request_json.cpp
    ├── service_protocol_response_json.cpp
    ├── service_security_host.cpp
    ├── windows_service_control.cpp
    ├── windows_service_control_win32.cpp
    └── windows_service_scm_host.cpp
```

- `aegra_app_service` / `Aegra::AppService`：依赖 Application、Base、Contracts、Ports、WindowsIpc；
  PRIVATE nlohmann-json 与 Advapi32。
- `aegra_service.exe`：Composition Root，依赖 AppService、WindowsIpc 和 Local Storage Adapter。
- JSON、Win32、Qt、数据库类型不得进入 Contracts。

## 生命周期

Service 默认监听逻辑名称 `control`，实际 Pipe 名称见 [ADR-0011](../adr/0011-local-service-desktop-ipc.md)。
`--once` 只接受一个连接并处理一个请求，用于真实进程测试；默认模式在会话断开后继续接受连接。

每个 session 顺序执行：

```text
Accept -> Receive Frame -> Decode/Validate -> Dispatch -> Encode -> Send -> Receive Next
```

断线结束当前 session，不结束 Service。取消必须唤醒 pending accept/receive。未知请求形成拒绝响应；只有传输
已经不可用或响应编码失败时 Host 返回边界错误。

## 协议与安全

- schema 3 使用 Request/Response/Event envelope，不兼容未发布的 schema 1/2；详见
  [ADR-0013](../adr/0013-service-control-protocol-v3.md)。
- `request_id` 用于请求响应对账，不作为权限或幂等凭据；查询不携带幂等键，命令必须携带幂等键。
- V3 已定义全部规划内 request kind 和 payload codec。当前只声明并执行 `service.info` 与 `repository.list`；
  其他合法请求返回 `service.capability_unavailable`，不产生副作用。
- Repository 响应只包含 Repository UUID 和不含客户 Metadata 的 Catalog 摘要，不包含根路径、Archive key、
  主机名、SID、SecretRef 或原始 Adapter 错误。
- frame 最大 64 KiB，JSON 根必须是 object，整数必须先检查范围。
- 错误响应使用稳定 `ErrorCode` 和 message code，不返回 JSON/Win32 异常文本。
- S1：交互模式 `kProcessDefault` + 拒绝远程 Client + 调用方 SID/session 授权；LocalSystem 正式模式再切
  `kServiceLocalControl`。见 [ADR-0014](../adr/0014-windows-service-ipc-security.md) 与
  [windows_ipc.md](windows_ipc.md)。
- 授权 Host：`run_authorized_service_host`；SCM stop 有界等待 STOPPED；`WindowsServiceScmHost`
  在 `stop_deadline` 内收口（非协作 worker 超时失败）。

## 测试与完成标准

- 契约、codec、dispatcher、session 和真实进程均有确定性测试。
- 授权成功、未授权拒绝、多 session、stop 取消 pending accept、install/recovery/restart/uninstall 与
  recovery 失败 rollback 有确定性测试。
- Service 可独立启动，Desktop 可以握手并显示 Ready 与版本。
- Service 不依赖 Qt、具体备份 Adapter 或数据库权威。
- 未知版本、损坏请求、断线和取消不会终止整个常驻 Host。
- Debug/Release、源码规模、格式和秘密扫描通过。

## 后续控制面工作

Service 剩余工作按 [Desktop 迁移与个人版 Service 完成计划](../migration/DESKTOP_SERVICE_COMPLETION_PLAN.md)
中的 `S2` 至 `S8` 工作包推进。S2 已提供独立 SQLite 控制面 Adapter 与 ports（见
[control_plane_sqlite.md](control_plane_sqlite.md)），尚未由 Service composition root 打开数据库或暴露
对应 capability。后续：接入控制面、Worker Supervisor、Repository/Inventory API、
Restore/Verify/Mount 编排和计划/事件查询。现有 Worker 数据面继续复用，Service 不重复实现 Backup、
Verify 或 Restore Pipeline。Composition root 仍需接入正式 Service ACL/授权 Host 与 SCM CLI 入口。

Service 协议只返回稳定枚举、错误码和 message code，不返回本地化文本。个人版 SQLite 不保存 Recovery Point、
Chunk Index、Manifest 或 Archive metadata 的权威副本；Repository 仍是可重建 Recovery Point 事实来源。

## 当前状态

S0 已完成 Service V3。S1 已完成库级 Windows Service 安全边界：显式 Pipe ACL、调用方 SID/session 校验、
授权 accept Host、SCM install/uninstall/recovery/restart 入口与 ServiceMain 状态机。`aegra_service`
Composition Root 仍以交互用户 `--once`/`--pipe` 模式运行，尚未接线 `--service` /
`--install` / `--uninstall`；见完成计划交接清单。
