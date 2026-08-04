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

## 验证

构建 Connector 生产 Target，审查快照清理、CBT 失效、多磁盘、取消和 SDK 异常路径。真实平台仅执行隔离的
人工验收，并禁止把生产凭据写入仓库。
