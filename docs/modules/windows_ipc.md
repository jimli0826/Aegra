# Windows Named Pipe IPC Adapter

## 目标与非目标

`adapters/windows_ipc` 提供本地 Named Pipe 传输：Worker 父子会话 Client，以及 Service 控制面 Listener。
Adapter 只负责连接、监听、framing、取消、Handle 生命周期与显式 ACL；不解析 JSON、不实现
Service 协议、不启动 SCM。

## 依赖

- Target：`aegra_adapter_windows_ipc` / `Aegra::AdapterWindowsIpc`
- 仅依赖 `Aegra::Ports` 与 Windows Named Pipe / Advapi32 API
- 公共头不 include `Windows.h`

## 公共接口

### `WindowsNamedPipeChannel`

实现 `ports::IMessageChannel`。逻辑名称最长 128 字节，字符集 `[A-Za-z0-9_.-]`，映射为：

- Worker：`\\.\pipe\aegra-worker-<name>`
- Service：`\\.\pipe\aegra-service-<name>`

传输：byte mode、4 字节 little-endian 长度前缀、UTF-8 body；零长度非法。默认最大帧 1 MiB
（Service 控制面与 Worker session 均为此上限；Listener 可配置，硬上限 1 MiB）。

### `WindowsNamedPipeListener`

Service 侧监听器。`WindowsNamedPipeListenRequest` 支持 ACL 配置：

| Profile | 行为 |
| --- | --- |
| `kProcessDefault` | 进程 token 默认 DACL + 拒绝远程（Worker 私有 Pipe） |
| `kLocalEveryoneControl` | Everyone 本机读写 + 拒绝远程（Service 控制 Pipe） |

Listener 只接受本机连接；Service 不读取客户端进程 token，不执行 SID、session 或管理员身份认证。
连接生命周期内的业务 token 仅绑定 Service 生成的不可预测 session id。

## 核心不变量

- 拒绝远程 Pipe Client
- Service 控制 Pipe 允许 Everyone 读写，但不授予 full access
- Worker 私有 Pipe 继续使用进程默认 DACL
- 取消通过 `CancelIoEx` 唤醒 pending connect/read/write
- 错误映射为稳定 `ErrorCode`，消息不含客户路径或数据
- Adapter 不解析 JSON

## 验证

- 审查 framing、取消、断线、名称校验、ACL 和 accept 取消路径。
- 使用临时本地 Pipe 执行隔离的人工运行验证。

## Definition of Done

- 公共头无 Win32 类型泄漏
- Service 本机 ACL 边界清晰且可独立人工验证
- Debug/Release 与源码规模门禁通过
