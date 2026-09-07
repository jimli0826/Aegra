# Post Backup BootCheck 开发设计

## 状态

**Phase 2 in progress**。Dokan Adapter 已提供只读 VMDK presenter，VirtualBox Adapter 已实现隔离
capability probe 与 VM/medium 生命周期；Manifest schema 2 Boot Profile 及备份/增量链写读约束已实现；
启动成功判据为 Hyper-V 心跳（主）+ 差分盘持续增长（兜底）（见「Windows 启动成功判据」，COM1 Guest Probe 已于 2026-09-03 移除）；
独立单任务 `AegraBootCheck.exe`（`apps/boot_check`）已实现完整阶段
流水、`logs/bootcheck` 任务日志与 Worker 兼容响应/退出码（见 [boot_check_host.md](../modules/boot_check_host.md)）；
Service 侧 durable Post Backup plan、`PostBackupCoordinator`、`BootCheckSupervisor` 与孤儿清扫已落地；
Hyper-V 已作为第二 provider 接入（VHDX parent + PowerShell 生命周期）；
Desktop UI（Phase 3）已提供 BootCheck 开关、VirtualBox/Hyper-V 用户选择及安装状态展示；选择按
ADR-0029 快照到 durable plan，Host 不再自动回退 provider。
当前代码不得在 UI 中把 VirtualBox VM `running` 等同于 Windows 启动成功。

## 目标与非目标

BootCheck 在 `volume_set` 备份及其 Verify 成功后，把 Recovery Point 的系统盘以只读虚拟 VMDK/VHDX
呈现给用户选择的 VirtualBox 或 Hyper-V，并在隔离 VM 中确认备份内的 Windows 已经启动到 Aegra Service 可运行阶段。

BootCheck 验证的是：

- Archive 链可构成完整系统盘；
- MBR/GPT、启动分区和 Windows 卷能在受控的所选 hypervisor 硬件配置下启动；
- Windows Kernel、系统卷、SCM 和备份内的 Aegra Service 已运行。

BootCheck 不证明：

- Recovery Point 没有恶意软件或业务数据完全正确；
- 任意物理硬件、任意 Hypervisor 或原机 TPM/BitLocker 状态均可启动；
- 用户可以登录，或所有应用服务都健康；
- VM 进入 `running` 状态就代表 Windows 已启动。

## 用户体验与前置条件

Backup Options 的 `Post Backup` 区域增加：

```text
Post Backup
☑ Enable verify
☐ Enable boot check
```

规则：

1. `Enable boot check` 只对 `volume_set` 显示；`file_set` 不支持。
2. 只有 Source 包含一个完整、受支持的 Windows 系统盘时才可勾选。只选 C: 不满足条件。
3. BootCheck 与 Verify 是相互独立的选项（2026-09-03 起解除联动）。两者都启用时执行顺序固定为
   Backup → Verify → BootCheck，Verify 失败仍跳过 BootCheck；只启用 BootCheck 时在 Backup 成功后直接执行。
4. VirtualBox/Hyper-V 未安装时对应项禁用并显示“未安装”；两者均未安装时不能新启用 BootCheck。
   已安装不等于运行时可用，任务执行时仍由 provider `inspect` 复检。
5. Schedule 创建后 Source 仍不可变；`verify_after_backup` 和 `boot_check_after_backup` 可修改。

“完整系统盘”由 Service 权威判断，不能依赖 Desktop checkbox：

- 唯一 Windows 系统卷所在的物理盘已被选中；
- 该盘所有具备稳定 identity、可备份且非零容量的 Volume 都在 `source_ids` 中；
- GPT/UEFI 包含 EFI System Partition，MBR/BIOS 包含活动启动分区；
- Windows 启动卷不跨盘、不属于不支持的 dynamic/spanned 布局；
- 初版只支持 x64 Windows、基本 MBR/GPT 磁盘和 512-byte logical sector；
- 初版对受 TPM 封装的 BitLocker 系统卷返回不支持，不采集或导出恢复密钥。

Schedule 校验只能作为第一道门。Backup 完成后必须再次读取已认证 Manifest，确认实际写入 Recovery Point
的 Boot Profile 和 Volume extents 满足同一条件；不允许用当前机器 Inventory 替代 Archive 事实。

## 总体架构

```text
Backup Worker succeeds + Catalog committed
        |
        v
Durable PostBackupCoordinator
        |
        +--> Verify Job --failed--> BootCheck skipped(prerequisite)
        |
        +--> BootCheck Job
                 |
                 v
        AegraBootCheck.exe
          |-- open base-first Archive chain
          |-- WholeDiskByteReader(system disk)
          |-- expose read-only virtual VMDK through Dokan
          |-- create selected-provider differencing medium
          |-- create isolated headless VM
          |-- wait for differencing overlay growth (boot confirmation)
          `-- poweroff, unregister and clean artifacts
```

模块边界：

- `apps/service`：持久化后置动作计划、幂等调度、状态查询和 Host 监督；不解析 VMDK，不直接调用
  `VBoxManage`。
- `apps/boot_check`：单任务进程 Host，负责 deadline、取消、阶段日志、清理状态机和最终结果。
- `virtualization`：平台无关 `IBootCheckProvider` / VM 生命周期合同。
- `adapters/virtualbox`：VirtualBox 发现、版本/capability 探测、VM/medium 操作和 machine-readable
  输出解析。只用受信任绝对路径启动 `VBoxManage.exe`。
- `adapters/hyperv`：Hyper-V 安装/运行态探测、VHDX 差分盘、VM 与 PowerShell 隔离操作。
- `adapters/dokan`：把 `IRandomAccessReader` 整盘视图呈现为 VMDK descriptor + flat extent 虚拟文件。
- `adapters/personal_archive`：继续提供 Archive chain 和整盘随机读视图，不知道 VirtualBox。
- `adapters/sqlite`：保存 Post Backup plan/action 的运行状态，不保存 VMDK 或 VM 厂商对象。

VirtualBox/Hyper-V SDK/CLI、进程输出和厂商 SDK 枚举不得进入 Contracts、Pipeline、Format Manifest 或
SQLite 公共合同；Contracts 仅保留产品级 `BootCheckHypervisor` 用户选择枚举。

## 持久化后置动作链

BootCheck 不应继续依赖纯内存 completion callback。创建 Backup Job 时同时写入
`post_backup_plans`，至少包含：

- `backup_job_id`、`schedule_id`、`recovery_point_id/file_uuid`；
- `verify_required`、`boot_check_required`、选中的 `boot_check_hypervisor` 快照；
- 每个 action 的 `pending/running/succeeded/failed/skipped`、child job id 和稳定 message code；
- claim owner、lease expiry、attempt 和 timestamps。

Service 启动时扫描未完成 plan，按 lease 重新认领，确保 Backup 成功后 Service 崩溃或重启不会漏掉动作。
动作 Job 使用独立确定性幂等键：

```text
post-backup-verify:<backup_job_id>:<file_uuid>
post-backup-bootcheck:<backup_job_id>:<file_uuid>
```

Request fingerprint 仍可使用结构化分隔符，但 SQLite `idempotency_key` 只能使用稳定标识符字符集。
Verify 失败时 BootCheck action 记为 `skipped`，message code 为 `bootcheck.verify_prerequisite_failed`；
Backup Job 本身保持成功。

## Boot Profile

当前 Manifest 已包含 Disk、Partition、原始 MBR/GPT、Volume 和 extents，但没有足够明确的启动语义。
Backup Worker 应在加密 Manifest 中写入经过验证的 `BootProfile`：

- `system_disk_number`、`windows_volume_index`、必需 boot partition numbers；
- `firmware_mode = bios|uefi`、OS architecture/build；
- Secure Boot、TPM 和 BitLocker protection 状态（只存状态，不存 key）；
- logical sector size、磁盘布局 fingerprint；
- probe protocol version 和 Aegra Service version。

UEFI 由 Windows 启动环境和 ESP 共同确认，不能只用“GPT”推断；BIOS 必须确认活动分区和可用启动代码。
增量链各层 Boot Profile 的系统盘身份和布局 fingerprint 必须兼容；布局变化应使增量降为 Full。
本产品未发布，落地时直接更新当前 Manifest schema 和文档，不增加旧格式 fallback。

## 虚拟 VMDK 呈现

默认方案不是复制整个系统盘到一个几百 GiB 的实体文件，而是按需呈现：

```text
PersonalArchiveChainReader
  -> WholeDiskByteReader (offset 0 = original disk byte 0)
  -> VmdkFlatImage
       base.vmdk       : 小型 descriptor
       base-flat.vmdk  : Dokan 虚拟文件，读请求 1:1 映射到整盘 reader
  -> VirtualBox child.vmdk (真实临时 differencing medium)
```

约束：

- `base.vmdk` / `base-flat.vmdk` 对 VirtualBox 只读；Recovery Point 永不接收写请求。
- 原始 MBR/GPT 和 disk signature/GPT disk GUID 原样出现在整盘视图中。
- VirtualBox 只挂载 child medium，所有客体写入由 VirtualBox 写入 job 目录中的差分盘。
- 差分盘设硬配额；超过配额以 `bootcheck.overlay_full` 失败。
- VMDK descriptor 只引用同一私有 mount 目录内的 flat extent，禁止相对路径逃逸、设备路径和 UNC。
- 分卷 Archive 和 Incremental chain 继续由 Archive Reader 处理，VMDK Adapter 不感知 `.bkf` 分卷。

该方案避免宿主挂载带有原系统 disk signature 的虚拟物理盘，也避免每次备份后完整转换。Phase 0 必须用
VirtualBox 7.1/7.2 在 Windows 上验证 descriptor、只读 parent、差分盘和 Dokan random I/O 的兼容性。
若官方 provider 不接受虚拟 flat extent，才实现 `MaterializedVmdkPresenter`：直接写动态 VMDK，并在执行前
检查临时空间；不得把 RawDisk VMDK 指向宿主真实系统盘。

## VirtualBox VM 配置

每个 Job 使用独立 `VBOX_USER_HOME` 和 VM 名称 `Aegra-BootCheck-<job_id>`，不访问用户已有 VM。
VM 使用 headless 模式并采用最小设备集：

- 2–4 vCPU，2–4 GiB RAM（当前默认 4 vCPU / 4 GiB），并受全局 BootCheck resource budget 限制；
  默认最多并发 1 个；
- BIOS/UEFI 与 Boot Profile 一致；UEFI 创建指向 ESP Windows Boot Manager 的临时 NVRAM boot entry；
- SATA/AHCI 作为初版系统盘控制器；不支持的启动存储驱动在前置校验中明确拒绝；
- boot order 只允许 disk，不创建或挂载 challenge ISO/DVD；
- NIC、共享目录、clipboard、drag-and-drop、USB、audio、VRDE 全部关闭；
- 新 VM UUID，不复制原机硬件 UUID；虚拟盘内 disk identity 保持不变；
- VM 配置、VirtualBox 日志和差分盘只存在于该 Job 私有目录；最终截图在清理前写入受控的
  `<data_dir>/logs/bootcheck`，与对应任务日志同名并使用 `.png` 扩展名。

VirtualBox capability 只有在以下条件同时满足时声明：受支持版本、签名可信的绝对
`VBoxManage.exe`、Host driver 可用、headless 启动探测成功、Dokan/VMDK presenter 可用。移除或损坏
VirtualBox 后，已启用 Schedule 保留设置，但该次动作以 provider unavailable 结束并给出稳定提示。

## Windows 启动成功判据

VM `running`、出现登录画面或 hypervisor process 未退出都不能作为成功。判据按权威性分两级：

1. **主判据 · Hyper-V 心跳**：Host 每 10 秒查 `(Get-VM).Heartbeat`，返回 `Ok*` 即代表 guest OS
   真正运行（Integration Services 响应）。这是 Windows 自带集成服务，无需向镜像注入 agent，转圈
   阶段是 `NoContact`、崩溃/蓝屏也永远等不到 `Ok`，因此不会误判。
2. **兜底判据 · 差分盘持续增长**（心跳不可用：集成服务被禁用，或 VirtualBox 无 Guest Additions）：
   VM 只挂载以只读 parent 为基础的差分盘，guest 全部写入落在 overlay。Host 每 2 秒读 overlay 大小，
   仅当「超过 `boot_confirmed_overlay_bytes`（默认 60 MiB）+ 至少 90s 已过 + 近 20s 窗口内仍增长
   ≥8 MiB」三者同时满足才判定成功。早期引导写入量再大也会因「最短时长 + 持续增长」被挡住，挂死/
   蓝屏（写入停滞）也无法满足持续增长。
3. Host 每 10 秒查询一次 VM 状态：VM 关机或异常终止报 `bootcheck.guest_powered_off`；overlay 超过
   `overlay_limit_bytes` 报 `bootcheck.overlay_full`；到达 boot timeout 报
   `bootcheck.boot_not_confirmed`。`[Result]` 的 `boot_confirmed` 记录命中的判据
   （`guest_heartbeat` / `overlay_growth`）。

该判据不需要向镜像注入 agent、串口、网络或 Guest Additions（COM1 Guest Probe 已移除；Hyper-V 心跳
用的是 Windows 出厂自带的集成服务，非我们安装的组件）。BootCheck 是可启动性检查，不是远程安全证明。
初版 boot timeout 为 10 分钟。超时使用 `bootcheck.boot_not_confirmed`，UI 文案必须是“未确认启动”，
不能武断显示“镜像不可启动”。

## 阶段、日志与结果

每个已接受的 BootCheck Job 单独写：

```text
<data_dir>/logs/bootcheck/YYYYMMDD_HHMMSS[_job-id].log
```

阶段固定为：

```text
validate_prerequisites
resolve_credentials
open_archive_chain
validate_boot_profile
present_vmdk
create_vm
attach_media
start_vm
wait_boot_confirmation
capture_screenshot
cleanup
```

成功、失败、取消都写 `[Result]`。可记录 VirtualBox 版本、firmware mode、系统盘号、层数、耗时、VM state
和差分盘峰值；不得记录密码、SecretRef 或 VirtualBox 用户配置。CLI 输出只能按稳定字段
解析并有界、脱敏地写入日志。

建议稳定 message code：

- `bootcheck.completed`
- `bootcheck.source_not_system_disk`
- `bootcheck.incomplete_system_disk`
- `bootcheck.unsupported_boot_profile`
- `bootcheck.provider_unavailable`
- `bootcheck.archive_open_failed`
- `bootcheck.vmdk_present_failed`
- `bootcheck.vm_create_failed`
- `bootcheck.vm_start_failed`
- `bootcheck.guest_powered_off`
- `bootcheck.boot_not_confirmed`
- `bootcheck.overlay_full`
- `bootcheck.cancelled`
- `bootcheck.screenshot_failed`
- `bootcheck.cleanup_incomplete`

VM session 已创建时，成功、失败和取消均在 cleanup 前采集最终截图；VirtualBox 使用 provider 原生 PNG，
Hyper-V 使用 WMI thumbnail 转为 PNG。截图视为敏感用户数据，沿用任务日志目录的访问控制与保留策略；
协议响应和普通 Service 日志不返回截图内容或像素。截图使用独立 30 秒预算，失败只记录
`bootcheck.screenshot_failed`，不改写原始 BootCheck 结果。VM 创建前失败时没有可截图对象。

## 取消、清理与崩溃恢复

清理顺序在所有终态执行：

```text
stop/poweroff VM
-> detach child/base media
-> unregister VM (不使用可删除外部 medium 的宽泛操作)
-> stop Dokan VMDK presenter
-> close Archive readers and credentials
-> delete the exact validated job directory
```

Service stop 或 deadline 先请求协作取消，超时后终止 BootCheck Host。Host 和 Service 启动时都扫描带有效
Aegra marker 的孤儿 Job；只处理隔离 `VBOX_USER_HOME` 中名称、UUID、marker 三者一致的 VM，绝不按宽泛
名称或用户 VirtualBox 全局目录删除。清理失败不把已确认的 boot result 改为失败，但增加 warning、
`cleanup_incomplete` 和启动 scavenger 重试。

## 实施顺序

### Phase 0：受控可行性验证

- 用现有 `WholeDiskByteReader` 构造 descriptor + virtual flat extent；
- 在 VirtualBox 7.1/7.2 上验证只读 VMDK parent + differencing child；
- 人工覆盖 BIOS/MBR、UEFI/GPT、VM poweroff、timeout、取消和清理；
- 验证 NIC/共享/clipboard/USB 均关闭，Recovery Point 哈希前后不变。

Phase 0 不进入 UI，不持久化新 Schedule 字段。

#### 2026-08-29 Windows / VirtualBox 7.2.14 记录

已完成的基础链路验证：

- Debug 生产构建和源码规模检查通过；
- `ReadOnlyVmdkPresentation` 通过 Dokan 呈现 `monolithicFlat` descriptor + virtual flat extent；
- VirtualBox 成功识别 64 MiB 只读 parent，设置为 immutable，并创建 2 MiB VDI differencing child；
- child 通过 SATA/AHCI attach 到隔离 VM；VM 使用 2 vCPU、2 GiB、BIOS、disk-only boot，NIC、audio、
  clipboard、file transfer、drag-and-drop、USB、VRDE 和 recording 均关闭；
- headless VM 进入 `running`，VirtualBox 日志确认 parent descriptor、virtual flat extent 和 child VDI
  三个 endpoint 均成功打开；
- 测试源 SHA-256 在运行前后保持一致；VM poweroff、detach、unregister、child delete、parent close 和
  Dokan unmount 后，隔离 media registry 为空。

验证中首先发现只读 descriptor 缺少 `ddb.uuid.image` 时，VirtualBox 会把介质标记为 inaccessible：它无法
把随机 UUID 回写只读 descriptor，介质报告的全零 UUID 与 registry 不一致。生产接口因此要求调用方显式
提供非零 image/modification UUID，并将其写入 descriptor；修复后 parent 与 differencing child 链成功。

本次源是非启动空盘，日志中的 `VMBootFail` 是预期结果；它只证明 VM/介质生命周期链路可用，不构成
Windows 启动成功。Phase 0 尚需使用受控的 BIOS/MBR 和 UEFI/GPT Windows Recovery Point 验证真实启动、
Archive chain random I/O、timeout/cancel、差分盘配额和 guest probe。

#### 2026-08-29 VirtualBox Provider 生产适配器记录

- `IProcessLauncher` 支持 per-child environment override，两个隔离 VirtualBox home 不修改父进程环境；
- `IBootCheckProvider` / Session 合同及 `Aegra::AdapterVirtualBox` 生产 Target 已加入构建；
- capability 同时验证 Authenticode、7.1/7.2 版本、host info 和真实 headless probe；probe VM 无介质、无网络，
  清理后 capability registry 的 VM/HDD 列表为空；
- 生产 Provider 使用 Phase 0 的 Dokan presenter 创建 immutable parent、VDI child 和隔离正式 VM，headless
  状态达到 `running`；显式 cleanup 后 child 文件不存在、presenter inactive，Job registry 的 VM/HDD 列表为空；
- 人工验收发现 `controlvm poweroff` 返回早于 VirtualBox 完全释放 VM 锁；生产代码已增加 poweroff 状态轮询
  和 unregister/detach/medium close 的有界重试；
- VS 2026 Insiders Debug 全量构建及源码规模检查通过。人工介质仍是 64 MiB synthetic whole-disk reader，
  因此该记录只验证生产 Adapter 生命周期，不证明客体 Windows 成功启动。

### Phase 1：合同、Manifest 与 Adapter

- 增加 Boot Profile、`IBootCheckProvider`、VirtualBox Adapter 和 VMDK presenter；
- 增加 Guest Service 的有界 COM1 READY 发送路径和 Host serial reader；
- 构建受影响生产 Target，执行源码规模、架构和秘密扫描。

#### 2026-08-29 Boot Profile 记录

- current V7 Manifest 直接升级 schema 2，根固定携带 `boot_profile` map/null，不读取开发期 schema 1；
- Format 定义 BootProfile、严格引用/启动布局校验、canonical disk-layout SHA-256 preimage 与增量兼容判断；
- Windows Disk Adapter 只读采集 Windows volume、firmware、x64 build、Secure Boot、TPM 与 BitLocker 状态，
  BitLocker 使用 WMI 且不读取 recovery key；
- Worker 仅在完整系统盘、ESP/活动分区、完整 raw layout 和 512-byte sector 均满足时写 Profile；普通数据盘
  或不完整系统盘仍成功，但 Profile 为 null；
- Personal Archive write/read chain 对相邻层 Profile 身份和布局 fingerprint fail-closed；
- VS 2026 Insiders Debug 全量生产构建与源码规模检查通过。真实系统盘 Archive Profile/null 分支及
  BitLocker on/off 人工矩阵仍待隔离环境验收。

#### 2026-08-29 Guest Probe Protocol 记录

- Contracts 定义 protocol 1 与固定 26-byte ASCII `AEGRA_BOOTCHECK_READY_V1\r\n`；
- Virtualization matcher 只接受完整逐字节相同的 READY 标识；VM running 不进入该合同；
- Format Boot Profile 与 READY 合同共用 Contracts protocol version 常量；
- V1 明确不使用 Challenge ISO、job/nonce、Machine GUID digest 或防重放逻辑；
- `IBootCheckVmSession::wait_for_boot_probe` 已增加；VirtualBox Adapter 在 VM 启动前创建拒绝远程客户端的
  per-job byte-mode Named Pipe，VM 使用 COM1 `0x3F8/IRQ4`、16550A 和 pipe client 模式；Host 以 overlapped
  connect/read 支持分段累计及 `CancelIoEx` 取消；
- VirtualBox 7.2.14 隔离临时 VM 已确认 UART 配置可接受且清理完成；
- `Aegra::AdapterWindowsBootCheck` 已实现 live VirtualBox SMBIOS gate、115200/8N1 无 flow control、1 秒
  write timeout 与固定 READY 写入；Service 仅在 SCM runtime/listener 成功后调用，未发送不改变 Service
  启动结果，成功只记录稳定 message code；
- 当前非 VirtualBox 开发机可覆盖安全跳过路径；真实 Windows Recovery Point 的 Guest→Host 端到端信号仍
  待独立 BootCheck Host 接入后人工验收。

### Phase 2：持久化编排与 Host

- 增加 durable Post Backup plan/action、BootCheck Job 和 Supervisor；
- 支持 Service 重启重放、幂等、deadline、取消、资源上限和孤儿清理；
- 完成独立 `logs/bootcheck`。

#### 2026-08-30 Hyper-V Provider 记录

- `virtualization` 合同泛化：`IBootCheckProvider` 新增 `parent_disk_format()`（`kVmdk`/`kVhdx`），
  `BootCheckVmRequest.parent_vmdk_path` 更名为格式无关的 `parent_disk_path`；Host 新增
  `select_provider` 阶段（只检查 Schedule 快照指定的 provider，不自动回退），
  `present_vmdk` 阶段更名 `present_disk` 并按选中 provider 的格式呈现。
- Dokan 新增 `ReadOnlyVhdxPresentation`：复用挂载功能已验证的 `VhdxDiskImage` 固定容器编码，经
  Dokan 呈现单个只读 `<job>/present/disk.vhdx` 供 Hyper-V 差分链引用；语义与 VMDK 版一致。
- 新增 `adapters/hyperv`（`Aegra::AdapterHyperV`）：全部操作经 System32 `powershell.exe`
  `-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand`（UTF-16LE base64，前置
  `$ErrorActionPreference='Stop'`，变量单引号转义注入）；可用性 = vmms Running + `Get-VM` 可解析
  （不建探测 VM）。VM 形状与 VBox 版对齐：Boot Profile 决定 Gen2/UEFI（关 Secure Boot、显式
  FirstBootDevice）或 Gen1/BIOS，移除网卡、固定内存、禁 checkpoint、`AutomaticStopAction
  TurnOff`；`New-VHD -Differencing` 生成 `<job>/child.vhdx`（配额监视对象）。
- READY pipe 方向相反：`Set-VMComPort` 由 Hyper-V 以 server 身份创建
  `\\.\pipe\aegra-bootcheck-<job_id>`，Adapter 以 client 连接（`ERROR_FILE_NOT_FOUND`/PIPE_BUSY
  每 200ms 取消感知重试）；READY 字节、滑动窗口扫描器、4 MiB 上限不变（ADR-0028 已更新）。
- 孤儿清扫：Hyper-V 无 per-job 隔离注册表，创建时把 `aegra-bootcheck:<job_id>` 写入 VM Notes，
  `--scavenge` 在全局清单以「`Aegra-BootCheck-` 名称前缀 + Notes 标记」双重门控识别并
  Stop-VM/Remove-VM；vmms 未运行或模块缺失视为 0 台成功（本机实测：模块存在但 vmms 停止时
  `Get-VM` 会抛错，脚本已显式门控）。
- Guest 发送端 SMBIOS 门禁扩展：`SystemProductName` 为 `VirtualBox` 或 `Virtual Machine`
  （Hyper-V）时才写 COM1；结果枚举 `kNotVirtualBox` 更名 `kNotVirtualMachine`
  （日志 status `not_virtual_machine`）。
- 冒烟：Debug/Release 构建通过；`--scavenge` 在 vmms 停止的本机上 `hyperv: vms_removed=0`、
  outcome clean。真机 Hyper-V 端到端（启用 hypervisorlaunchtype 的宿主）待实测——注意与
  VirtualBox 互斥：宿主开 Hyper-V 会显著拖慢 VBox guest（见 2026-08-29 记录）。

#### 2026-08-30 durable Post Backup plan 记录

- 控制面 schema v24 新增 `post_backup_plans`：Backup 提交时与 JobRecord **同事务**写入
  （`FK backup_job_id → jobs ON DELETE CASCADE`，随 Job 保留期清除）；字段与本计划
  「持久化后置动作链」一致（verify/boot_check 双动作 state/job_id/message_code/attempts +
  claim_owner/lease）。旧开发库需删除重建（无迁移，按仓库政策）。
- `PostBackupCoordinator`（apps/service）取代内存版 `PostBackupVerifier`：15s claim/lease 轮询 +
  completion 回调即时 kick；Backup 成功且 Catalog 可见后按确定性幂等键
  `post-backup-verify:<backup_job_id>:<file_uuid>` 提交 Verify，重启后重扫未完成 plan 重放同一键；
  容量暂满/Catalog 未可见保持 pending（有界 attempts），Backup 非成功终态动作记 skipped，
  Verify 失败时 boot_check 预置为 skipped（`bootcheck.verify_prerequisite_failed`）。
- Verify 请求改为从 Catalog 重建（`prepare_verify` 增加可选 Schedule 密文引用）：加密备份用
  Schedule 的 dpapi-lm 引用，不再依赖内存快照，也不需要 connection 默认凭据；旧快照式
  `prepare_post_backup_verify` 路径删除。
- 冒烟验证：Service 交互模式启动 → schema 24 建库、`post_backup_plans` 表结构正确、
  `post_backup.coordinator_started` / `runtime_ready` 正常；Debug/Release 生产构建通过。
  真机端到端（备份→plan→verify 自动完成→重启重放）待下一轮 Schedule 实测。
- 同日追加：Schedule 新增 `boot_check_after_backup`（v24；仅 volume_set，强制随
  `verify_after_backup`；暂未进入 V4 协议，Desktop 更新保留既有值）；Backup 提交把双动作策略
  快照写入 plan（file_set 自动降为不需要 boot check）。`BootCheckSupervisor`（apps/service）
  以兄弟路径拉起 `AegraBootCheck.exe --request <staging file>`（Host 新增该模式），从
  Catalog 解析 base-first 卷链、按 Schedule 密文引用注入凭据，45 分钟 run budget 超时
  terminate，stdout WorkerResponse 回填 plan；coordinator 在 Verify 成功后驱动 boot_check
  动作（并发 1，attempt 上限 3，重启丢失的运行以新 attempt id 重派，Host 缺失记
  `post_backup.boot_check_unavailable`）。冒烟：schema 24 + `boot_check_after_backup` 列 +
  Service 启动/收口正常。
- 同日追加（scavenger + V4 协议）：`adapters/virtualbox` 新增 `scavenge_boot_check_home`
  （只处理单个隔离 VBOX_USER_HOME 内 `Aegra-BootCheck-*` 前缀 VM：poweroff + unregister，绝不
  触碰用户全局注册表）；Host 新增 `--scavenge` 模式清理孤儿 job 目录与 staging 残留；
  `BootCheckSupervisor` 启动时后台执行一次 scavenge，完成前阻塞新派发（消除清理与新 job 目录
  的竞态）。V4 `UpsertSchedule` 增加可空 `boot_check_after_backup`（null = 更新保留既有值 /
  创建 false；契约校验 volume_set-only + 强制 Verify），进入幂等指纹；`ScheduleSummary`
  返回该字段；Desktop codec 同步（UI 未落地前发送 null），Qt Desktop 构建通过。
- Desktop UI 已增加 volume_set 专用 **Enable boot check**，启用时自动启用并锁定 Verify，
  Schedule 创建、编辑和 enabled 切换均保留 `boot_check_after_backup`，并完成五语言翻译；待办为
  更细粒度的 eligibility 提示。备份后
  Manifest Boot Profile 的权威 eligibility 复核目前由 Host 的 `validate_boot_profile` 阶段承担
  （不合格记 `bootcheck.source_not_system_disk` / `unsupported_boot_profile`）。真机端到端
  （Schedule 双动作自动串联 + 重启重放 + 孤儿清扫）待实测。

#### 2026-08-29 独立 BootCheck Host 记录

- `apps/boot_check` 新增单任务 `AegraBootCheck.exe`：stdin 单次 `BootCheckJobRequest`
  （contracts schema 3，必填 hypervisor、CPU 数与内存；字段名含 password/secret 的明文凭据拒绝），stdout 复用 Worker
  `WorkerResponse`/`TaskResult` wire shape 与退出码表，Service 后续可复用现有解码器；
- 阶段流水 `validate_prerequisites → resolve_credentials → open_archive_chain →
  validate_boot_profile → present_vmdk → create_vm(含 attach) → start_vm → wait_boot_probe →
  cleanup` 全部落地；`wait_boot_probe` 合并外部取消、10 分钟 boot timeout 与每 2 秒 `child.vdi`
  配额监视（超限 `bootcheck.overlay_full`，`kInsufficientSpace`）；串口断开时按 VM 状态区分
  `guest_powered_off` 与 `boot_not_confirmed`；
- Boot Profile 门禁：非 volume_set / 无 Profile → `source_not_system_disk`；非 x64、非 512B
  扇区、probe 版本不符、BitLocker enabled → `unsupported_boot_profile`；
- cleanup 有 2 分钟总预算：session（poweroff/detach/unregister/closemedium）→ Dokan 关闭 →
  reader 释放 → 删除 Host 在 validate 阶段验证为空并取得所有权的 `job_directory`；清理失败不
  改写成功判定，转 `SucceededWithWarning` + `cleanup_incomplete`；
- `WorkerTaskLog` 提取为共享 `Aegra::AppWorkerTaskLog` 目标（公共头
  `aegra/apps/worker/worker_task_log.h`），Worker 与 BootCheck Host 共用同一 `logs/<op>` 版式；
- `adapters/virtualbox` 新增 `discover_vbox_manage_path()`（注册表 InstallDir → Program Files），
  签名与版本校验仍在 provider inspect；capability home 为 `<data_dir>\bootcheck\capability`；
- Debug/Release 生产构建通过；人工验证：畸形请求 exit 20、明文凭据字段 exit 20、Archive 缺失
  exit 10 + `bootcheck.archive_open_failed` + `logs/bootcheck` 任务日志生成 + job 目录清理。
  真实 Windows Recovery Point 端到端启动矩阵与 Service durable plan/Supervisor 仍待后续。

#### 2026-08-29 真实 Recovery Point 端到端验收记录

用真实 UEFI/GPT Windows 系统盘 Recovery Point（schema 2、40 GiB 盘、单层 Full、未加密）在
VirtualBox 7.2.14 上多轮运行独立 Host，结论：

- **可启动性链路真机全通**：archive chain → Boot Profile 门禁 → Dokan VMDK → 隔离 VM →
  备份内 Windows 启动到锁屏/登录界面（Windows Hello PIN 在 VM 内不可用属预期，非判据）；
- **发现并修复 UEFI 串口噪声**：EDK2 固件在 guest Service 运行前向 COM1 写引导输出，原
  "严格匹配最先 26 字节"必然 `probe_invalid`；已改为 `BootProbeSignalScanner` 滑动窗口匹配
  （见 ADR-0028 更新）；
- **发现并修复整盘读性能**：`WholeDiskByteReader` 原单槽 chunk 缓存在 guest 交替随机读下
  彻底抖动（每次未命中重解压 64 MiB Chunk，Host ~200% CPU、启动 >10 分钟超时）；改为有界
  LRU（shared_ptr payload，BootCheck 16 槽 / 挂载默认 8 槽）后启动到锁屏约 2 分钟，同时修复
  了原实现锁外复制缓存载荷的并发竞态；宿主 Hyper-V（`HypervisorPresent`）会显著拖慢 guest，
  验收环境建议关闭；
- **超时/提前关机路径已真机覆盖**：10 分钟 boot timeout → `boot_not_confirmed`；外部
  poweroff → pipe 断开 → Host 自行 cleanup 干净退出（本轮竞态下报 `boot_not_confirmed`
  而非 `guest_powered_off`，状态查询与 poweroff 完成存在竞窗，可接受）；
- **跨 Hypervisor VMDK 交叉验证（2026-08-30）**：Host 新增 `--present-only [minutes]` 诊断模式
  （只呈现只读 VMDK 并保持挂载 N 分钟后清理，不建 VM；响应/退出码不变）。用该模式把同一三层链
  呈现的 `base.vmdk` 挂到 VMware Workstation（`disk.locking="FALSE"` + independent-nonpersistent
  + UEFI 固件），guest Windows 成功启动——证明 descriptor/flat extent 呈现符合 VMDK 规范，
  不是仅 VirtualBox 可读的方言。
- **存储控制器负向验证（2026-08-30 00:11）**：临时把 VM 控制器换成 VirtualBox `LsiLogic`
  并行 SCSI 后，同一镜像在 guest 内以 `INACCESSIBLE_BOOT_DEVICE` 蓝屏（Win10+ 无 symmpi
  boot-start 驱动），BootCheck 正确报 `boot_not_confirmed`——证明 SATA/AHCI 基线选择正确、
  且 BootCheck 能捕获不可启动的硬件配置。注：VMware 的"SCSI"实为 LSI SAS（Windows 内置
  `lsi_sas.sys`），与并行 SCSI 不同；VirtualBox 对应为 `--add=sas LsiLogicSas`，可作 V2 按
  Boot Profile 选择控制器的扩展。实验代码已还原为 SATA。
- **端到端成功闭环（2026-08-30 00:06）**：源系统安装 SCM 模式 AegraService 后，三层链
  （Full + 2 × Incremental）合成 tip 的 UEFI 系统盘在隔离 VM 中完整启动，guest Service 经
  COM1 发出 READY，Host 滑动窗口精确匹配（串口共 113 B = 87 B UEFI 固件噪声 + 26 B 信号），
  `bootcheck.completed`、exit 0、清理干净，全程 430 秒。诊断结论：guest 无信号时
  `serial_bytes_scanned` 恒为 87 B（仅固件噪声），该计数已作为 wait 阶段固定日志项；
  Service 侧 `bootcheck.guest_ready_signal` 现记录每种发送结果
  （sent/not_virtualbox/com1_unavailable/failed），消除静默盲区。

#### 2026-09-03 启动判据改为差分盘增长（移除 COM1 Guest Probe）

- 判据从「guest Service 经 COM1 发送固定 READY 标识」改为「差分盘 overlay 大小超过
  `boot_confirmed_overlay_bytes`（默认 60 MiB）」；Host 每 2 秒轮询 overlay 大小、每 10 秒查询
  VM 状态，超时/关机/配额语义不变（`boot_not_confirmed`/`guest_powered_off`/`overlay_full`）。
- 新判据不再依赖备份内安装的 Aegra Service，任何能启动到持续写盘阶段的 Windows 都能确认；代价是
  语义从「SCM + Aegra Service 已运行」放宽为「OS 已进入持续写盘阶段」。
- 移除：`IBootCheckVmSession::wait_for_boot_probe`/`probe_bytes_scanned`、两个 provider 的
  probe pipe 与串口配置（Hyper-V `Set-VMComPort`、VirtualBox `--uart*`）、
  `virtualization/boot_probe_protocol`（virtualization 转为 INTERFACE 库）、guest 侧
  `adapters/windows_boot_check` 与 Service 启动时的 `bootcheck.guest_ready_signal`、
  `bootcheck.probe_invalid` message code、Boot Profile 准入中对 `probe_protocol_version` 的检查
  （Manifest V7 字段与 Contracts 常量保留，仅作历史记录）。阶段 `wait_boot_probe` 更名
  `wait_boot_confirmation`。

#### 2026-09-04 启动判据增加 Hyper-V 心跳为主判据

- 现象：纯 overlay 阈值（60 MiB）在 Windows 引导转圈阶段（winload/内核初始化写注册表 hive、
  驱动、页面文件）就被跨过，误判成功——VM 还在转圈时 `bootcheck.completed`。单纯调大阈值治标不治本
  （不同镜像早期写入量差别大），且无法区分「慢启动」与「崩溃/蓝屏」。
- 新增 `IBootCheckVmSession::guest_heartbeat_ok`：Hyper-V 查 `(Get-VM).Heartbeat`，`Ok*` 即
  guest OS 已运行（Windows 出厂 Integration Services，无需注入 agent）；VirtualBox 恒返回 false。
  `wait_boot_confirmation` 以心跳为主判据，overlay 降级为兜底且加「≥90s 已过 + 近 20s 窗口内增长
  ≥8 MiB」约束，早期引导与写入停滞（挂死/蓝屏）都不再误判。`[Result].boot_confirmed` 记录命中判据。

### Phase 3：Desktop 与发布门禁

- 增加选项、eligibility reason、Job/Recovery Point 状态和五语言翻译；
- 在受控 Windows VM 上人工覆盖 Full/Incremental、Verify 失败、VirtualBox 缺失、启动成功、启动超时、
  overlay 满和 Service 重启；
- 不增加 CTest、测试可执行文件或 fixture；独立验证脚本按 ADR-0031 管理；构建生产 Target、运行静态/
  架构检查并执行聚焦人工验收。

## Definition of Done

- Backup 成功、Verify 成功后，完整系统盘能自动进入一个独立 BootCheck Job；
- 只有 Hyper-V 心跳 `Ok*`，或（心跳不可用时）差分盘持续增长满足阈值+最短时长+持续增长，才能形成 `bootcheck.completed`；
- Service 任意重启不丢动作、不重复创建有效 VM；
- `.bkf` 全程只读，所有客体写入只落入有配额的临时差分盘；
- VM 无网络和宿主集成功能，VirtualBox 状态与用户已有 VM 隔离；
- 成功、失败、取消和崩溃路径都完成有界清理并留下独立 BootCheck 日志；
- 不支持的 Source、BitLocker/TPM、4Kn、跨盘启动布局在启动 VirtualBox 前被明确拒绝；
- 生产构建、源码规模、架构检查和秘密扫描通过，人工矩阵记录可复核。
