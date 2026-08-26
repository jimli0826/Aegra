# `apps/pe_restore` 模块开发文档

WinPE 内的最小离线恢复执行器（`aegra_pe_restore.exe`，Win32 子系统，全屏深色 UI）。
设计权威：[WinPE 离线系统盘恢复设计](../architecture/WINPE_OFFLINE_RESTORE.md) §9、
[ADR-0026](../adr/0026-winpe-offline-restore-and-secret-envelope.md)。当前状态为 **PE3 交付**。

在线侧的编排由 application 层 `PeRestorePrepareService` 承担（PE4a，见下节），本执行器只在
WinPE 内消费其写下的 Pending Job。

## 目标与非目标

- 目标：读取跨重启 Pending Job，解封密码，重匹配目标磁盘身份，经 Worker Session 协议
  驱动 `aegra_personal_worker.exe` 完成整盘恢复，写回 Result 并重启。
- 非目标：链解析（在线定稿）、还原数据面（Worker 进程内）、配置中心（参数全部来自 Job）。

## 进程架构（mini-supervisor）

```text
winpeshl → wpeinit → aegra_pe_restore.exe
                        │ 卷扫描定位 restore_job.v1.json → 完整读入内存
                        │ 信封解封（sealed）/ 交互输入（prompt）→ consume_job_key
                        │ 序列号+容量重匹配 PhysicalDriveN（0/多命中即拒绝）
                        │ 密码 → DPAPI(machine, 本 PE 会话) → dpapi-lm SecretRef
                        ▼
             监听随机命名管道（kWorker 命名空间，1 MiB 帧）
                        │ spawn aegra_personal_worker.exe --pipe <name>
                        │ send JobRequest(schema 4, disk_restore,
                        │      bring_target_online=false, require_source_size)
                        │ recv Progress* → UI 进度；recv Result → 终态
                        ▼
             写 restore_result.v1.json → 成功/取消自动 wpeutil reboot；失败停留错误页
```

## 文件

```text
src/apps/pe_restore/
  src/main.cpp             # wWinMain
  src/pe_ui.h/.cpp         # 全屏 Win32 UI（IPeRunView 实现：倒计时/摘要/密码页/进度/结果）
  src/pe_run.h/.cpp        # 流程编排（UI 通过视图桥解耦）、结果持久化、wpeutil 重启
  src/pe_worker_client.h/.cpp # Worker Session 协议客户端（与 service 侧同为独立编解码副本）
  src/pe_strings.h/.cpp    # 内置五语言字符串表（en/zh-CN/zh-TW/ja/de，按 Job locale 选择）
```

## 关键不变量

1. Job 在任何破坏性动作前完整读入内存并通过 contracts 验证（读取即验证）。
2. 磁盘身份：序列号（trim 后精确）+ 容量双匹配，恰好 1 个命中才继续；
   永不按 disk number 写盘。
3. 密钥生命周期：sealed 信封解封成功后立即覆写删除 `.key`（写盘开始前）；明文密码
   仅存在于 `SecureZeroMemory` 守护的缓冲与 DPAPI 密文中，不落日志、不进管道明文。
4. 取消策略：倒计时/密码页可取消（写 result=cancelled + 焚毁 Key + 重启，未写盘）；
   Job 一经下发 Worker 即禁用取消（比设计 §9.4 更保守，PE6 可细化到按进度阶段放开）。
5. `bring_target_online=false`：PE 内不上线目标、不分配盘符；离线扩容被 Worker 跳过。
   WinPE SAN 策略 OfflineShared 会把本地唯一盘重新联机，因此 Worker 不以
   `DISK_ATTRIBUTE_OFFLINE` fail-closed；写盘前强制卸载目标卷并禁用 automount。
6. Result 语义：success / cancelled 自动重启；failed 停留错误页并保留稳定错误码
   （`pe_restore.job_not_found / job_invalid / envelope_failed / chain_unreachable /
   target_mismatch / worker_failed / cancelled`）。

## 依赖

链接：Base、Contracts、Ports、AdapterWindowsPe、AdapterCryptoSodium、AdapterWindowsDisk、
AdapterWindowsIpc、AdapterWindowsProcess、AdapterWindowsSystem、nlohmann-json（PRIVATE）。
禁止：Qt、sqlite、Dokan、windows_vss、pipeline/personal_archive（数据面在 Worker 进程）。

## WIM payload 闭包（PE4 接线）

显式必选清单（`PeRestoreJobService` 的 `kPayloadCandidates`），缺任一文件则 kind 51
Arm 以 `kNotFound` 失败，不进入 DISM。不做隐式发现，不注入 debug CRT / zlib：

- 执行器：`aegra_pe_restore.exe`、`aegra_personal_worker.exe`
- vcpkg：`libsodium.dll`、`zstd.dll`
- MSVC CRT：`vcruntime140.dll`、`vcruntime140_1.dll`、`msvcp140.dll`、
  `msvcp140_1.dll`、`msvcp140_2.dll`、`msvcp140_atomic_wait.dll`、
  `msvcp140_codecvt_ids.dll`、`concrt140.dll`

payload 由 `IPeImageBuilder` 请求显式携带。

## 在线编排：`PeRestorePrepareService`（PE4a，application 层）

[header](../../src/application/include/aegra/application/pe_restore_prepare_service.h) /
[impl](../../src/application/src/pe_restore_prepare_service.cpp)。设计 §4.1 状态机的落地：

- `prepare_and_arm`：单占用检查（已存在 Pending → kConflict）→ `IPeImageBuilder::ensure_ready`
  → 组装 `PePendingJobV1` → `IPeSecretSealer::seal`（绑定到已组装 Job 的 binding）→
  `IPePendingJobStore::write_pending` → `IOneTimeBootController::arm_once`；**Arm 失败即
  `clear_pending`**，绝不遗留半成品 hand-off。
- `cancel`：disarm + clear_pending（幂等）。
- `query_state`：is_armed + Pending 摘要，供 kind 20 与 Desktop 状态条。

架构纪律：只依赖 ports（PE0/1/2 的三个端口）+ 注入的 `IPeSecretSealer`，**不链接任何适配器**。
密封器由 composition root 的 `apps/service` 侧 `PeSecretSealer` 基于
`crypto_sodium::pe_envelope` 实现，使 application 层不引入 crypto 依赖。

## 协议接入（PE4b，apps/service）

kind 19/20/51/52 已全链路接线（contracts → codec → executor lane → `service_host` 分发 →
`PeRestoreJobService` → `PeRestorePrepareService`）；能力位 `restore.pe.prepare/arm/cancel`
由 `service_main` 在 windows_pe 适配器栈装配成功时声明。Arm 失败把具体原因放进
`message_arguments`（缺 payload 用 `pe_restore.payload_missing` + `file_name`），Desktop
展示该原因而不是笼统的 command failed。逐字段 wire 说明见
[协议 V4 §12](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)。阶段 C：Service 启动扫描
`restore_result.v1.json` → 审计事件（`pe_restore.succeeded/failed/cancelled`）→ 清理。
Desktop 客户端 codec 与 UI 接入属 PE5。

## 验证

ADR-0015：无自动化测试。PE3 + PE4a + PE4b 验证 = 全量生产构建 + 源码规模检查通过。
运行期人工验收（虚拟机）：

- [ ] UEFI GPT 系统盘 Full 链恢复后可启动；增量链正确
- [ ] sealed / prompt / none 三种信封模式；删 Key / 篡改绑定字段后拒绝且不写盘
- [ ] 盘号漂移场景序列号匹配正确；0/多命中拒绝
- [ ] 倒计时取消未写盘且回 Windows；DPAPI 在 WinPE 会话内 protect/unprotect 自洽
- [ ] 五语言 UI 文案渲染（WinRE 字体覆盖 CJK 的确认）
