# 本地 Management Service Host 开发文档

## 目标与非目标

`apps/service` 是个人版本地控制面的进程入口和协议 Host。阶段 13A 提供可被 Desktop 发现和握手的
`GetServiceInfo`，建立后续 Repository、任务和策略 API 的稳定入口。

本阶段不执行备份/恢复、不直接读取 `.bkf`、不访问 SQLite/PostgreSQL、不启动 Worker、不实现管理员安装、
SCM 生命周期或跨用户授权。业务能力增加时必须先进入 Application Use Case，再由 Service Controller 调用。

## 依赖与 Target

```text
src/apps/service/
├── CMakeLists.txt
├── include/aegra/apps/service/
│   ├── service_host.h
│   └── service_protocol.h
└── src/
    ├── service_host.cpp
    ├── service_main.cpp
    └── service_protocol.cpp
```

- `aegra_app_service` / `Aegra::AppService`：依赖 Base、Contracts、Ports 和 PRIVATE nlohmann-json。
- `aegra_service.exe`：Composition Root，依赖 AppService 与 WindowsIpc Adapter。
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

- schema 1 只支持 `GetServiceInfo`。
- `request_id` 用于请求响应对账，不作为权限或幂等凭据。
- 响应只包含产品版本、API 版本、稳定状态和 capability 名，不包含主机名、路径、SID、SecretRef 或配置。
- frame 最大 64 KiB，JSON 根必须是 object，整数必须先检查范围。
- 错误响应使用稳定 `ErrorCode` 和 message code，不返回 JSON/Win32 异常文本。
- 阶段 13A 使用同用户默认 Pipe DACL；禁止远程 Client。正式 Windows Service ACL 是后续上线门禁。

## 测试与完成标准

- 契约、codec、dispatcher、session 和真实进程均有确定性测试。
- Service 可独立启动，Desktop 可以握手并显示 Ready 与版本。
- Service 不依赖 Qt、具体备份 Adapter、Personal Repository 或数据库。
- 未知版本、损坏请求、断线和取消不会终止整个常驻 Host。
- Debug/Release、源码规模、格式和秘密扫描通过。

## 当前状态

阶段 13A 已完成。`aegra_service` 支持常驻模式、`--once` 和受限逻辑 Pipe 名；Contracts、严格 JSON
codec、dispatcher、session、listener 双向 framing/取消和真实子进程均有自动化测试。当前仍以交互用户运行，
正式 Windows Service 安装、显式 ACL 和连接方身份校验不在本阶段范围内。
