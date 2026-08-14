# ADR-0011：本地 Service 与 Desktop IPC

- 状态：Accepted
- 日期：2026-08-03
- 决策者：Aegra 项目
- 关联模块：contracts、adapters/windows_ipc、apps/service、apps/desktop

## 背景

个人版需要先建立 Desktop GUI 到本机 Management Service 的稳定连接，再逐步迁移 Repository、任务、备份、
恢复和挂载页面。旧 GUI 直接调用 `https://127.0.0.1:8999`，另开 WebSocket 端口接收任务事件，并包含忽略
自签名证书校验的客户端逻辑。旧 Backend 同时承担 Qt 状态、HTTP 编码、业务判断和本机操作，多个实现文件
超过新项目源码规模限制，不能作为新架构的兼容层直接迁移。

首阶段需要一个无需证书部署、只允许本机访问、支持版本化消息和后续扩展的控制通道。Worker Named Pipe
只服务父子任务进程，命名、ACL 责任和会话状态机都不适合直接复用为长期 Desktop API。

## 决策

1. 本地 Desktop 与个人版 Service 使用 Windows Named Pipe。公开逻辑名称为 `control`，实际路径为
   `\\.\pipe\aegra-service-control`；禁止远程 Pipe Client。
2. Service 是 Pipe Server，Desktop 是 Client。Service Adapter 使用进程 token 的默认 DACL；阶段 13A
   Service 与 Desktop 在同一交互用户下运行。安装为 LocalSystem Service 前必须增加显式用户授权与连接方
   本机 ACL 边界，不允许用 NULL DACL 或 Everyone full access 临时绕过；不执行调用方身份认证。
3. 传输使用 byte mode、4 字节 little-endian unsigned length 和 UTF-8 JSON body。零长度无效，Service API
   默认最大 frame 为 1 MiB（控制面 Service↔Desktop）。Adapter 只负责连接、监听、framing、取消和 Handle 生命周期，不解析 JSON。
4. `contracts` 定义传输无关的 `ServiceRequest`、`ServiceResponse` 与 `ServiceInfo`。每个根消息携带
   `schema_version` 和 `request_id`，枚举具有显式数值。JSON codec 只存在于 `apps/service`。
5. 阶段 13A 只支持 `GetServiceInfo`。成功响应返回 Service 版本、API 版本、`ready` 状态和 capability 列表；
   未知版本、未知 kind、损坏 JSON 和超限输入返回结构化拒绝，不输出解析器异常或客户数据。
6. 一个连接可以顺序发送多个请求。首版 Service 串行服务一个 Desktop 会话；连接断开后继续 accept。
   Desktop 使用有界重连，连接成功后立即握手，只有合法且 request ID 匹配的响应才进入 Ready 状态。
7. Desktop 使用 Qt 6.8/QML，但 Qt 只存在于 `apps/desktop`。Service、Contracts、Ports、Application 和
   数据面不依赖 Qt。
8. 从旧 GUI 只迁移品牌 bitmap/icon 和经过重新审查的视觉结构。旧生成物、qmake Makefile、超限 Backend、
   HTTP/WebSocket 客户端、自签名证书豁免和旧业务状态不复制。页面按新 Service API 逐个改造。
9. 当前产品未发布，不实现旧 HTTP API、旧 WebSocket 消息或旧 GUI 配置的兼容分支。

## Wire Schema 1

请求：

```json
{"schema_version":1,"request_id":"<uuid>","kind":1}
```

成功响应：

```json
{"schema_version":1,"request_id":"<uuid>","kind":1,"boundary_error_code":0,"message_code":"service.ready","service":{"api_version":1,"state":2,"service_version":"0.1.0","capabilities":["service.info"]}}
```

`kind=1` 分别表示 `GetServiceInfo` 请求和 `ServiceInfo` 响应；响应 `kind=2` 表示 Request Rejected。
`state=2` 表示 Ready。字段名和数值是 schema 1 的持久跨进程契约。

## 备选方案

- 继续旧 HTTPS + WebSocket：首阶段需要证书生命周期、两个端口和更多攻击面，旧实现还绕过证书验证，不采用。
- Qt Remote Objects：会把 Qt 协议和类型扩散到 Service，不采用。
- gRPC：长期远程管理可考虑，但本地首阶段引入成本高，且不能替代 Windows 本机授权设计，不采用。
- 复用 Worker Pipe：Worker 是每任务父子会话，Service 是长期控制面 API，权限和状态机不同，不采用。
- Desktop 直接链接备份引擎：违反 GUI 只调用版本化 IPC、任务在 Worker 执行的进程边界，不采用。

## 影响

- Desktop 可以在没有 TLS 证书和监听端口的情况下检测 Service 版本与 capability。
- 后续请求必须扩展 Service schema 和 Host dispatcher，不能让 QML 直接调用数据库、Archive 或 Windows API。
- 当前同用户默认 DACL 只适合开发和个人版前台运行；Windows Service 安装、用户授权和多会话属于后续安全
  阶段，必须在正式后台部署前完成。
- 旧 GUI 页面不会一次性恢复功能；它们按 Service API 和新模块边界逐页迁移。

## 验证

- Contracts 测试覆盖版本、request ID、枚举、响应 payload 和错误组合。
- JSON codec 测试覆盖 golden、roundtrip、未知字段、错误类型、超限和损坏输入。
- Named Pipe 测试覆盖 Service namespace、accept、双向 framing、frame 上限、取消和断线。
- 真实 Service 进程测试覆盖 `--once`、Desktop 等价请求和结构化 Ready 响应。
- Desktop Client 测试覆盖 framing、request ID 对账、错误响应和重连状态。
- Debug/Release、源码规模、依赖边界、格式和秘密扫描通过。
