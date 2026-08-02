# Worker Host 与进程协议

## 目标与边界

`apps/worker` 的 Worker Host 每次只执行一个已接收的任务。它负责跨进程消息校验、受信任运行配置、
外部停止与 deadline 合并、任务调用、异常收口、稳定退出码和响应编码，不负责备份算法、凭据持久化、
任务调度或控制面数据库访问。

当前协议使用 UTF-8 JSON。JSON 依赖只存在于 `apps/worker`，`contracts` 保持与传输技术无关。后续改用
命名管道、HTTPS 或 Protobuf 时，`JobRequest`、`TaskResult` 和 `WorkerResponse` 的语义保持不变。

## 请求协议

根对象是 `JobRequest`，字段名固定如下：

| 字段 | JSON 类型 | 规则 |
| --- | --- | --- |
| `schema_version` | unsigned integer | 当前固定为 `1` |
| `job_id` | string | 必填、非空 |
| `tenant_id` | string | 必填、非空 |
| `operation` | unsigned integer | 使用 `JobOperation` 的显式数值 |
| `source_refs` | string array | 至少一个非空值 |
| `target_ref` | string | 必填、非空 |
| `credential_refs` | string array | 只允许 `SecretRef` 定位符 |
| `trace_id` | string | 必填、非空 |
| `deadline_utc_ms` | signed integer | 可选；`0` 表示无 deadline |

顶层 `password` 和 `secret` 字段一律拒绝。解析器检查 JSON 类型和整数范围，未知 schema、无效枚举、
字段缺失或格式错误都表示请求未被接受。业务运行参数，例如 block/chunk 大小、KDF 参数、应用版本和
主机名，来自 Worker 的受信任配置，不从 Job 消息接收。

## 响应协议

`WorkerResponse` 的 `schema_version` 当前固定为 `1`，包含 `job_id`、`trace_id`、`kind`、
`boundary_error_code`、`message_code` 和可空 `task_result`。枚举按显式无符号数值编码。

| `kind` | 含义 | `task_result` | Boundary Error |
| --- | --- | --- | --- |
| `1` | 已接受任务的最终结果 | 必须存在 | `kNone` |
| `2` | schema 或请求校验拒绝 | 不存在 | `kInvalidArgument` 或 `kUnsupportedVersion` |
| `3` | Host、协议或边界故障 | 不存在 | 非校验类稳定错误 |

Host 在编码前验证响应，并确保响应及内部 `TaskResult` 的 job/trace 与输入 Job 一致。Adapter 原始错误、
异常文本、路径、SecretRef 和明文凭据不得进入响应。

## 退出码

| 数值 | 名称 | 含义 |
| ---: | --- | --- |
| `0` | `kSucceeded` | 成功或带告警成功 |
| `10` | `kTaskFailed` | 已接受任务运行失败 |
| `11` | `kCancelled` | 外部停止、deadline 或任务取消 |
| `20` | `kRequestRejected` | 请求未被接受 |
| `21` | `kHostFailure` | Host 异常、时钟或无效内部响应 |

退出码只用于进程监督和快速分类；Management Service 必须以结构化 `WorkerResponse` 为详细事实，不能
解析 stderr 或日志文本充当协议。

## 取消与生命周期

Host 创建局部 `CancellationSource`，通过 `stop_callback` 合并外部进程停止，并在 deadline 到达时请求
停止。deadline 监视线程由 RAII 管理；任务提前结束时唤醒并 join，不残留后台线程。系统时钟为负数时
任务不得启动。取消令牌贯穿凭据解析、VSS、块读取、Archive 写入与 Pipeline。

Host 核心收口任务执行抛出的异常；未来真正的 `main` 仍必须作为进程级最后异常边界，因为构造拥有字符串
的响应本身可能因资源耗尽失败。Host 是同步、单任务对象，不持有任务完成后的权威状态。凭据仍只在任务
入口同步调用期间以 `IResolvedSecret` 存活，协议响应不会返回凭据引用。

## 测试与完成标准

- JSON 覆盖合法请求、错误类型、未知版本、数值溢出和明文凭据字段；
- Host 覆盖成功、任务失败、请求拒绝、异常、无效响应、外部取消、运行中 deadline 和无效时钟；
- 所有响应通过契约校验，job/trace 不得串任务；
- Host 与协议代码只位于 `apps/worker`，核心模块不依赖 JSON、线程 Host 或 Windows API；
- Debug/Release、源码限制、静态分析、依赖检查和秘密扫描通过。
