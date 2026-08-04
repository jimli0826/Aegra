# ADR-0014：Windows Service 与本地 IPC 安全边界

- 状态：Accepted
- 日期：2026-08-03
- 决策者：Aegra 项目
- 关联模块：adapters/windows_ipc、apps/service
- 关联：ADR-0011（传输与命名）、ADR-0013（V3 协议，不变）

## 背景

阶段 13A/S0 的 Service 以交互用户运行，Named Pipe 使用进程默认 DACL，且不校验连接方身份。个人版正式
后台部署需要 LocalSystem（或等价高权限）Service，同时 Desktop 继续以普通交互用户连接。

S1 必须在不改动 Service V3 公共协议与 codec 的前提下，建立 SCM 生命周期、Pipe 安全边界、调用方身份
校验和可测的安装/恢复/卸载入口。

## 决策

1. Pipe ACL 分用途配置，**始终**设置 `PIPE_REJECT_REMOTE_CLIENTS`，禁止 NULL DACL / Everyone full：
   - `kProcessDefault`：进程 token 默认 DACL，用于 Worker 父子进程私有 Pipe。
   - `kLocalEveryoneControl`：Service 控制 Pipe 的显式 DACL
     （`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)`）。LocalSystem 与 Administrators 拥有 full access，
     Everyone 仅拥有读写；交互模式和 SCM 模式统一使用，保证提升权限运行的 Service 可由普通 Desktop 连接。
2. Accept 后通过客户端进程 token 查询调用方身份：用户 SID、session ID、process ID，以及是否
   interactive/administrator。不把高权限句柄交给 Desktop。
3. 授权策略保持简单：`kLocalInteractiveOrAdmin` —— 本机 interactive session（session > 0）或
   Administrators。当前不引入 SID allowlist。未授权连接断开并返回 `kUnauthorized`。
4. Service Host 提供 `run_authorized_service_host`：在授权 accept 上运行既有 V3 session；取消会唤醒
   pending accept/receive。
5. SCM 生命周期通过 `IWindowsServiceControlManager` 暴露：
   - `install_windows_service` 先 create 再配置 recovery；recovery 失败 delete 以支持 rollback
   - `stop_service` 在 Win32 实现中 `ControlService(STOP)` 后用 `QueryServiceStatusEx` 有界等待
     `SERVICE_STOPPED`，避免 restart 在 `STOP_PENDING` 时调用 `StartService`
   - `restart_windows_service` / `uninstall_windows_service` 依赖上述 stop 语义
6. `WindowsServiceScmHost` 在独立线程运行 worker：`request_stop` 后在 `stop_deadline` 内等待；
   超时则报告 Stopped 并返回 `kInternal`（非协作 worker 被 detach，由进程退出回收）。
7. 不修改 Service V3 协议字段、codec、SQLite、Repository 或 Worker Supervisor。

## 备选方案

- 仅允许 Interactive Users：无法覆盖所有本机用户 token，不采用。
- 首版引入 SID allowlist：部署复杂度高于当前个人版需求，延后。
- Everyone full access 或 NULL DACL：权限超过连接与收发所需，不采用。
- stop 后不写等待直接 start：真实服务常处于 STOP_PENDING，restart 间歇失败，不采用。

## 影响

- Service 控制 Pipe 在开发/交互态与正式 LocalSystem 模式统一使用 `kLocalEveryoneControl`。
- Worker 私有 Pipe 继续使用 `kProcessDefault`。
- SCM restart/uninstall 在生产路径上会阻塞至 STOPPED 或 deadline。
- 忽略取消的 worker 不会无限阻塞 ServiceMain；超时后 host 返回失败。

## 验证

- Named Pipe：process-default 连接、peer identity、多 session 授权、非交互拒绝、accept 取消。
- Service host：授权 once 成功、stop 取消 pending accept。
- SCM：install/recovery/restart/uninstall、recovery rollback（Fake）。
- SCM host：cooperative stop 与 non-cooperative stop_deadline。
- Debug/Release 构建与源码规模门禁通过。
