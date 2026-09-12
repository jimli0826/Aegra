# BootCheck Host 与进程协议

## 目标与边界

`apps/boot_check` 的 `AegraBootCheck.exe` 是单任务进程 Host：接收一个 BootCheck Job，把
volume_set Recovery Point 的系统盘经 Archive 链 + `WholeDiskByteReader` + Dokan 只读 VMDK/VHDX 呈现给
用户指定的隔离 VirtualBox 或 Hyper-V VM，以 Hyper-V 心跳（主）或差分盘 overlay 持续增长（兜底）
作为启动成功判据（详见「结果判据」）。Host 负责
deadline、取消、阶段日志、清理状态机和最终结果；不负责调度、持久化编排或控制面数据库访问
（Phase 2 的 durable plan 与 Service Supervisor 后续接入）。

BootCheck 是独立进程而不是 Worker 操作：不占用 Worker 数据面槽位；Dokan/VBoxManage 挂起时可被
整进程终止收口；VirtualBox 厂商依赖不进入 `AegraWorker.exe`。

## 请求与响应

- 请求：stdin 单次 UTF-8 JSON，上限 1 MiB，契约为 `contracts::BootCheckJobRequest`（schema 3）：
  `job_id`（小写字母数字与 `-`，≤64，成为 VM 名/pipe 名）、`trace_id`、必填 `hypervisor`
  （VirtualBox=1、Hyper-V=2）、由 Service 可信设置投影的 `cpu_count` / `memory_mib`、base-first `source_refs`、
  逐层 `credential_refs`（空 = 未加密层）、`job_directory`（本 Job 私有空目录）、可选
  `deadline_utc_ms`。字段名含 `password`/`secret` 的明文凭据一律拒绝。VM 形状（CPU/内存/差分盘
  配额/boot timeout）与 `VBoxManage.exe`/`powershell.exe` 路径来自受信任 Host 配置，不从消息接收。
- 响应：stdout 单条 JSON，复用 Worker 的 `WorkerResponse` wire shape（schema 1）与
  `TaskResult`（schema 4），`message_code` 为 `bootcheck.*` 稳定码；Service 端可直接复用
  Worker 响应解码器。
- 退出码与 Worker 相同：`0` 成功、`10` 任务失败、`11` 取消、`20` 请求拒绝、`21` Host 故障。
- Supervisor 模式 `--request <file>`：Service 无法向子进程 stdin 注入数据，请求 JSON 由
  Service 暂存到 `<data_dir>/bootcheck/staging/<job>.request.json`（仅含 SecretRef 密文引用，
  完成后删除），其余行为与 stdin 相同。
- 清扫模式 `--scavenge`：关机并注销 `<data_dir>/bootcheck/jobs/*/vbox-home` 隔离注册表中的
  `Aegra-BootCheck-*` VM（绝不触碰用户全局 VirtualBox 目录）；随后对 Hyper-V 全局清单做一次
  双重门控清扫（VM 名以 `Aegra-BootCheck-` 开头 **且** Notes 含创建时写入的
  `aegra-bootcheck:<job_id>` 标记；vmms 未运行或模块缺失视为 0 台成功）；再删除孤儿 job 目录
  与过期 staging 请求文件；Service 的 `BootCheckSupervisor` 启动时后台执行一次，完成前不派发
  新 BootCheck。
- 诊断模式 `--present-only [minutes]`（默认 60，上限 1440）：同一请求跑到 `present_disk` 为止，
  保持只读 parent 挂载 N 分钟（供外部 hypervisor/工具加载，如 VMware 需
  `disk.locking="FALSE"` + independent-nonpersistent），到期或取消后照常 cleanup；成功
  message code 为 `bootcheck.present_hold_completed`，不创建 VM。
- 能力探测模式 `--inspect <virtualbox|hyperv>`：运行对应 provider 的 `inspect()`（VirtualBox 含
  签名校验、7.1/7.2 版本门槛与 headless 探测 VM；Hyper-V 检查 vmms + 模块），stdout 输出单条
  JSON `{schema_version, kind:"inspect", hypervisor, available, message_code, provider_version,
  diagnostic}`；不可用仍以退出码 `0` 返回（`available=false` + 稳定 `bootcheck.*` 码），仅参数
  非法返回 `20`。Service 的 `BootCheckSupervisor` 在启动时和收到
  `RefreshBootCheckHypervisorStatus`（kind 53）时后台运行本模式并缓存结果，经 kind 21 查询
  提供给 Desktop 备份向导。

## 阶段与日志

任务日志写入 `<data_dir>/logs/bootcheck/YYYYMMDD_HHMMSS[_job-id].log`，复用 Worker 的
`WorkerTaskLog`（`Aegra::AppWorkerTaskLog` 目标）章节/stage 版式。阶段固定为：

```text
validate_prerequisites  # job_directory 必须为绝对路径且为空（Host 由此获得删除所有权）
resolve_credentials     # 空 SecretRef = 未加密层
open_archive_chain      # PersonalArchiveChainReader（相邻层 Boot Profile fail-closed）
validate_boot_profile   # 必须 volume_set + Boot Profile；x64、512B 扇区、非 BitLocker
select_provider         # 只探测请求指定的 provider；不可用即失败，禁止自动回退
present_disk            # VMDK: <job>/present/base.vmdk + base-flat.vmdk（随机非零 CID/UUID）
                        # VHDX: <job>/present/disk.vhdx（固定容器，供 Hyper-V 差分链）
create_vm               # 选中 provider 创建：VBox createvm/VDI 差分/attach；Hyper-V New-VHD
                        # -Differencing/New-VM（Notes 写入 aegra-bootcheck 标记）
start_vm
wait_boot_confirmation  # 每 10 秒查 Hyper-V 心跳/VM 状态、每 2 秒查 overlay；boot timeout（默认 10 分钟）+ 配额 + 外部取消
settle_after_boot       # 启动确认后再让 guest 运行 boot_settle_ms（默认 90 秒），期间仍监控掉电/配额/取消；
                        # 心跳在 Windows 转圈阶段即已就绪，不等待则截图几乎不会到登录界面
capture_screenshot      # VM 已创建时，无论启动确认成功/失败/取消均在 cleanup 前保存最终画面
cleanup                 # 先记录 guest 读路径计数（guest_read_calls/guest_bytes_read、chain_* 解码与缓存、layer_* 阶段）-> session cleanup -> Dokan 关闭 -> reader 释放 -> 删除 job_directory
```

日志不记录密码或 SecretRef；凭据只记 `present|empty` 层计数。

最终画面与对应任务日志使用相同文件名，保存为 `<data_dir>/logs/bootcheck/*.png`；因此不会随
`job_directory` 清理而删除。VirtualBox 使用 `VBoxManage controlvm ... screenshotpng`，Hyper-V 使用
`Msvm_VirtualSystemManagementService.GetVirtualSystemThumbnailImage`，`TargetSystem` 使用
`Msvm_ComputerSystem` 引用（WQL 过滤值必须自带引号 `ElementName='...'`），获取 640×480 RGB565 画面并编码为 PNG；
`ImageData` 可能比 width×height×2 多出少量尾部字节（实测 +4），只复制前 614400 字节，长度不足才报错。截图阶段有 30 秒独立预算且不复用
已经取消的任务 token；截图失败只记录
`bootcheck.screenshot_failed`（可附带有界 PowerShell 诊断），不改变原始 BootCheck 结论。VM 尚未创建的前置失败没有可截图对象。

## 结果判据与 message code

- 成功判据按权威性排序：① **主判据**——Hyper-V Integration Services 心跳（`(Get-VM).Heartbeat`
  返回 `Ok*`）确认 guest OS 已真正运行；该信号是 Windows 自带集成服务，无需向镜像注入任何 agent，
  且不会被早期引导转圈或随后的崩溃/蓝屏欺骗。② **兜底判据**（心跳不可用时：集成服务被禁用，或
  VirtualBox 无 Guest Additions）——差分盘 overlay 增长，且必须同时满足「超过
  `boot_confirmed_overlay_bytes` 阈值 + 至少 90s 已过 + 近 20s 窗口内仍持续增长（≥8 MiB）」，
  因此固件/boot loader 的早期写入和挂死/蓝屏（写入会停滞）都不会误判。任一判据满足即
  `bootcheck.completed`；清理失败不改写成功结果，转为 `SucceededWithWarning` + warning
  `bootcheck.cleanup_incomplete`。`[Result]` 的 `boot_confirmed` 记录实际命中的判据
  （`guest_heartbeat` 或 `overlay_growth`）。
- `bootcheck.source_not_system_disk`（无 Boot Profile / 非 volume_set）、
  `bootcheck.unsupported_boot_profile`（非 x64、非 512B 扇区、BitLocker enabled）、
  `bootcheck.provider_unavailable`、`bootcheck.archive_missing`（文件不存在）、
  `bootcheck.archive_credential_unavailable`（密码不可用）、`bootcheck.archive_corrupt`（认证失败）、
  `bootcheck.archive_open_failed`（其它打开失败）、`bootcheck.vmdk_present_failed`、
  `bootcheck.vm_create_failed`、`bootcheck.vm_start_failed`、`bootcheck.boot_not_confirmed`（boot
  timeout 内 overlay 未达阈值）、`bootcheck.guest_powered_off`（等待期间 VM 已 poweroff/aborted）、
  `bootcheck.overlay_full`（差分盘超配额，`kInsufficientSpace`）、`bootcheck.cancelled`、
  `bootcheck.cleanup_incomplete`。

## 取消、配额与清理

请求 deadline 与进程级取消合并（与 Worker Host 相同的 watchdog 模型）；`wait_boot_confirmation`
阶段每 10 秒查询一次 Hyper-V 心跳（`Ok*` 即成功）与 VM 状态（poweroff/aborted 按 guest_powered_off
收口），每 2 秒轮询一次 `child.vdi`/`child.vhdx` 大小（超过 `overlay_limit_bytes`，默认 8 GiB，按
overlay_full 收口；心跳不可用时的兜底成功另需阈值+最短时长+持续增长）。清理在所有终态执行且有 2 分钟
总预算：VBox 为 poweroff → detach → unregister → closemedium（child --delete、parent close）；
Hyper-V 为 Stop-VM -TurnOff → Remove-VM → 删除 child.vhdx；随后 Dokan 关闭 →
reader/credential 释放 → 删除验证过的 `job_directory`。孤儿 VM/目录扫描属于 Phase 2 Service
Supervisor 的职责。

## Composition 配置

- `VBoxManage.exe` 由 `adapters/virtualbox` 的 `discover_vbox_manage_path()` 解析（注册表
  `HKLM\SOFTWARE\Oracle\VirtualBox\InstallDir`，回退 `%ProgramFiles%\Oracle\VirtualBox`）；
  签名与 7.1/7.2 版本校验仍在 provider `inspect` 内执行。
- capability 探测使用 `<data_dir>\bootcheck\capability` 隔离 `VBOX_USER_HOME`。
- 数据目录顺序与任务日志一致：`AEGRA_DATA_DIR` → `%LOCALAPPDATA%\Aegra` → `%ProgramData%\Aegra`。
  Service 以交互用户 token 拉起 VirtualBox BootCheck 时，`CreateEnvironmentBlock` 不继承
  Service 环境，因此 Supervisor 必须把 `AEGRA_DATA_DIR` 作为 environment override 注入，并把
  该用户的可继承写权限授给 `<data_dir>/bootcheck` 与 `<data_dir>/logs/bootcheck`，保证任务日志
  和最终截图与 Service 同树（SCM 下即 `%ProgramData%\Aegra\logs\bootcheck`）。
- 构建产物复制到 `AegraService.exe` 同目录，供后续 Service Supervisor 以兄弟路径解析。

## 验证与完成标准

- Debug/Release 生产构建、源码规模与格式检查通过；
- 人工覆盖：畸形请求拒绝（exit 20）、明文凭据字段拒绝（exit 20）、Archive 缺失任务失败
  （exit 10 + `bootcheck.archive_open_failed` + job_directory 清理 + 独立任务日志）；
- 真实 Windows Recovery Point 的启动成功 / 超时 / overlay 满 / 取消矩阵仍待隔离 VirtualBox
  环境人工验收（见开发计划 Phase 2 记录）。
