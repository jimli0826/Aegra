# ADR-0027：Boot Profile 与 Manifest Schema 2

- 状态：Accepted
- 日期：2026-08-29
- 决策者：Aegra maintainers
- 关联模块：format、adapters/windows_disk、adapters/personal_archive、apps/worker、virtualization

## 背景

现有 Personal Archive V7 Manifest 保存 Disk、Partition、raw MBR/GPT、Volume 与 extent，但没有明确保存
Windows 系统卷、实际 firmware、启动所需分区和受 TPM/BitLocker 影响的兼容性事实。BootCheck 若改用执行时
Inventory 推断，会把当前主机状态错误地当成 Recovery Point 事实，也无法安全校验增量链。

产品尚未发布，不需要兼容开发期 Manifest schema 1。

## 决策

1. Personal Archive V7 的当前 CBOR Manifest schema 直接升级为 2；Header 与加密 CBOR 的 schema version
   必须同时为 2，Reader 不解析 schema 1。
2. Manifest 根固定增加 `boot_profile`。它对 file_set 和不完整/不支持的 volume_set 为 `null`；仅当 Worker
   证明 Source 包含完整、受支持的 Windows 系统盘时写入固定 Map。
3. Boot Profile 记录系统盘号、Windows volume index、必需启动分区、实际 BIOS/UEFI、x64 OS build、
   Secure Boot/TPM/BitLocker 状态、512-byte logical sector、布局 SHA-256、probe protocol 与 Aegra Service
   版本。只保存状态，不读取或保存 BitLocker/TPM key。
4. Windows API/TBS/WMI 探测属于 `adapters/windows_disk`；纯模型、编码、拒绝规则和布局指纹 preimage
   属于 `format`；`apps/worker` 只负责把真实 Disk/Volume/extents 与主机探测交叉验证并计算 SHA-256。
5. 完整系统盘要求系统卷不跨盘、系统盘所有具有稳定 Volume identity 的非零卷都在选择中；UEFI 还要求
   已选择 ESP 与完整 GPT raw layout，BIOS 还要求已选择活动分区、MBR signature 和非空 bootstrap code。
6. 相邻 volume_set 增量层的 Boot Profile 必须同时为空，或在系统盘身份、必需分区、firmware、sector、
   布局 fingerprint 和 probe protocol 上一致。不一致的父层不能作为本次增量基线。
7. Boot Profile 中 `unknown` 的安全状态是认证事实，不代表支持。BootCheck eligibility 必须 fail-closed；
   特别是无法证明 BitLocker 关闭时，不得启动 VM。

## 备选方案

- **Provider extension：** BootCheck 是 Aegra V7 的恢复语义，不是厂商私有附加数据；extension 无法提供统一
  的必填字段与链兼容验证。
- **只在运行时读取 Inventory：** 不能证明 Archive 实际包含哪些启动分区，也无法认证历史状态。
- **复制完整虚拟磁盘后再探测：** 成本高且发现不支持过晚，不能替代写入期结构验证。
- **兼容 schema 1：** 产品未发布，会永久扩大解析和安全审计面，因此拒绝。

## 影响

- 开发期 schema 1 Archive 被当前 Reader 拒绝，需要重新备份。
- 普通数据盘和不完整系统盘备份不受阻断，但 Manifest 的 `boot_profile` 为 `null`，不能运行 BootCheck。
- Windows Disk Adapter 新增只读 TBS/WMI 依赖；BitLocker 查询不请求恢复密钥。
- 系统盘布局变化会阻止继续使用旧层做增量，Service 后续必须按既有策略降为 Full。

## 验证

- Debug/Release 生产构建必须通过，Format Target 不得获得 Windows 或 Adapter 依赖。
- 人工检查 schema 2 CBOR 的 Profile/null 两个分支及非法引用拒绝。
- 在管理员 Windows 主机人工覆盖 BIOS/UEFI、完整/缺少启动分区、BitLocker on/off/unknown 与增量布局变化。
- BootCheck Host 在启动 VirtualBox 前重新认证 Manifest/chain 并执行同一 fail-closed eligibility。
