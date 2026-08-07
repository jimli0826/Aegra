# `apps` 模块开发文档

## 目标

定义进程入口、Composition Root、配置加载、协议 Host、权限和生命周期。业务规则留在 Application/Pipeline，基础设施留在 Adapter。

## 进程

- `service`：控制面 API、鉴权和任务；个人版组合 SQLite，企业版组合 PostgreSQL。
- `worker`：每任务执行 Backup/Restore Pipeline。
- `repository_gateway`：企业 Repository 在线入口。
- `vmware_connector`、`hyperv_connector`：厂商 SDK 隔离。
- `mount_host`：Recovery Point 随机读取、Dokan 和虚拟磁盘呈现。
- `pe_restore`：WinPE 最小离线恢复程序。
- `desktop`：普通用户 GUI。
- `shell_extension`：轻量 IPC 桥接。

## Composition Root 规则

- 入口创建具体 Adapter 并注入 Use Case。
- 配置和 Secret 在入口验证后转换为强类型选项。
- 捕获所有未处理异常并转换为进程退出码、任务结果和脱敏日志。
- 处理停止信号、取消、线程 join 和临时资源清理。
- 入口不实现块处理、SQL 查询或格式解析算法。

Windows 个人卷备份的首个 Worker Composition Root 见
[Windows 个人卷备份 Composition Root](windows_personal_backup.md)。它只负责具体 Adapter 装配、Manifest
输入映射和 Snapshot/Source/Archive 生命周期顺序；块处理仍由通用 Pipeline 执行。

个人版 Worker 任务入口接收版本化 `JobRequest` 和受信任运行参数，通过 `ICredentialResolver`、
`IRandomSource`、`IClock` 与 `IProgressSink` 注入运行期能力。Job 只携带 SecretRef；解析出的 Secret
只覆盖同步备份调用周期。输入拒绝与已接受任务的失败必须区分，TaskResult 只能包含稳定、脱敏的代码。

Worker Host 与 JSON wire schema、退出码、deadline 和异常边界见
[Worker Host 与进程协议](worker_host.md)。JSON 编解码属于 `apps/worker`，不得反向进入 `contracts`；
运行参数来自受信任配置，任务消息不能覆盖 KDF、内存预算或应用身份配置。

正式父子进程监督使用双向 Worker Session：父进程发送一个 Job，运行中可发送关联当前任务的 Cancel；
Worker 发送 Progress 事件并最终发送一个 Result。Windows Composition Root 通过 `--pipe` 注入
`WindowsNamedPipeChannel`，状态机只依赖 `IMessageChannel`。传输与 ACL 决策见
[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)。

个人版 Service 在提交备份前生成 `file_uuid`、全量 `backup_set_uuid` 和 `created_utc_ms`，并把目标固定为
`archives/YYYY/MM/<file_uuid>.bkf`。增量请求由 Service 按 Catalog 选父（当前树 tip 或显式父），并用
`RecoveryPointGraph::resolve_chain` 校验 tip→Full 祖先链完整；父点须具备 sidecar 与匹配的
`source_volume_ids`。树不完整则**降级为 Full**（保留 Schedule/已识别 set），再下发 Worker。Worker
成功提交 Archive 后，Service 的 Catalog Registrar 读取固定 Header、连续分卷、末卷 Footer 和 Sidecar
Header，确认任务身份与 Archive 一致，再以 create-only 语义发布
`catalog/recovery-points/<file_uuid>.entry`。Catalog 发布失败只记录错误并保留已提交 Archive，后续
Repository 扫描负责补建。选父与降级细节见
[personal_repository.md](personal_repository.md#service-增量选父与树完整判定)。

## WinPE 离线恢复

从旧 WinPE 设计保留以下安全需求：

```text
Online Prepare -> Validate -> Build/Cache WinRE -> Write Pending Job
-> Arm One-Time Boot -> Reboot -> PE Revalidate -> Restore -> Write Result
```

- 在线阶段选择 Recovery Point、映射目标并完成不可逆操作二次确认。
- 写盘前验证目标容量、磁盘稳定指纹、备份链完整性和密钥可用性。
- 备份源不得位于任何将被整盘覆盖的目标磁盘。
- PE 不依赖盘符；通过卷枚举和相对路径定位 Job/Recovery Point。
- Pending Job 使用版本化 Contract，只保存 SecretRef 或受 ACL 保护的短期密钥信封。
- PE 启动后再次匹配磁盘身份，禁止静默退化为只按 disk number 写入。
- Job 必须在破坏分区表前完整读入内存。
- BCD 使用一次性启动，不修改默认启动项；准备失败必须回滚。
- WinRE 挂载失败必须 discard/unmount，避免残留 DISM 状态。
- PE 镜像只携带 Restore Pipeline、必要 Adapter 和最小 UI，不携带 Qt、Service、PostgreSQL、Dokan 或虚拟化 SDK。
- 写盘开始后的取消策略必须由 Restore Plan 明确，不允许假装安全取消。

旧设计中明文 JSON 旁放置 JobKey 的方案不直接采用；具体跨重启 Secret Envelope 需要安全 ADR。

## Mount Host

Mount Host 只读打开 Recovery Point，组合 `PersonalArchiveChainReader`、`WholeDiskByteReader`、Dokan/VHDX Adapter。
每个挂载会话具有独立生命周期、取消和临时 overlay；Shell Extension 不加载这些实现。

### Service 编排（S7）

- `MountSupervisor` 在 `apps/service` 内维护内存态会话表（session_id → host PID、pipe、summary）。
- Service 通过 `--pipe` 启动 `aegra_mount_host`，发送 mount 请求 JSON，等待 `mounted`/`failed` 事件后向 Desktop 返回 CommandAcknowledgement。
- 能力位：`mount.list`、`mount.start`、`mount.unmount`。协议 kinds：8 / 41 / 42。
- 可选 CLI：`--mount-host-path <abs>`；默认与 Service 同目录的 `aegra_mount_host.exe`。
- Overlay 根：`<data_dir>/mount_overlays/<session_id>`（Service 已按会话隔离，host 不再二次嵌套）。
  - Dokan 挂载点：`<session>/mnt/`（必须为空目录，与旧 backup host 一致）。
  - COW sidecar：`<session>/diskN.vhdx.overlay(.map)`，**不得**放在 `mnt/` 内，否则 Dokan 报 `DOKAN_MOUNT_POINT_ERROR`。
- Tear-down：unmount 命令 → 终止 host → join waiter；Service 析构时 `shutdown()` 清理全部会话。
- MVP：整盘只读、会话不落盘；崩溃后 list 可见 `mount.host_exited` 失败态。

## 验证

- 每个 Composition Root 必须可独立构建，并人工核对正常装配与缺配置行为。
- 进程边界必须审查异常、退出码、取消和崩溃恢复。
- WinPE 的人工验收覆盖 UEFI/BIOS、盘号变化、源位于目标盘、缺链、错误密钥和还原后启动。
- Mount Host 的人工验收覆盖并发读、强制卸载、overlay 清理和损坏 Recovery Point。
