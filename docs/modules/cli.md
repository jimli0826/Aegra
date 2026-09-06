# `apps/cli` 开发文档

## 目标与非目标

`apps/cli` 是本机 Management Service 的命令行客户端（`AegraCLI.exe`）。它只通过版本化 Service 控制协议与 `AegraService.exe` 交互，用于运维与诊断：列出 Schedule / Job / Repository、立即运行备份任务、对非系统测试盘发起已预检的整盘恢复、取消或等待 Job。

非目标：

- 不执行 Backup/Restore 数据面，不打开 `.bkf`，不访问 SQLite 控制面；
- 不创建或编辑 Schedule（`UpsertSchedule` 需要 Desktop 浏览 token 与完整冻结字段）；
- 不直接打开 Archive、执行数据面、启动卷恢复、加密 Archive 恢复、WinPE 或文件浏览；整盘恢复只支持空密码，且 CLI 在同一会话完成 opaque preflight token 获取和精确目标二次确认；Mount 命令只转发未加密 Recovery Point 的 Service 控制请求；
- 不使用 Qt，不链接 Application、Pipeline、Personal Repository 或 Windows Disk/VSS。

## 允许和禁止依赖

允许：`Aegra::AppServiceProtocol`（Service V4 JSON codec）、`Aegra::AdapterWindowsIpc`、`Aegra::AdapterWindowsSystem`（随机数）、`Aegra::Contracts`、`Aegra::Base`。

禁止：`Aegra::AppService` Host、Application、Pipeline、Format、PersonalRepository、SQLite、Windows Disk/VSS/Filesystem、Qt、Dokan。

JSON codec 与 Service Host 共享 `aegra_app_service_protocol`，避免 CLI 复制 exact-keys wire 实现。该库只依赖 Contracts / Base / nlohmann-json。

## 公共接口

进程入口：`AegraCLI [global-options] <noun> <verb> [options]`

| 命令 | Service kind | 说明 |
| --- | ---: | --- |
| `status` | 1 | 握手后的 ServiceInfo |
| `schedule list` | 6 | 分页列出 Schedule |
| `schedule run --id` | 37 | `StartBackup`；默认 Incremental |
| `schedule delete --id` | 44 | 删除 Schedule |
| `job list` | 5 | 默认 `scope=active` |
| `job cancel --id` | 38 | 取消 Job |
| `job wait --id` | 5 | 轮询直到终态 |
| `restore run` | 9 + 40 | 非系统整盘恢复；同一会话完成 `PrepareRestore` 与 `StartRestore` |
| `repository list` | 3 | Repository connection |
| `recovery-point list` | 2 | Catalog 摘要 |
| `inventory list` | 4 | 本机 Source inventory |
| `event list` | 7 | Audit events |
| `mount list` | 8 | Mount sessions |
| `mount start` | 41 | 挂载未加密 Recovery Point，可指定首选盘符 |
| `mount unmount` | 42 | 卸载 Mount session |
| `settings get` | 16 | 控制面偏好 |

`restore run` 必须提供 `--connection`、`--recovery-point`、`--target disk.N`、`--source-disk-number`，并以完全相同的 `--confirm-target disk.N` 明确确认不可逆目标。默认保留源 disk signature 并自动扩展末分区；自动化验证环境可使用 `--regenerate-disk-signature` 避免源/目标同时在线时发生签名冲突，或用 `--no-auto-expand` 保持源布局。CLI 不接受 Archive 密码，因此仅支持未加密恢复。

`mount start` 必须提供 `--connection`、`--recovery-point` 和 `--source-disk-number`；`--drive-letter` 可指定 `D`–`Z` 的首选盘符。CLI 不接受 Archive 密码，因此仅支持未加密挂载。成功 acknowledgement 的 `resource_id` 是 Mount session id，供 `mount list` 与 `mount unmount --id` 使用。

全局选项：`--json`（打印 Service V4 响应 JSON）、`--timeout-ms`（单请求，默认 30s）、`--wait-timeout-ms`（`job wait` / `schedule run --wait` / `restore run --wait`，默认 1h）。

退出码：0 成功；1 用法错误；2 无法连接或握手失败；3 请求被 Service 拒绝；4 Job 失败/取消/等待超时；5 内部错误。

所有权：CLI 拥有 Named Pipe 会话；每条命令一次连接、握手、执行后退出。`--wait` 期间继续使用同一会话轮询 `ListJobs`。取消通过请求 deadline 的 `stop_token` 传到 Pipe `CancelIoEx`。

线程：主线程同步收发。每条请求有独立 deadline 监视线程；析构时 stop 并 join。

错误：连接失败映射稳定 `ErrorCode`；业务失败打印 `message_code` 与 `message_arguments`，不打印密码、SecretRef 或认证材料。

## 核心不变量

- 传输与 Desktop 相同：`\\.\pipe\aegra-service-control`，4 字节 LE 长度前缀，最大 1 MiB，schema 4。
- 查询 `idempotency_key = null`；命令生成 `cli:<uuid>` 幂等键。
- 列表自动跟随 `continuation_token`，最多 10,000 项；token 必须前进。
- CLI 不发送绝对路径、Volume GUID、Archive 密码或明文网络凭据；恢复目标只接受 Inventory opaque `disk.N`。
- `restore run` 必须先获得 `restore_eligible=true` 的 Service preflight，并要求 `--confirm-target` 与 `--target` 字节完全一致；CLI 不缓存或输出 preflight token。
- Service 未运行时连接失败并退出码 2，不启动 Service。连接等待最多 5 秒（不超过 `--timeout-ms`）。

## 目录与 CMake Target

```text
src/apps/cli/
├── CMakeLists.txt
└── src/
    ├── main.cpp
    ├── cli_args.*
    ├── cli_session.*
    ├── cli_commands*.cpp
    ├── cli_format.*
    ├── cli_ids.*
    └── cli_io.*
```

- `aegra_cli`：Windows console EXE；构建后复制到 `aegra_service` 输出目录便于本机运行。

## 验证

- 构建 `aegra_cli` 与 `aegra_app_service_protocol`。
- `CheckSourceLimits`。
- 人工：Service 未运行时连接失败；Service Ready 后 `status`、`schedule list`、`job list`；对已有 Schedule `schedule run --id` 得到 CommandAccepted；隔离测试盘上确认错误/缺失 `--confirm-target` 无请求副作用，正确确认后 preflight 和整盘 Restore Job 均由 Service 接受。

## 可观测性与安全

CLI 默认不写日志文件。stdout 为表格或 JSON；stderr 为错误。`--json` 输出 Service codec 编码的响应，字段集合与 Desktop/Service 相同，不含认证材料。恢复 preflight 响应不写 stdout，避免泄漏短期 token；只输出最终 StartRestore acknowledgement。

## Definition of Done

- CLI 是独立 Composition Root，只经 Named Pipe 调用 Service V4。
- Schedule 列表、立即备份与隔离测试盘的已确认整盘恢复可在人工运行中完成。
- 文档、安装清单与模块边界同步更新。
