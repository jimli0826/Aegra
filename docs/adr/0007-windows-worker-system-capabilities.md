# ADR-0007：Windows Worker 系统能力与凭据引用

- 状态：Accepted
- 日期：2026-08-02
- 决策者：Aegra 项目
- 关联模块：ports、adapters/windows_system、apps/worker

## 背景

个人版 Worker 已有 `IClock`、`IRandomSource` 和 `ICredentialResolver` Port，但真实进程不能使用测试能力，
也不能从 Job、命令行或环境变量接收明文 Archive 密码。Worker 还需要一个可被 Management Service 监督
的最小进程协议，同时保持 stdout 不混入日志文本。

## 决策

1. Windows 系统能力放在独立 `Aegra::AdapterWindowsSystem` Target，由 Worker Composition Root 注入；
   Port 和核心模块不依赖 Windows SDK。
2. 随机数使用 `BCryptGenRandom` 的系统首选 RNG；系统时钟使用
   `GetSystemTimePreciseAsFileTime` 并转换为 Unix UTC 毫秒。
3. Archive 口令凭据引用只接受 `dpapi-lm:<entropy_id>:<base64>`。密文由 Service 用 DPAPI
   `CryptProtectData`（`CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN`）保护后 Base64 编码；
   **`pOptionalEntropy` 为 `entropy_id` 的 UTF-8 字节**（Schedule 口令的 `entropy_id` = `schedule_id`）。
   Worker 经 `ICredentialResolver` 解析出同一 `entropy_id` 后调用 `CryptUnprotectData`。
   不使用 Windows Credential Manager。
4. 解密后的明文立即复制到 `VirtualAlloc` 内存，并要求 `VirtualLock` 成功。锁页失败即拒绝凭据，不退化为
   普通可换页字符串。对象析构时先 `SecureZeroMemory`，再 unlock/free。
5. Job、响应、日志和错误都不返回密文或明文。`LOCAL_MACHINE` 范围使本机 Service 与 Worker 进程可共享
   同一受保护 Blob（无需共享用户 Credential Store）。
6. Schedule 创建时把 `dpapi-lm:<schedule_id>:<base64>` 写入控制面 SQLite
   `schedules.archive_password_protected`。`StartBackup` 仅接收 `schedule_id` + `backup_type`，Service
   从 Schedule 展开源/仓库/选项，并把已存密文放进 Worker Job 的 `credential_refs`（不再接受一次性口令）。
7. 首个 `aegra_personal_worker` 使用 stdin 接收至多 1 MiB 的单个 UTF-8 JSON Job，stdout 只输出一个
   JSON `WorkerResponse`，进程退出码按 Worker Host 规范返回。stderr 不构成协议。
8. block/chunk、内存预算、KDF 参数、应用版本和 hostname 由可执行程序的受信任配置构造，Job 不能
   覆盖。当前基线为 4 KiB block、1 MiB chunk、16 MiB pipeline memory、Argon2id opslimit 3 和
   256 MiB memory。
9. 当前 stdin 协议不承担运行中控制消息；外部停止 Port 已保留，但 Windows Service/Named Pipe 控制
   通道在后续阶段实现。deadline 仍能在运行中触发 Host cancellation。

## 备选方案

- Job 或环境变量传明文密码：容易进入任务库、进程列表、诊断转储和日志，不采用。
- Windows Credential Manager（`wincred://`）：跨服务账户/Worker 账户共享困难，且与“密文落库”模型不符，不采用。
- `VirtualLock` 失败后继续使用 `std::string`：违反敏感缓冲区最小暴露要求，不采用。
- 把 DPAPI 调用写入 `apps/worker` 业务任务：会让入口承担基础设施实现且不可复用，不采用；放在
  `adapters/windows_system`。
- 在 stdout 输出进度行和最终结果：增加 framing、截断和日志混淆风险，不采用。

## 影响

- 密文可随本机控制面 SQLite 与 Job JSON 传递；明文只在 Worker 解析后短暂驻留锁页内存。
- 账户没有足够 working-set quota 导致 `VirtualLock` 失败时任务会以凭据不可用失败，需要运维修正，
  不静默降低安全级别。
- stdin/stdout 入口足以做本地和父进程 MVP 集成，但强制停止与多事件进度需要后续双向控制通道。

## 验证

- Adapter 覆盖系统时钟、密码学随机、取消、scheme/空密文和 DPAPI 失败路径。
- 协议测试覆盖 1 MiB 上限以及解析拒绝不访问随机数或 DPAPI。
- 可执行烟测验证无效 Job 输出一个合法拒绝响应并以 20 退出。
- Debug/Release、clang-tidy、依赖边界、源码规模、差异和秘密扫描作为质量门禁。
