# ADR-0026：WinPE 一次性启动与跨重启 Secret Envelope

- 状态：Proposed
- 日期：2026-08-25
- 决策者：Aegra 项目
- 关联模块：ports、adapters/windows_pe、adapters/crypto_sodium、apps/service、apps/pe_restore
- 关联设计：[WinPE 离线系统盘恢复设计](../architecture/WINPE_OFFLINE_RESTORE.md)

## 背景

系统盘恢复必须离线执行（[产品范围](../requirements/PRODUCT_SCOPE.md)、
[ADR-0009](0009-windows-volume-restore-safety.md)）。这带来两个跨重启问题：

1. **启动切换**：如何让下一次（且仅下一次）启动进入定制 WinPE，失败时用户仍能回到原系统；
2. **密钥传递**：加密 Archive 的密码在线阶段已验证，PE 阶段需再次使用。DPAPI 用户级与
   机器级密钥在 WinPE（另一个 Windows 实例）中均不可用；[apps.md](../modules/apps.md)
   明确「旧设计中明文 JSON 旁放置 JobKey 的方案不直接采用；具体跨重启 Secret Envelope
   需要安全 ADR」——即本文。

## 决策

### A. 一次性启动

1. 通过 `bcdedit.exe` 创建独立 `osloader` 条目（描述固定为产品恢复项名称），
   `device`/`osdevice` 指向 `ramdisk=[卷]\<boot.wim>,{ramdiskoptions}`，
   `{ramdiskoptions}` 指向 `boot.sdi`；`path` 按 `GetFirmwareType()` 选择
   `\windows\system32\winload.efi`（UEFI）或 `winload.exe`（BIOS）；
   `systemroot=\windows`、`detecthal=yes`、`winpe=yes`。
2. 启用方式 **仅允许** `bcdedit /bootsequence {guid}`：只影响下一次启动，启动后自动消耗。
   **永不修改 `{default}`、`displayorder` 的默认项或超时。**
3. 端口 `IOneTimeBootController` 冻结为 `arm_once / disarm / is_armed / firmware_kind`
   四个操作；`arm_once` 幂等（同名条目先删后建）；GUID 不出现在端口签名中。
4. 准备流程任一后续步骤失败必须调用 `disarm` 回滚；`disarm` 同时清理仍指向该条目的
   `bootsequence`。卸载产品时执行 `disarm` 并删除 PE 工作目录。
5. 实现使用 `bcdedit` 子进程并解析其输出；未来若替换为 WMI/BCD API，端口签名不变。

### B. 跨重启 Secret Envelope

1. **信封三态**（Pending Job `envelope.mode`）：
   - `none`：Archive 未加密，无密文字段；
   - `prompt`：不落任何密钥材料，PE 内交互式输入密码（用户可在在线确认时选择）；
   - `sealed`：默认模式，密文信封如下。
2. **sealed 构造**（全部使用 libsodium，与 ADR-0001/0005 依赖一致）：
   - 在线生成 256-bit 随机 Job Key（`randombytes_buf`）；
   - `crypto_aead_xchacha20poly1305_ietf_encrypt_detached` 加密 UTF-8 密码，
     nonce 随机 24 字节；tag 并入密文字段；
   - **Additional Data = SHA-256(canonical binding)**，canonical binding 为
     `schema_version | job_uuid | chain_fingerprint | target.serial_number | target.size_bytes`
     的定序 UTF-8 拼接。Job 中任一绑定字段被篡改 → PE 内解封必然失败，不需要独立 HMAC。
3. **Job Key 存放**：与 Job JSON 分离的 `restore_job.v1.key` 文件，写入时设置显式 DACL
   （仅 SYSTEM 与 Administrators，禁用继承）；不写入 Job JSON、不写入 WIM 镜像、不落日志。
4. **生命周期（用后即焚）**：
   - PE 内读取并解封成功后、开始写盘前，覆写并删除 `.key`；
   - 取消待恢复（在线或 PE 内倒计时取消）、Arm 失败回滚时同样先删 `.key`；
   - 明文密码与 Job Key 使用后 `sodium_memzero`；进程内不复制到可增长容器之外。
5. **单占用**：同一时间至多一个 pending envelope；存在未消费 pending 时拒绝新的 Arm。

### C. 威胁模型边界

sealed 模式防御的是：备份文件与 Job JSON 的离线拷贝泄露（无 Key 文件则密文不可解）、
Job 内容篡改（AD 绑定）、密码进入日志或事件。**不防御**：本机管理员或 SYSTEM 级攻击者
（其已能改 BCD、直写磁盘）、物理接触与磁盘取证（Key 文件在重启窗口内存在于盘上）。
接受该边界的理由：恢复窗口短（Arm→重启→消耗）、Key 与数据分离、且任何更强方案
（见备选）在 WinPE 可用性上不成立。

## 备选方案

- **沿用旧仓库「明文 JobKey 旁放 JSON」**：无 AD 绑定，Job 可被篡改后仍解密成功；
  ACL 未定义。作为基线被本 ADR 的 AD 绑定 + 显式 DACL + 单占用语义替代。
- **DPAPI（用户级 / 机器级 / CNG DPAPI-NG）**：WinPE 是独立 Windows 实例，不持有宿主
  DPAPI 主密钥，PE 内无法解密。不可用。
- **TPM 密封（NCrypt PCP / TPM2.0）**：可把 Job Key 密封到 TPM 并绑定 PCR。WinPE 中
  TPM 栈可用性依赖镜像组件与硬件，失败模式复杂（PCR 变化即拒解），一期不采用；
  列为后续强化项，可在 `envelope.mode` 上新增 `tpm` 值向后兼容。
- **PE 内强制交互输密码（仅 prompt 模式）**：最安全但破坏无人值守恢复；保留为可选模式
  而非唯一模式。
- **把密码写入 WIM 镜像内**：镜像有缓存与复用语义，生命周期远长于单次恢复，泄露面更大。
  不采用。

## 影响

- ports 新增 `IOneTimeBootController`、`IPePendingJobStore`；`adapters/crypto_sodium`
  新增 `pe_envelope` 单元（复用既有 AEAD 封装风格）。
- `adapters/windows_pe` 为新适配器：bcdedit 封装、DISM 定制、Pending Store（含 JSON
  编解码与 DACL 写入）。apps/service 与 apps/pe_restore 共同链接，JSON 不进入 contracts。
- 卸载与阶段 C 清理必须覆盖 BCD 条目与 pending 目录，否则留下可被复用的启动项。
- `bcdedit` 输出解析存在本地化风险：创建条目时显式指定并回读 GUID，不解析本地化文本字段。

## 验证

（ADR-0015：无自动化测试，以下为人工验收）

- `arm_once` 后 `bcdedit /enum {bootmgr}` 可见 `bootsequence`，重启一次后自动消失；
  `{default}` 全程不变。
- `arm_once` 连续调用两次不产生重复条目；`disarm` 后 `bcdedit /enum all` 无产品条目。
- sealed 信封：篡改 Job 的 `target.serial_number` 后 PE 内解封失败且不写盘；
  删除 `.key` 后解封失败；正常路径恢复完成后 `.key` 不存在。
- `prompt` 模式：盘上不存在任何密钥材料；PE 内密码输错可重试、可取消。
- Key 文件 DACL 经 `icacls` 核对仅含 SYSTEM 与 Administrators。
