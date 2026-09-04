# ADR-0029：BootCheck Hypervisor 由用户选择并持久化

- 状态：Accepted
- 日期：2026-08-31
- 决策者：Aegra maintainers
- 关联模块：contracts、ports、apps/service、apps/desktop、apps/boot_check、adapters/sqlite、adapters/virtualbox、adapters/hyperv

## 背景

BootCheck 同时支持 VirtualBox 和 Hyper-V。由 Host 按固定优先级自动选择会让一次任务的执行环境不透明，
并且 Schedule 保存后无法保证重启恢复的 durable action 使用用户期望的平台。Desktop 还需要在创建或编辑
Schedule 时区分已安装和未安装的平台，但不得自行读取注册表或 Windows Service Manager。

## 决策

1. `BootCheckHypervisor` 是版本化控制面合同，数值为 VirtualBox(1)、Hyper-V(2)。启用 BootCheck 时
   Schedule 必须同时保存一个合法值；关闭时必须为 null。
2. 创建 Backup Job 时把选择快照到 `post_backup_plans`。后续 Schedule 修改不改变已经排队的动作。
3. BootCheck Job schema 升为 2 并携带必填 `hypervisor`。Host 只检查和创建指定 provider；指定 provider
   缺失或不可用时任务失败为 `bootcheck.provider_unavailable`，禁止自动切换。
4. Service 启动时探测安装状态并通过 `GetServiceInfo.capabilities` 发布
   `boot_check.hypervisor.virtualbox.installed` 和
   `boot_check.hypervisor.hyperv.installed`。VirtualBox 以可信 `VBoxManage.exe` 是否存在为安装判据；
   Hyper-V 以 `vmms` 服务是否存在为安装判据。安装不等于运行时可用，签名、版本、服务运行状态、模块和
   headless capability 仍由 provider `inspect()` 在任务执行时检查。
5. Desktop 只消费 Service capability：未安装项显示“未安装”且不能选择；没有任何支持的平台时不能新启用
   BootCheck。已保存但后来卸载的平台仍显示原选择，用户可以关闭 BootCheck 或改选已安装平台。
6. Control Plane schema 升为 25。产品尚未发布，不提供 schema 24 或 BootCheck Job schema 1 的迁移和
   fallback；开发数据库需重建。

## 备选方案

- Host 自动选择或失败后回退：拒绝。结果不可预测，也破坏 durable plan 的请求身份。
- Desktop 直接查询注册表、SCM 或 PowerShell：拒绝。会复制平台逻辑并越过 Service 权限边界。
- 在保存 Schedule 时运行完整 provider probe：拒绝。创建 UI 会受外部命令延迟影响，而且安装状态与任务
  执行时可用状态本来就是不同语义。

## 影响

Schedule V4 payload/summary、SQLite Schedule/Post Backup Plan、备份幂等指纹和 BootCheck 子进程请求均
增加 hypervisor 字段。卸载或停用平台不会静默改用另一个平台；任务日志中的 provider stage 可用于诊断。

## 验证

- Debug 生产构建、源码规模与架构检查通过。
- UI 人工覆盖：两者已安装、仅安装一个、均未安装、编辑后平台被卸载。
- 分别用 VirtualBox 和 Hyper-V Schedule 完成 Backup → Verify → BootCheck，并确认 Host 只创建选中平台。
