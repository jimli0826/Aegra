# ADR-0008：Worker Session 与本地 Named Pipe 传输

- 状态：Accepted
- 日期：2026-08-02
- 决策者：Aegra 项目
- 关联模块：contracts、ports、adapters/windows_ipc、apps/worker

## 背景

stdin/stdout 单请求模式不能在任务运行中传递取消命令，也不能持续发送结构化进度。Worker 仍需保持
每任务独立进程、跨进程契约版本化、核心模块不依赖 Windows/JSON，并允许父进程可靠区分进度与最终结果。

## 决策

1. `contracts` 定义版本化 `WorkerCommand` 与 `WorkerEvent`。会话首帧必须是 `JobRequest`；运行中父进程
   最多发送一个与该任务关联的 `Cancel`。Worker 可以发送零到多个 `Progress`，并在传输仍可用时发送
   恰好一个最终 `Result`。
2. 每个 Command/Event 都携带 `job_id` 与 `trace_id`。Cancel 关联不匹配、未知命令或损坏命令均停止任务，
   最终映射为稳定 `worker.command_failed` Host failure。
3. `WorkerEvent` 的 Progress 与 Response payload 互斥。最终 Result 复用 `WorkerResponse`，请求拒绝和 Host
   failure 也通过 Result event 表达。
4. `ports::IMessageChannel` 只抽象拥有所有权的 UTF-8 消息；允许一个 Reader 与一个 Writer 并发，要求
   实现限制帧大小并使挂起 I/O 响应取消。
5. Windows 传输由父进程先创建本地 Named Pipe Server，Worker 通过逻辑名称作为 Client 连接。逻辑名称
   只允许 `[A-Za-z0-9_.-]` 且最长 128 字节；Worker 不接受 UNC、完整 Device Path 或远程 Pipe。
6. Named Pipe 使用 byte mode 和固定 framing：4 字节 little-endian unsigned length，随后是 UTF-8 JSON。
   零长度帧无效，默认最大帧为 1 MiB。连接和读写使用 Overlapped I/O；取消通过 `CancelIoEx` 唤醒。
7. Named Pipe 可同时存在一个读取和一个写入操作。Worker Session 只有任务线程发送 Progress，任务完成并
   停止 Command Listener 后才发送 Result，因此不产生并发双写。
8. stdin/stdout 保留为本地诊断和最小入口，但正式父进程监督使用 `--pipe <logical-name>` 会话模式。
9. Pipe Server 的 ACL 由父进程按实际 Worker SID 和最小权限创建。Worker Adapter 只连接既有 Server，
   不创建 Server、ACL 或跨账户授权。

## 备选方案

- stdout 行协议：无法可靠区分日志、截断帧与并发进度，不采用。
- Worker 创建 Named Pipe Server：会把命名、ACL 和账户授权责任放到低权限子进程，不采用。
- 直接在 `apps/worker` 使用 Win32 Handle：会让状态机和传输实现耦合，无法用内存 Channel 测试，不采用。
- Message Mode Named Pipe：仍需定义跨实现 framing，且长度前缀更容易设置统一上限，不采用。
- 首阶段使用 gRPC/HTTP：部署和依赖成本高于本地单机父子进程需求，后续可用同一 Port 增加实现。

## 影响

- Management Service/父进程必须先创建 Pipe、应用最小 ACL，再启动 Worker，并以结构化 Result 为任务事实。
- 父进程断开或停止读取时，Worker 将传输错误映射为 Host failure；传输已不可用时无法保证 Result 送达，
  父进程必须结合进程退出码判定会话异常。
- JSON 仍只存在于 `apps/worker`；Named Pipe Adapter 不解析消息内容，Contracts 不依赖 JSON 或 Win32。
- 当前协议只允许单任务和单 Cancel；若增加暂停、凭据刷新或多任务复用，必须升级 Command schema 并补 ADR。

## 验证

- Contracts 测试覆盖版本、payload 互斥和 job/trace 关联。
- 内存 Channel 测试覆盖 Progress、Cancel、错误 Command、关联不匹配和唯一最终 Result。
- Named Pipe 测试覆盖名称限制、双向 framing、并发读写、长度边界、取消和 Server 断线。
- 真实 Worker 进程测试覆盖 `--pipe` 连接、无效 Job 的单 Result 和退出码 20。
- Debug/Release、clang-tidy、依赖边界、源码规模和秘密扫描作为质量门禁。
