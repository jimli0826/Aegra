# WinPE 恢复分阶段开发计划（PE0–PE6）

| 属性 | 值 |
|------|-----|
| 状态 | Proposed（随 [WinPE 离线系统盘恢复设计](../architecture/WINPE_OFFLINE_RESTORE.md) 评审） |
| 日期 | 2026-08-25 |
| 前置 | [ADR-0026](../adr/0026-winpe-offline-restore-and-secret-envelope.md) 接受 |
| 验证约束 | [ADR-0015](../adr/0015-no-project-test-suite.md)：无自动化测试，每阶段列人工验收 |

每个阶段独立可构建、可人工验收；后一阶段不返工前一阶段的公开接口。设计文档章节号
（§n）均指架构设计文档。

---

## PE0：跨重启契约与 Secret Envelope

**交付**

- `contracts`：`PePendingJobV1` / `PeRestoreResultV1` DTO 与字段校验（§5）。
- `adapters/crypto_sodium`：`pe_envelope` seal/open（XChaCha20-Poly1305 + AD 绑定，ADR-0026 B）。
- `adapters/windows_pe`（首个单元）：Pending Store 文件实现——JSON 编解码、原子写、
  DACL 收紧、卷扫描定位（§8.1）。
- ports：`IPePendingJobStore`。

**验收**

- 构建 `aegra_contracts`、`aegra_adapter_crypto_sodium`、`aegra_adapter_windows_pe` 生产
  Target 并通过源码规模检查（仓库不新增测试可执行文件，ADR-0015）；
- 运行期人工验收（写入→读回往返一致、`icacls` 核对 Key DACL、篡改绑定字段后 open 失败、
  删 Key 后 open 失败、双卷环境卷扫描定位）随 PE3/PE4 消费方执行，或由开发者使用不入库的
  本地临时程序完成。

## PE1：一次性启动控制器

**交付**

- ports：`IOneTimeBootController`；
- `adapters/windows_pe`：bcdedit 封装（ADR-0026 A），GUID 回读不解析本地化文本；
- 固件类型探测（UEFI/BIOS → winload 路径选择）。

**人工验收**

- 手工指定现成 WinPE WIM：arm → 重启进 PE → 再重启回 Windows，`{default}` 不变；
- arm 幂等；disarm 清空；BIOS 与 UEFI 虚拟机各验一轮。

## PE2：WinRE 镜像构建

**交付**

- ports：`IPeImageBuilder`；
- `adapters/windows_pe`：WinRE/SDI 定位（reagentc → 恢复分区 → System32\Recovery）、
  DISM mount/注入/commit、失败 discard、遗留挂载自愈、`build_id.json` 缓存（§6）。

**人工验收**

- 首次构建成功且 `winpeshl.ini` 生效（结合 PE1 启动进入注入的占位 EXE）；
- build_id 命中跳过 DISM；改动 payload 哈希后强制重建；
- 构建中途杀进程后重试成功（discard 自愈）；缺 WinRE 时错误信息含 `reagentc /enable` 指引。

## PE3：aegra_pe_restore 执行器（数据面接通）

**交付**

- 新 CMake target `aegra_pe_restore`（静态链接，Win32 子系统，§9.7）；
- Composition Root：卷扫描读 Job → 完整入内存 → 信封解封 → 链解析（Volume GUID + 相对路径）
  → 磁盘身份重匹配（§8.2）→ Restore Pipeline + `windows_disk` 整盘路径 → Result 写回与 mirror；
- Win32 UI：摘要、倒计时、阶段文案、进度条、错误页；五语言资源；
- 取消策略（§9.4）与重启逻辑（§9.5）；日志 mirror（§9.6）。

**人工验收**

- 虚拟机内：手工构建镜像 + 手工 arm，UEFI 下 GPT 系统盘 Full 链恢复后可启动；
- 增量链恢复正确；加密备份 sealed / prompt 两种模式均可恢复；
- 盘号漂移场景（新增数据盘）匹配正确；序列号不匹配时拒绝且不写盘；
- 倒计时取消回 Windows 且未写盘。

## PE4：在线编排与协议接入

### PE4a：应用层编排（已交付）

- `application`：`PeRestorePrepareService`（[header](../../src/application/include/aegra/application/pe_restore_prepare_service.h)
  / [impl](../../src/application/src/pe_restore_prepare_service.cpp)）——`prepare_and_arm`
  串联镜像构建 → 密封 → 写 Pending Job → Arm，任一步失败逆序回滚；`cancel` = disarm +
  clear_pending；`query_state` = is_armed + pending 摘要（kind 20 数据源）。仅依赖 ports +
  注入的 `IPeSecretSealer`，不链接任何适配器。
- `apps/service`：`PeSecretSealer`（[header](../../src/apps/service/src/pe_secret_sealer.h)
  / [impl](../../src/apps/service/src/pe_secret_sealer.cpp)）——composition-root 的密封器，
  基于 `crypto_sodium::pe_envelope` 生成 Job Key、绑定密封、填充 digest。

**验收**：`aegra_application`、`aegra_app_service` 生产 Target 构建 + 源码规模检查通过。

### PE4b：协议 wire 接线（已交付）

- contracts：kind 19/20/51/52、`ArmPeRestoreCommand` / `PeRestoreStateRequest` /
  `PeRestoreState` + 校验（kind 19 复用 `RestorePreflightRequest`/`RestorePreflight`，
  kind 52 复用 `ResourceRef`，编解码零新增大结构）；
- 请求/响应编解码、executor 泳道（预检 lane 1、命令 lane 3）、`service_host` 双分发 +
  `pe_restore.*` message code；
- 磁盘链辅助（`DiskRestoreChain`、指纹 make/parse、`source_disk_size_from_archive`）从
  restore TU 提升到 `worker_job_service_restore_shared`（消除重复）；
- 独立 `PeRestoreJobService`（[header](../../src/apps/service/src/pe_restore_job_service.h)）
  经 `ServiceRuntimeInfo.pe_restore` 挂入，不扩展 `IWorkerJobService`：kind 19 复用共享链
  解析并要求系统盘目标 + 非空序列号 + 链不在目标盘（`windows_pe::locate_pe_archive_layer`
  解析每层卷 GUID / 相对路径 / 所在物理盘）；kind 51 复验 token/大小/密码后转
  `PeRestorePrepareRequest` 交 `PeRestorePrepareService`；
- `service_main`：装配三个 windows_pe 适配器 + Sealer + Prepare 服务（打开失败仅关闭能力位，
  不阻服务启动）；能力位 `restore.pe.prepare/arm/cancel`；启动时阶段 C 扫描 → 审计事件
  （unit-of-work append）→ 清理 pending 与残留启动项；
- 协议文档新增 §12（kind 表 + 四个 kind 逐字段说明 + 阶段 C 事件）。

**说明**：Arm 不做幂等重放表——单占用语义下重复 Arm 返回 kConflict（提示先取消），
Desktop 以 kind 20 刷新状态。kind 19 的 token 与 kind 9 同 store，跨用被两侧 is_system
复检互斥拒绝。

**人工验收**（随 PE5 Desktop 接入后执行）

- 通过 Service 完成整个 Arm → 重启 → 恢复 → 阶段 C 事件链路；
- 备份位于目标盘：kind 19 拒绝；存在未消费 pending：再次 Arm 拒绝；
- kind 52 取消后 `bcdedit /enum` 无残留、pending 目录清空；
- Service 重启后 kind 20 仍返回 armed 状态。

## PE5：Desktop 集成（已交付）

- 客户端：`PeRestoreController`（[header](../../src/apps/desktop/src/client/pe_restore_controller.h)，
  QML 经 `serviceClient.peRestore` 访问）——kind 19→51 链式（prepare 后自动 arm）、kind 52
  取消、kind 20 状态刷新（连接就绪时自动拉取，armed 状态跨应用重启可见）；能力位
  `restore.pe.arm` 驱动 `available`；协议编解码四个 kind 全接
  （`service_protocol_commands.cpp`）。
- RestorePage：系统盘目标在能力可用时解锁（拦截原因/下拉禁用双处放行，标签
  "— offline restore (restart required)"）；离线恢复要求**独占映射**（混排即拒绝）；
  `PeRestoreConfirmDialog`（目标身份 + 重启说明 + 不可逆勾选后才可确认）；页顶
  armed 横幅（目标展示 + 取消按钮，Theme 令牌着色）。
- i18n：17 个新键五语言（含阶段 C 事件码 `aegra.event.pe_restore.*` 与
  `message_code_map` 映射）。**注意**：`generate_ts.py` 相对 .ts 文件已漂移（缺 ~44 条
  既有条目），本次为避免丢翻译采用向 .ts 手工合并 + lrelease；脚本回填已另立任务。
- 结构调整：`service_client.h` 已满 800 行上限，PE 面提取为独立 QObject 子组件而非
  扩展门面；两个私有嵌套枚举外移至 `service_client_state.h`（`using` 别名保持引用不变）。

**验收**：全量生产构建（含 Desktop/QML cachegen）+ 源码规模检查通过。

**人工验收**（待运行期执行）

- 全 GUI 路径完成一次系统盘恢复；应用重启后待恢复状态仍可见并可取消；
- 文案评审：重启警告、不可逆提示、失败指引；五语言渲染；
- EventLog 中阶段 C 事件（succeeded/failed/cancelled）正确本地化显示。

**已知未做**（列入 PE6 或后续）：Desktop 侧 `prompt_for_password` 模式入口（当前始终
密封信封）；Home 页 armed 状态摘要卡。

## 端到端里程碑（2026-08-26，已达成）

UEFI/GPT 系统盘整盘还原在虚拟机端到端实测通过：在线准备 → Arm 一次性启动 → 重启进
WinPE → 卷 dismount → 删布局 → 数据裸写（27s，23.4 GB，稀疏跳过 6.46 GB）→ GPT 最后写 →
自动重启 → **进入被还原的系统**。

真机排查暴露的 **WinRE 定制镜像兼容性清单**（对任何 WinPE 整盘还原通用；已全部修复）：

| # | 坑 | 修复 |
|---|-----|------|
| 1 | payload 缺 exe | `aegra_pe_restore.exe` POST_BUILD 拷到 service 目录 |
| 2 | VC++ CRT redist WinRE 无 | 注入 vcruntime140/msvcp140/_atomic_wait/vcruntime140_1 |
| 3 | worker 依赖 VSS/VirtDisk | 延迟加载 |
| 4 | pending 定位路径 | PE 栈固定用 `%ProgramData%\Aegra`，与 `--data-dir` 解耦 |
| 5 | spdlog(CRT) 不认卷 GUID 路径 | worker 日志改 Win32 write-through；卷扫描返回盘符形式 |
| 6 | WinRE 缺 MiniNT 注册表键 | PE 执行器设 `AEGRA_WINPE=1` 显式信号 |
| 7 | `IOCTL_VOLUME_OFFLINE` 在 WinRE 挂起 | 逐卷 dismount 移除该调用（旧项目从不用） |
| 8 | worker 日志/工作目录在目标盘 | `AEGRA_DATA_DIR` 指向 RAM 盘 X:，执行器把日志镜像到档案卷 |
| 9 | 先写分区表触发写保护 | **数据在前、分区表在后**（裸盘无分区时写数据，GPT 最后落） |

诊断设施（保留）：worker 任务日志 Win32 write-through 每行落盘；`set_target_disk_offline`
子步骤面包屑（`offline_step`）；`bcdedit` 命令级日志（`event=pe_restore.bcdedit`）；
`AEGRA_PE_DEBUG_SHELL=1` 构建带 cmd 的镜像做缺 DLL 排查。

参考实现（跑通的旧项目）：`D:\Work\OpenSource\backup\src\engine\{disk_device,restore_planner,restore_engine}.cpp`。

## PE6：加固与收尾

**交付**

- §4.4 失败分支逐条落实与稳定错误码清单；`restore.credential_unavailable` 误映射复核
  （kUnauthorized 目前既表凭据也表写盘 ACCESS_DENIED，PE 场景需区分）；
- 卸载清理（disarm + 删除 `<data_dir>\pe`）；
- 设计文档 §15 人工验收矩阵全量执行并出记录（对齐 SR10 验证矩阵的做法）。

**验收**

- §15 全部 13 项通过并留档；已知限制（无序列号磁盘、单卷 PE 恢复、异机驱动）写入产品范围说明。

---

## 后续工作包（不在本计划内）

启动介质（USB/ISO）、异机驱动注入、网络源、引导修复、TPM 信封强化——见设计文档 §14。
