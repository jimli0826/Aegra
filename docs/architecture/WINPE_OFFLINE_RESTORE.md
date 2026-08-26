# WinPE 离线系统盘恢复设计

| 属性 | 值 |
|------|-----|
| 状态 | Accepted；PE0–PE5 已实现，UEFI GPT 系统盘主路径端到端实测通过（2026-08-26） |
| 日期 | 2026-08-25 |
| 关联需求 | [产品范围](../requirements/PRODUCT_SCOPE.md)：系统盘 WinPE 离线恢复；不变量「系统盘恢复必须离线执行，并在写盘前重新确认目标磁盘身份」 |
| 关联决策 | [ADR-0009](../adr/0009-windows-volume-restore-safety.md)（在线写入安全边界）、[ADR-0026](../adr/0026-winpe-offline-restore-and-secret-envelope.md)（一次性启动与跨重启 Secret Envelope） |
| 关联模块 | contracts、ports、application、pipeline、adapters/windows_disk、adapters/crypto_sodium、adapters/windows_pe（新增）、apps/service、apps/pe_restore（新增）、apps/desktop |
| 开发计划 | [WinPE 恢复分阶段开发计划](../development/WINPE_RESTORE_DEVELOPMENT_PLAN.md) |
| 旧仓库参考 | `D:\Work\OpenSource\backup\document\design\WINPE_SYSTEM_RESTORE_DESIGN.md` 及 `backup\src\pe\`（仅作语义参考，不构成行为依据） |

---

## 1. 背景与目标

### 1.1 问题

在线 Windows 无法安全覆盖正在使用的系统卷/系统盘（ADR-0009 明确拒绝）。当前三层防线均为
**拒绝**：Service 预检（`worker_job_service_restore.cpp` 中 `is_system` → Conflict）、
`windows_disk` 适配器写入门（`validate_target_disk_for_raw_restore`）、Desktop UI 禁用系统目标。
系统盘整盘恢复必须在 **RAM 启动的 WinPE** 中离线执行。

### 1.2 一期目标

| 项 | 说明 |
|----|------|
| 场景 | 本机：在线选择 Recovery Point 与目标磁盘 → 准备 PE → 重启 → PE 内确认后恢复 → 重启进入已恢复系统 |
| 数据面 | 复用现有 Restore Pipeline 与 `windows_disk` 整盘恢复路径（`kPhysicalDisk` Sink + `raw_layout` 重建） |
| PE 来源 | 本机 Windows RE（`Winre.wim`）定制注入，不在安装包分发 WinRE |
| 启动 | 一次性 BCD `bootsequence`（不修改默认启动项，ADR-0026） |
| PE UI | Win32 极简进度界面；启动后倒计时确认 |
| 备份源 | 仅本地 Repository（`storage_local` 可达的路径） |
| 密码 | 跨重启 Secret Envelope（ADR-0026），PE 内解封后交给 Archive Reader |

### 1.3 一期非目标

- 可分发 USB / ISO 启动介质（可复用同一 WIM 定制流水线，后续工作包）
- 网络 / 云备份源
- 异机恢复驱动注入（平台驱动目录预留，见 §14）
- PE 内完整恢复向导（选择 Recovery Point、编辑映射仍在在线 Desktop 完成）
- PE 内运行 Qt、Service、SQLite、Dokan、虚拟化 SDK（apps.md 已禁止）
- PE 内文件级恢复、NTFS 缩容恢复（一期仅 `require_source_size` 整盘路径）

---

## 2. 总体架构

```text
┌────────────────────────────────────────────────────────────────────┐
│ 阶段 A：在线 Windows（Desktop / Service / Application）              │
│ 选 Recovery Point → PE 预检 → 定制 WinRE → 写 Pending Job(信封)     │
│ → Arm 一次性启动 → 用户确认重启                                      │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ reboot（one-time bootsequence）
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│ 阶段 B：WinPE（RAM 启动，X:）                                        │
│ winpeshl → aegra_pe_restore.exe → 卷扫描定位 Job → 完整读入内存      │
│ → 解封密码 → 重匹配磁盘身份 → 倒计时 → Restore Pipeline → 写 Result │
│ → 重启                                                              │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ reboot（bootsequence 已消耗，回默认项）
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│ 阶段 C：恢复后首次启动                                                │
│ Service 启动扫描 Result → 发布事件 → 清理 pending / 残留 BCD 条目    │
└────────────────────────────────────────────────────────────────────┘
```

apps.md「WinPE 离线恢复」列出的安全需求全部保留，本文逐条落实（对照见 §13）。

### 2.1 组件职责

| 组件 | 层 | 环境 | 职责 |
|------|-----|------|------|
| `PePendingJobV1` / `PeRestoreResultV1` | contracts | 共享 | 版本化跨重启契约（纯 DTO，无 JSON） |
| `IPeImageBuilder` | ports | — | 定制 `boot.wim` 的端口 |
| `IOneTimeBootController` | ports | — | 一次性启动 Arm / Disarm / IsArmed 端口 |
| `IPePendingJobStore` | ports | — | Pending Job / Result 读写端口（含卷扫描定位） |
| `PeSecretEnvelope` | adapters/crypto_sodium | 共享 | 密码封装/解封（ADR-0026） |
| `adapters/windows_pe` | adapter | 在线 + PE | WinRE 定位与 DISM 定制、bcdedit 封装、Pending Store 文件实现（含 JSON 编解码与 ACL） |
| `PeRestorePrepareService` | application | 在线 | 串联 PE 预检 → 镜像构建 → 写 Job → Arm 的 Use Case |
| `apps/service` | app | 在线 | 协议 V4 新 kind 的 Host 分发、与现有 Restore 预检复用、阶段 C 结果发布 |
| `apps/pe_restore` | app | WinPE | Composition Root + Win32 UI：读 Job、解封、重验证、调用 Restore Pipeline、写 Result、重启 |
| `apps/desktop` | app | 在线 | RestorePage 系统目标解锁为 PE 引导流程；待重启状态展示与取消 |

依赖方向遵守模块化架构：application 只依赖 ports/contracts；`windows_pe` 适配器实现端口；
两个 app（service、pe_restore）通过链接同一 `windows_pe` 适配器共享 Pending Job 的 JSON
编解码，避免 wire 契约在两个 app 各写一份（JSON 不进入 contracts）。

### 2.2 与在线恢复的分支

```text
Desktop 提交恢复
      │
      ▼
目标 Inventory 条目 is_system？
      │
 否 ──┼── 是（disk.N 或系统卷所在盘）
      │         └── PE 离线路径（本文）：kind 19 PreparePeRestore …
      ▼
现有 kind 9/40 在线路径（不变）
```

- 系统 **卷** 目标：一期仍拒绝单卷 PE 恢复，引导用户选择整盘路径（系统卷恢复没有
  不重建引导链的安全语义，整盘恢复语义最清晰）。
- 非系统盘整盘恢复：继续走在线路径，不进 PE。

---

## 3. 目录布局

全部位于 Service 数据目录（[service_host.md](../modules/service_host.md)：缺省
`%ProgramData%\Aegra`）之下；PE 内禁止假设盘符，按 §8.1 卷扫描以相对路径定位。

```text
<data_dir>\pe\
  image\
    boot.wim              # 已注入 aegra_pe_restore 的定制镜像
    boot.sdi              # ramdisk 所需 SDI
    build_id.json         # 缓存失效依据（§6.3）
    mount\                # DISM 临时挂载点（用完必须卸载）
  pending\
    restore_job.v1.json   # 跨重启 Pending Job（含密文信封字段）
    restore_job.v1.key    # 短生命周期 Job Key（ADR-0026；ACL 收紧）
    restore_result.v1.json# PE 写回的执行结果
  logs\
    pe_restore_*.log      # PE 内日志 mirror（§9.6）
```

- `boot.wim` 放系统卷 **允许**：PE 启动后整镜像已在 RAM，覆盖目标盘不影响运行中的 PE。
- 定制 WIM 的存放不豁免「备份链不得位于任何目标盘」预检（§7）。

---

## 4. 端到端流程

### 4.1 在线主路径（阶段 A）

```text
1. Desktop RestorePage：用户选择 Recovery Point，目标为系统盘 → 进入 PE 引导流程
2. kind 19 PreparePeRestore（Query）：
   - 复用现有链解析 / 容量 / 密码验证逻辑（§7 预检全表）
   - 通过 → 签发 PE 预检 token（复用 restore preflight store 的短期占用语义）
3. Desktop 展示目标盘身份（序列号 / 容量 / 总线）+ 不可逆确认；
   若已有未消费 Pending（用户上次准备后尚未重启），先确认覆盖再继续
4. kind 51 ArmPeRestore（Command，携带 token + confirmed=true）：
   a. IPeImageBuilder::ensure_ready()   —— 缓存命中跳过 DISM（首次 1–3 分钟）；
      Desktop 摘要进度卡显示「正在准备 PE 环境」与不确定进度条，完成后 100% 就绪
   b. 生成 PePendingJobV1 + Secret Envelope，IPePendingJobStore::write_pending()
   c. IOneTimeBootController::arm_once()
   任一步失败 → 逆序回滚（Disarm → 删 pending → 保留镜像缓存），返回稳定错误码
5. Desktop 摘要页提供 Restart（用户点击后本机发起重启）；Service 不代发重启
```

Application 状态机（`PeRestorePrepareService`）：

```text
Idle → Validating → BuildingImage → WritingJob → ArmingBoot → AwaitingReboot
任一失败 → Failed（可重试）；用户取消 → Disarm + 删除 pending → Idle
```

`AwaitingReboot` 为持久可观察状态：kind 20 GetPeRestoreState 返回
`{armed, job_uuid, target_summary, created_utc_ms}`，Desktop 重启/重连后仍能呈现并提供取消。

### 4.2 PE 内主路径（阶段 B）

```text
1. WinPE 启动（X: RAM 盘）；winpeshl 启动 wpeinit（PnP/磁盘栈）再启动 aegra_pe_restore.exe
2. 初始化日志（X: 优先）；枚举物理盘与卷
3. 卷扫描定位 restore_job.v1.json；校验 schema_version / 产品版本
4. Job 完整读入内存（在任何破坏性动作之前）
5. 读取 .key，按 ADR-0026 解封密码；AD 绑定校验失败 → 拒绝
6. 通过 Volume GUID + 相对路径解析备份链各层；打开 tip Header 快速校验
7. 重匹配目标磁盘身份（§8.2）；失败 → 拒绝，不退化为按 disk number 写入
8. 摘要 UI + 倒计时（缺省 10 秒）：立即开始 / 取消（写 result=cancelled 后重启）
9. 工作线程执行整盘恢复（§9）；分区表销毁开始后禁用取消
10. 写 restore_result.v1.json（所有可写宿主卷 mirror）；覆写并删除 .key；
    sodium_memzero 清除明文密码
11. 成功 / 取消：自动重启；失败：停留错误页，用户手动重启
```

### 4.3 恢复后（阶段 C）

- 正常 Windows 启动（`bootsequence` 已消耗）。
- Service 启动时扫描 `pending\restore_result.v1.json` → 转换为事件（现有 kind 7 / 45 / 46
  事件通道，新增 `pe_restore.succeeded / pe_restore.failed / pe_restore.cancelled` 事件码）。
- 清理：删除 pending Job / Key / Result；`IOneTimeBootController::disarm()` 清残留条目。
- 镜像缓存保留（下次系统盘恢复复用）。

**系统盘还原的阶段 C 限制（实测）**：目标盘即系统盘时，还原会用备份内容替换目标盘的
文件系统，且 PE 内 `bring_target_online=false` 使目标卷保持未挂载——`persist_result` 无法
写回目标盘的 `%ProgramData%`，重启后进入的是备份自带状态的系统，不含本次 PE 还原的 pending/
result。因此阶段 C 的审计事件对**系统盘**还原不保证触发；用户"重启即进入还原后的系统"本身
即成功自证，Desktop 的 kind 20 在还原系统中回到 not-armed（bcdedit 无产品条目）。阶段 C 事件
对**非系统盘**（数据盘）还原仍正常工作（result 落在系统盘的 `%ProgramData%`，与目标解耦）。
诊断日志始终镜像到档案卷（§9.6 / §9.8），不受此限制影响。

### 4.4 失败分支

| 失败点 | 行为 |
|--------|------|
| PE 预检失败 | 不写镜像 / Job / BCD；返回稳定错误码 |
| 镜像构建失败 | `dism /Unmount-Wim /Discard` 清理挂载；不写 Job、不 Arm |
| 写 Job 失败 | 删除半成品文件；不 Arm |
| Arm 失败 | 删除 pending（先删 `.key`）；不提示重启 |
| 重启后未进 PE | 用户回到原系统；pending 保留；Desktop 呈现「待重启」并可重试 / 取消 |
| PE 内找不到 Job / Key | 不写盘；错误页；可重启回原系统 |
| 信封解封失败 | 不写盘；错误页（提示 Job 可能被篡改或 Key 已失效） |
| 备份链不可达 / Header 校验失败 | 不写盘；错误页 |
| 磁盘身份不匹配 | 不写盘；错误页（防盘号漂移写错盘） |
| 恢复中途失败 | 目标盘可能已损坏；明确提示 + 日志路径；同一备份可重试 |
| 恢复成功但系统无法启动 | 一期保留日志与分区信息辅助排障（引导修复为后续工作包） |

---

## 5. 契约：PePendingJobV1

contracts 中为纯 DTO；JSON 编解码由 `adapters/windows_pe` 的 Pending Store 实现（唯一实现，
service 与 pe_restore 共同链接）。写入文件采用 UTF-8、原子替换（临时文件 + rename）。

```json
{
  "schema_version": 1,
  "job_uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "created_utc_ms": 0,
  "product_version": "x.y.z",
  "operation": "pe-disk-restore",

  "source": {
    "repository_uuid": "…",
    "chain": [
      {
        "file_uuid": "…",
        "volume_guid": "\\\\?\\Volume{…}\\",
        "relative_path": "archives/2026/08/<file_uuid>.bkf",
        "online_path": "D:\\Repo\\archives\\2026\\08\\<file_uuid>.bkf"
      }
    ],
    "source_disk_number": 0,
    "disk_size_bytes": 512110190592,
    "chain_fingerprint": "…"
  },

  "envelope": {
    "mode": "sealed",
    "ciphertext": "<base64>",
    "nonce": "<base64>",
    "binding_digest": "<lowercase hex sha256>"
  },

  "target": {
    "serial_number": "…",
    "size_bytes": 512110190592,
    "bus_type": "NVMe",
    "friendly_name": "…",
    "partition_style": "gpt"
  },

  "options": {
    "preserve_disk_signature": true,
    "auto_expand_last_partition": false
  },

  "ui": {
    "locale": "zh-CN",
    "auto_start_seconds": 10
  }
}
```

| 字段 | 说明 |
|------|------|
| `source.chain` | **base-first 完整链**，在线阶段用 `RecoveryPointGraph::resolve_chain` 解析定稿；PE 内不做链发现、不需要 SQLite |
| `chain[].volume_guid + relative_path` | PE 内首选定位方式（Volume GUID 跨重启稳定）；`online_path` 仅用于日志 |
| `chain_fingerprint` | 与在线 Restore 相同的链指纹算法；参与信封 AD 绑定 |
| `envelope` | ADR-0026；`mode: "sealed"`（密文信封）/ `"prompt"`（PE 内交互输密码）/ `"none"`（未加密 Archive） |
| `target.serial_number` | PE 内重匹配依据；在线阶段序列号不可得时 **拒绝进入 PE 路径**（一期） |
| `options` | 语义与 kind 40 StartRestore 同名字段一致；一期不携带 `partition_layout_edits`（整盘 `require_source_size`） |

`PeRestoreResultV1`：

```json
{
  "schema_version": 1,
  "job_uuid": "…",
  "finished_utc_ms": 0,
  "status": "success | failed | cancelled",
  "error_code": "pe_restore.…",
  "error_message": "…",
  "log_relative_path": "pe/logs/pe_restore_….log"
}
```

错误码进入产品稳定码清单，脱敏规则与 TaskResult 相同。

---

## 6. IPeImageBuilder：WinRE 定制（adapters/windows_pe）

### 6.1 源文件定位（按优先级）

**Winre.wim：**
1. `reagentc /info` 输出的 Windows RE 位置；
2. 恢复分区 `\Recovery\WindowsRE\Winre.wim`（分区枚举复用 `windows_disk` 的 WinRE 分区识别）；
3. `%SystemRoot%\System32\Recovery\Winre.wim`。

**boot.sdi：** `%SystemRoot%\System32\Recovery\boot.sdi`，缺失时枚举已知 Boot 资源路径。

两者缺一 → 预检失败，错误信息给出 `reagentc /enable` 指引。

### 6.2 构建步骤（需管理员 / SYSTEM）

```text
1. build_id 命中且 boot.wim / boot.sdi 存在 → 直接返回
2. 从 Recovery 定位的 Winre.wim 复制为 boot.wim；sdi → boot.sdi（每次重建都从恢复分区取新 WIM）
3. dism /Mount-Wim /WimFile:<boot.wim> /Index:1 /MountDir:<mount>
4. 创建 mount\Windows\System32\Aegra\
5. 注入 payload：显式必选闭包（两个 EXE + libsodium/zstd + Release CRT；缺一即 Arm 失败）
6. 写 mount\Windows\System32\winpeshl.ini：
     [LaunchApps]
     %SYSTEMROOT%\System32\wpeinit.exe
     %SYSTEMROOT%\System32\Aegra\aegra_pe_restore.exe
7. dism /Unmount-Wim /MountDir:<mount> /Commit
8. 写 build_id.json
```

任何失败路径 **必须** 尝试 `/Unmount-Wim /Discard`（apps.md 安全需求：不残留 DISM 状态）；
构建前若发现遗留挂载点，先 discard 再重建。

### 6.3 缓存键 build_id.json

字段：产品版本、payload 文件清单及各文件 SHA-256、启动的执行器文件名、宿主 OS build、
`PePendingJobV1.schema_version`。任一变化 → 强制重建。哈希用 Windows CNG（BCrypt）实现，
使 `windows_pe` 适配器不引入 libsodium 依赖（加密算法仍全部留在 `crypto_sodium`）。
不保留 `winre_base.wim`；每次重建都从 Recovery 分区（或 reagentc / System32 回退）复制
`Winre.wim` → `boot.wim`。残留的 `winre_base.wim` 在 `ensure_ready` 时删除。

### 6.4 许可与体积

仅使用本机 WinRE 做本机恢复，安装包不分发 WinRE；payload 限定为单一 EXE 及其最小依赖，
禁止注入 Qt / SQLite / Dokan / 虚拟化 SDK（apps.md）。

---

## 7. PE 预检（在线，重启前全部完成）

在现有 `prepare_restore` 基础上新增 `prepare_pe_restore`（复用链解析、容量、密码验证，
去掉 `is_system` 拒绝，改为 **要求** 目标为系统盘整盘）：

| 检查项 | 失败行为 |
|--------|----------|
| 调用方能力位（`restore.pe`）与 Service 提升权限 | 拒绝 |
| 目标为 `disk.N` 且 `is_system`（非系统盘走在线路径） | 拒绝 |
| 目标容量 ≥ 源盘 `disk_size_bytes`（`require_source_size`） | 拒绝 |
| 目标序列号可读且非空 | 拒绝（一期不支持无序列号磁盘的 PE 恢复） |
| 备份链完整（base-first，tip→Full 祖先链） | 拒绝 |
| **链中任一 Archive 不得位于任何将被覆盖的目标物理盘**（按 Volume→Disk extent 解析，含跨盘卷） | **拒绝**（最重要） |
| 备份源为本地可读路径 | 拒绝 |
| 加密 Archive：在线试开 tip Header 验证密码 | 拒绝 |
| WinRE 源与 SDI 可定位；`<data_dir>\pe` 所在卷剩余空间 ≥ WIM 大小 × 2.5 | 拒绝 |
| 固件类型可识别（UEFI / BIOS） | 记录；异常时告警并拒绝 Arm |
| 无未消费的既有 pending Job（同一时间仅一个 PE 恢复） | 拒绝（提示先取消） |

「备份在目标盘上」阻断理由与 ADR-0009 第 4 条同源：整盘恢复重建分区表后源数据即毁，
且恢复过程无法继续从目标盘读取。`boot.wim` 位于目标盘允许（已加载入 RAM）。

---

## 8. PE 内定位与重验证

### 8.1 Job 定位（不依赖盘符）

1. 枚举所有固定卷（复用 `windows_volume_enumerator`）；
2. 依次探测 `\ProgramData\Aegra\pe\pending\restore_job.v1.json`（`<data_dir>` 被用户改到
   非缺省位置时，路径以 Job 注入时写入 WIM 的 hint 文件为准，hint 内容为卷 GUID + 相对路径，
   卷扫描仍是兜底）；
3. 找到后 **整份读入内存** 并立即计算绑定摘要，之后才允许任何破坏性动作（apps.md 安全需求）。

### 8.2 磁盘身份重匹配

1. 枚举物理盘，读取序列号、容量、总线、分区风格；
2. `target.serial_number` 精确匹配；容量（±0）与总线作为辅助校验；
3. 匹配到 0 个或多于 1 个 → **拒绝恢复**；
4. 禁止静默退化为按 disk number 写入（apps.md 安全需求；PE 中盘号必然漂移）。

### 8.3 适配器安全边界在 PE 中的形态

`windows_disk` 的 `validate_target_disk_for_raw_restore` 在 PE 中继续生效：
PE 的「系统盘」是 X: RAM 盘所在设备，宿主系统盘在 PE 视角不是系统盘，天然放行；
该检查同时保护 PE 自身启动介质（外置 USB 启动时防止覆盖启动 U 盘，为二期介质做准备）。
「Archive 不在目标盘」在 PE 内 **再执行一次**（解析 chain 各卷所在物理盘），不信任在线预检结果。

WinPE 默认 SAN 策略为 OfflineShared：对本地唯一盘发出的 `IOCTL_DISK_SET_DISK_ATTRIBUTES
OFFLINE` 会被 partmgr 立即重新联机，`GET_DISK_ATTRIBUTES` 仍显示 online。因此 PE 内
`set_target_disk_offline` 不以 OFFLINE 位 fail-closed；它先对目标盘上每个卷执行
`FSCTL_LOCK_VOLUME`（best-effort）+ `FSCTL_DISMOUNT_VOLUME` + `IOCTL_VOLUME_OFFLINE`
（对齐旧项目 `PreparePhysicalDiskForWrite`），再禁用 mountmgr automount，防止重写 GPT
后新分区在写 payload 期间自动挂载。完整 Windows 数据盘路径仍要求 OFFLINE 位粘滞。

---

## 9. apps/pe_restore：WinPE 执行器

### 9.1 定位

执行器 + 进度壳，**不是配置中心**；参数全部来自 Pending Job。Composition Root 规则
（apps.md）适用：装配 Adapter、验证输入、异常→退出码与脱敏日志、处理取消与资源清理。

### 9.2 数据面复用：Worker 进程 + 会话协议（实现定稿）

`aegra_pe_restore.exe` **不在进程内重建还原编排**，而是完整照搬 Service↔Worker 的既有
进程架构：`aegra_personal_worker.exe` 一并注入 WIM，PE 执行器作为 mini-supervisor 通过
Worker Session 协议（ADR-0008，命名管道 + 版本化 JSON）下发一个 `disk_restore` Job 并
接收 Progress/Result 事件。理由：

- 整盘恢复编排（分区表重建、布局解析、两阶段不可逆写入、ADR-0009 复验）全部复用
  Worker 内经过验证的实现，PE 侧零重复；
- 进程协议是 Aegra 认可的跨 Composition Root 边界（Service 侧同样自持一份协议编解码，
  不链接 Worker 库），避免 app 之间的库依赖与跨模块源码 include；
- 「链中 Archive 不得位于目标盘」由 `WindowsBlockSink` 打开时在 PE 内天然复验
  （protected_sources 机制，ADR-0009 第 4 条）。

密码传递：PE 执行器解封信封得到明文后，在 **同一 PE 启动会话内** 调用
`protect_local_machine_secret`（DPAPI machine scope）转为 Worker 已支持的
`dpapi-lm:` SecretRef——DPAPI 在同一 PE 实例内 protect/unprotect 自洽，密码不以明文
出现在管道消息或任何落盘数据中。Worker 侧凭据解析零改动。

PE 执行器直接链接的模块：`adapters/windows_pe`（Pending Store / 卷扫描）、
`adapters/crypto_sodium`（信封）、`adapters/windows_disk`（磁盘身份重匹配）、
`adapters/windows_ipc`（管道）、`adapters/windows_process`（Worker 进程）、
`adapters/windows_system`（时钟 / 随机 / DPAPI）。
**不链接**：Qt、sqlite、Dokan、windows_vss、personal_repository、pipeline、
personal_archive（数据面全部在 Worker 进程内）。

### 9.3 UI 与进程模型

主线程 Win32 消息循环 + 绘制（标题、摘要区、倒计时、阶段文案、进度条、错误区、日志路径）；
工作线程执行恢复；进度经线程安全回调 → `PostMessage` 刷新。语言包内置
en / zh-CN / zh-TW / ja / de（与 Desktop 翻译键对齐，资源编译进 EXE）。

### 9.4 取消策略（Restore Plan 显式定义）

| 阶段 | 取消 |
|------|------|
| 倒计时 / 打开链 / Header 校验 / 身份匹配 | 允许 → `cancelled`，未写盘，重启 |
| 分区表删除开始之后 | **禁用取消**；UI 明示「无法安全中止」（apps.md：不允许假装安全取消） |

### 9.5 重启

成功 / 取消：写 Result 后调用关机 API 重启；失败：不自动重启，停留错误页。

### 9.6 日志（实测定稿）

Worker 任务日志与工作目录固定在 **RAM 盘 X:**（`AEGRA_DATA_DIR`），**绝不放目标盘**
（§9.8 不变量 1）。Worker 退出后 PE 执行器把该日志 best-effort 镜像到**档案卷**
（`<档案卷>\AegraPeRestoreLogs\`）——§7 保证档案不在目标盘上，故重启后仍可读，且不受
系统盘被替换影响。~~原设计的"mirror 到宿主 `\ProgramData\Aegra\pe\logs`"被系统盘场景推翻~~。
密码、Job Key、密文一律不落日志。Worker 任务日志用 Win32 write-through 每行落盘（spdlog/CRT
在 WinPE 不可靠）。

### 9.7 工程约束

- 独立 CMake target `aegra_pe_restore`（apps.md 已预留进程名 `pe_restore`）；子系统 WINDOWS；
- 当前 payload 为显式必选闭包（两个 EXE + libsodium/zstd + Release CRT）；缺任一文件
  则 Arm 失败、不进入 DISM。不注入 debug CRT / zlib。静态三元组缩减清单仍是后续评估项；
- vcpkg 依赖仅 libsodium / zstd / nlohmann-json（PE 内无 sqlite3）。

### 9.8 WinPE 磁盘写入不变量（实测定稿，2026-08-26）

系统盘整盘还原在 WinPE 里的三条硬约束，违反任一都会挂起或失败（已真机验证）：

1. **Worker 的数据目录必须离开还原目标盘。** 目标盘即当前系统盘时，其卷（含
   `%ProgramData%`）在还原前要被 dismount；若 Worker 的任务日志/工作文件开在该卷上，
   dismount 会撞上自己持有的句柄而挂起。PE 执行器给 Worker 设 `AEGRA_DATA_DIR` 指向
   **RAM 盘 X:**，并在 Worker 退出后把日志镜像到**档案卷**（§7 保证档案不在目标盘）。
2. **写入顺序：卷数据在前，分区表在后。** Vista+ 磁盘写保护拒绝对属于已知分区的扇区做
   裸 `PhysicalDrive` 写入（`ERROR_ACCESS_DENIED`）。删除旧分区表后，先把卷数据写到**无
   分区表的裸盘**（此时无分区、无写保护），最后再写 GPT/MBR。
3. **卷释放只用 `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME`。** 不调
   `IOCTL_VOLUME_OFFLINE`（WinRE 派生镜像里会挂起）。`DISK_ATTRIBUTE_OFFLINE` 在 WinPE
   SAN 策略下不生效，仅作 best-effort；WinPE 检测靠 `AEGRA_WINPE=1` 显式信号，不赌注册表
   `MiniNT` 键。

镜像/环境兼容性清单见 [开发计划](../development/WINPE_RESTORE_DEVELOPMENT_PLAN.md#端到端里程碑2026-08-26已达成)。

---

## 10. 协议 V4 扩展（apps/service）

新增 4 个 kind（编号按协议文档权威表顺延，当前 Query 已用至 18、Command 至 50）：

| kind | 名称 | 类型 | 说明 |
|------|------|------|------|
| 19 | PreparePeRestore | Query | §7 预检；成功签发 PE 预检 token（复用 preflight store 短期占用语义） |
| 20 | GetPeRestoreState | Query | `{armed, job_uuid, target_summary, created_utc_ms}`；Desktop 重连后恢复展示 |
| 51 | ArmPeRestore | Command | token + `confirmed=true` + 密码 → 构建镜像、写 Job、Arm；幂等指纹不含密码 |
| 52 | CancelPeRestore | Command | Disarm + 删除 pending（先删 `.key`）；未 Arm 时为 no-op 成功 |

能力位：`restore.pe.prepare`、`restore.pe.arm`、`restore.pe.cancel`。阶段 C 结果经现有事件
通道发布，不新增 kind。协议正式编号与逐字段 wire 说明在实现变更中更新
[SERVICE_CONTROL_PROTOCOL_V4.md](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)（或按 ADR-0017
的升版规则处理）。

---

## 11. Desktop 集成

- RestorePage 系统目标从「禁用」改为「引导 PE 流程」：入口文案说明将重启进入恢复环境；
- 二次确认对话框展示目标序列号 / 容量 / 型号，要求勾选不可逆确认；
- Arm 过程展示不确定进度（首次 DISM 1–3 分钟）；完成后展示「重启以开始恢复」+「取消待恢复」；
- `GetPeRestoreState` 驱动的常驻状态条：应用重启后仍可见待恢复状态；
- 阶段 C 事件在 EventLogPage 与 HomePage 摘要中展示；
- 新翻译键 `aegra.restore.pe_*` 覆盖五种语言。

---

## 12. 安全

| 风险 | 缓解 |
|------|------|
| 写错磁盘 | 在线二次确认 + Job 序列号 + PE 内重匹配（0 或 >1 命中即拒绝） |
| 半截恢复 | 明确 `failed` + 可重试指引；写盘开始后禁用取消 |
| 密码泄漏 | Secret Envelope（ADR-0026）：密文 + 分离短命 Key + AD 绑定 + ACL + 用后即焚 + `sodium_memzero` |
| Job 被篡改 | 信封 AD 绑定 `job_uuid / chain_fingerprint / target.serial_number`，改动任一字段解封即失败 |
| Secure Boot | 仅用微软签名的 winload 与本机 WinRE 框架；注入内容为数据文件 |
| 残留启动项 / DISM 挂载 | 失败逆序回滚；阶段 C Disarm；卸载清理 `<data_dir>\pe` |
| 盘符 / 盘号漂移 | 全程 Volume GUID + 序列号，禁止盘符与 disk number 作为身份 |

威胁模型边界（与 ADR-0026 一致）：能以管理员权限读写 `<data_dir>` 的攻击者等同于已能改
BCD / 直写磁盘；不声称防御本机管理员与物理接触。

---

## 13. apps.md 安全需求对照

| apps.md 需求 | 落实 |
|--------------|------|
| 在线阶段二次确认 | §4.1 / §11 |
| 写盘前验证容量、指纹、链完整、密钥可用 | §7 + §8.2 + §4.2 步骤 5–7 |
| 备份源不在目标盘 | §7（在线）+ §8.3（PE 内复验） |
| PE 不依赖盘符 | §8.1 卷扫描 + Volume GUID |
| 版本化 Contract + 受 ACL 保护短期密钥信封 | §5 + ADR-0026 |
| PE 内重匹配身份、禁止退化为 disk number | §8.2 |
| Job 破坏前完整读入内存 | §8.1 |
| 一次性 BCD、不改默认项、失败回滚 | ADR-0026 + §4.4 |
| WinRE 挂载失败 discard | §6.2 |
| PE 镜像最小化 | §6.4 / §9.2 |
| 写盘后取消策略显式 | §9.4 |

---

## 14. 后续工作包（非一期）

- USB / ISO 启动介质（复用 §6 流水线 + `bcdboot`/介质写入）；
- 异机恢复：平台驱动目录（VMware pvscsi/vmxnet3、VirtIO、AWS ENA/NVMe/Xen、GCP gVNIC）
  的 DISM 注入与 PE 内 `drvload` 兜底（旧仓库 `PE_Drivers` 语义参考）；
- 网络备份源与 PE 内网络栈初始化；
- 恢复后引导修复（BCD 重建 / `bcdboot`）与分区布局编辑；
- TPM 绑定的 Secret Envelope 强化（ADR-0026 遗留项）。

---

## 15. 人工验收（ADR-0015：无自动化测试）

| # | 场景 | 状态 |
|---|------|------|
| 1 | UEFI 虚拟机：GPT 系统盘整盘恢复后可启动 | ✅ 通过（2026-08-26；27s 写 23.4 GB，重启进入还原系统） |
| 2 | BIOS/legacy 虚拟机：`winload.exe` 路径可启动 | 待测 |
| 3 | 增加数据盘使盘号漂移：序列号仍匹配正确目标 | 待测 |
| 4 | 备份在数据盘、系统在 C:：主路径成功 | ✅ 通过（本次即此拓扑：档案在 AegraRepo 卷，目标系统盘 disk 6） |
| 5 | 备份在目标系统盘：在线预检阻断 | 待测 |
| 6 | 加密备份：PE 内解封成功；删 `.key` 后失败；篡改 Job 绑定字段后解封失败 | 待测（本次为无密码备份，走 none 信封） |
| 7 | 增量链（Full + N 增量）恢复正确 | 待测（本次单层 Full） |
| 8 | Arm 后未重启即取消：Disarm + pending 清空，`bcdedit /enum` 无残留 | ✅ 通过（多轮 debug 循环反复验证取消清理） |
| 9 | 倒计时内取消：不写盘，回 Windows，阶段 C 事件为 cancelled | 待测 |
| 10 | 缺 WinRE / 缺 SDI：在线错误可理解且给出 `reagentc /enable` 指引 | 待测 |
| 11 | build_id 命中跳过 DISM；升级 payload 后强制重建 | ✅ 通过（debug_shell 切换触发重建，多轮验证） |
| 12 | DISM 构建中途杀进程：下次构建自动 discard 残留挂载并成功 | 待测 |
| 13 | 恢复中途断电：再次进 PE 用同一备份重试成功 | 待测 |

核心主路径（UEFI GPT 系统盘、无密码 Full、档案在非目标卷）已端到端实测通过，含
§9.8 三条 WinPE 磁盘写入不变量。其余场景待补测。
