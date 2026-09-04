# `virtualization` 模块开发文档

## 目标

定义平台无关的虚拟机发现、快照、变化块、虚拟磁盘和恢复编排。VMware/Hyper-V SDK 只存在于 Adapter/Connector Host。

## 能力接口

- `IVmInventory`
- `IVmSnapshotProvider`
- `IChangedBlockProvider`
- `IVirtualDiskReader` / `IVirtualDiskWriter`
- `IVmProvisioner`
- `IVmLifecycle`
- `IBootCheckProvider` / `IBootCheckVmSession`

不创建要求所有平台实现所有能力的巨型接口。Connector 在握手时报告 capability。

## VM 备份流程

```text
Discover -> Begin Snapshot Session -> Collect Provider Metadata
-> Query CBT/RCT -> Wrap Disks as IBlockSource -> Backup Pipeline
-> Commit -> Remove Snapshot
```

## 不变量

- 多磁盘一致性由同一个 Snapshot Session 表达。
- Snapshot 通过 RAII 在成功、失败和取消路径清理。
- CBT/RCT generation 不匹配时退化为全量，不生成不完整增量。
- provider metadata 显式版本化，不把厂商对象写入通用 Manifest。
- VM 磁盘读取和物理磁盘读取使用同一 Pipeline。

## 恢复范围

接口应可扩展到原位置、新位置、单磁盘、VMDK/VHDX 导出、文件级挂载和 Instant Recovery。新增模式通过 Use Case 与 Adapter 扩展，不修改通用 Pipeline。

Post Backup BootCheck 的开发设计见
[Post Backup BootCheck 开发设计](../development/BOOT_CHECK_DEVELOPMENT_PLAN.md)。该设计使用独立 Host、
可替换的 hypervisor Adapter（VirtualBox 或 Hyper-V）、只读虚拟 parent 和临时 differencing medium；
VM 运行状态不能作为 Windows 启动成功判据。主判据是 Hyper-V Integration Services 心跳
（`IBootCheckVmSession::guest_heartbeat_ok`，`Ok*` 即 guest OS 已运行，无需注入 agent）；心跳不可用时
（集成服务被禁用或 VirtualBox 无 Guest Additions）退化为 differencing medium 的持续增长（阈值+最短
时长+持续增长）。`IBootCheckProvider::parent_disk_format()` 声明 provider 需要的 parent 容器格式
（`kVmdk`=VirtualBox、`kVhdx`=Hyper-V），Host 据此选择 Dokan 呈现形态；`BootCheckVmRequest`
携带格式无关的 `parent_disk_path`。

当前 `IBootCheckProvider` 只描述单次隔离 VM 的 capability、创建、启动、状态、关机和清理，不暴露
`VBoxManage`、厂商 enum 或 Win32 handle。Session 是单调用方对象；`create` 成功后调用方必须在所有终态
显式调用 `cleanup`，析构只执行 15 秒有界的 best-effort 清理。启动确认由调用方（BootCheck Host）编排：
Host 轮询 `guest_heartbeat_ok`（主）与 `BootCheckVmInfo::child_medium_path` 增长（兜底）及 session 状态，
超过 request 的 `overlay_limit_bytes` 时 Host 必须先停止 VM，再清理 session；Provider 只经
`guest_heartbeat_ok` 暴露 hypervisor 自带的 guest 心跳（Hyper-V），不注入 agent，也不承担文件系统 quota。

`inspect` 对 provider 缺失、版本不支持、host driver 不可用或 headless probe 失败返回
`available=false` 和稳定 message code；取消返回 `Result` failure。VirtualBox 只接受 7.1/7.2；
Hyper-V 的判据为 vmms 服务 Running 且 PowerShell Hyper-V 模块可解析。

用户选择由 `contracts::BootCheckHypervisor` 表达并快照到 durable post-backup plan（ADR-0029）。Host 只
实例化选中的 provider，不把 `inspect` 失败解释成切换平台的许可。Service 的 installed capability 是轻量
安装探测，与这里的运行态 `inspect` 语义严格分开。

历史：ADR-0028 的 COM1 Guest Probe（V1 READY 固定标识、`boot_probe_protocol.h`、
`IBootCheckVmSession::wait_for_boot_probe`）已于 2026-09-03 移除，被差分盘增长判据取代（本模块自此为
header-only INTERFACE 库）；2026-09-04 又以 Hyper-V 心跳 `guest_heartbeat_ok` 为主判据、差分盘增长
为兜底，解决 overlay 阈值在引导转圈阶段误判的问题。

## 验证

构建 Connector 生产 Target，审查快照清理、CBT 失效、多磁盘、取消和 SDK 异常路径。真实平台仅执行隔离的
人工验收，并禁止把生产凭据写入仓库。
