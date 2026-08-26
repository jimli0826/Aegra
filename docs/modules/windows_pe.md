# `adapters/windows_pe` 模块开发文档

WinPE 离线系统盘恢复的 Windows 基础设施适配器。当前范围为 **PE0（跨重启 Pending Store）+
PE1（一次性启动控制器）+ PE2（WinRE 镜像构建）**。后续阶段按
[WinPE 恢复分阶段开发计划](../development/WINPE_RESTORE_DEVELOPMENT_PLAN.md) 推进
（PE3 起为 `apps/pe_restore` 与在线编排）。

权威设计：[WinPE 离线系统盘恢复设计](../architecture/WINPE_OFFLINE_RESTORE.md)、
[ADR-0026](../adr/0026-winpe-offline-restore-and-secret-envelope.md)。

## 目标与非目标

- 目标：为在线 Service 与 WinPE 执行器提供同一份 Pending Job / Result 的文件实现
  （JSON 编解码、原子写、Job Key ACL、WinPE 侧卷扫描定位），以及基于 bcdedit 的
  一次性启动控制器（ADR-0026 A）。
- 非目标：链解析、磁盘恢复数据面、协议 kind 分发、密码加解密算法（属于
  `adapters/crypto_sodium` 的 `pe_envelope`）。

## 依赖

- 允许：`Aegra::Ports`（含 contracts）、Win32（文件、卷枚举、SDDL、Token）、
  nlohmann-json（PRIVATE）、`ports::IProcessLauncher`（由 Composition Root 注入
  windows_process 实现，本模块不直接引用其实现）。
- 禁止：Qt、SQLite、Dokan、其它 Adapter 的实现、libsodium（加密留在 crypto 适配器）。

## 公共接口

`aegra/adapters/windows_pe/pe_pending_store.h`：

| 接口 | 说明 |
|------|------|
| `open_pe_pending_store(request)` | 在线侧：以已知 `<data_dir>` 打开 Store（文件位于 `<data_dir>\pe\pending\`） |
| `locate_pe_pending_store()` | WinPE 侧：扫描所有固定卷的 `\ProgramData\Aegra\pe\pending\restore_job.v1.json`；0 个命中 → kNotFound，多于 1 个 → kConflict（拒绝猜测） |

`aegra/adapters/windows_pe/one_time_boot.h`：

| 接口 | 说明 |
|------|------|
| `open_one_time_boot_controller(request)` | 注入 `IProcessLauncher`，返回 `ports::IOneTimeBootController`（`arm_once` / `disarm` / `is_armed` / `firmware_kind`） |

两者返回 `ports::IPePendingJobStore`（[pe_pending_store.h](../../src/ports/include/aegra/ports/pe_pending_store.h)）：
`write_pending` / `read_pending` / `read_job_key` / `consume_job_key` / `clear_pending` /
`write_result` / `read_result` / `clear_result`。

## 核心不变量

1. **单占用 create-only**：已存在 pending job 时 `write_pending` 返回 kConflict（ADR-0026 B5）。
2. **原子发布**：文档先写 `.tmp` 再 `MoveFileExW(WRITE_THROUGH)`；job 发布失败时已写入的
   Key 文件必须被擦除删除，不留孤儿 Key。
3. **Key 文件 ACL**：`D:PAI(A;;FA;;;SY)(A;;FA;;;BA)`（仅 SYSTEM 与 Administrators，禁继承），
   创建时随 `CREATE_NEW` 一次性设置。
4. **用后即焚**：`consume_job_key` 先覆写内容再删除；重复调用与文件缺失均成功。
5. **双向验证**：编码前与解码后都执行 `contracts::validate_pe_pending_job`；读取有大小上限
   （job 1 MiB、result 256 KiB、key 4 KiB），超限按 kCorruptData 拒绝。
6. **严格 wire shape**：JSON 每个 section 校验精确字段集合；`operation` 固定
   `pe-disk-restore`；未知 envelope mode / status 拒绝。
7. **不落敏感日志**：错误信息只含操作名与 Win32 错误码，不含 Key、密文与密码。

## 目录与 CMake Target

```text
src/adapters/windows_pe/
  include/aegra/adapters/windows_pe/pe_pending_store.h
  src/pe_pending_internal.h    # RAII 句柄、文件助手、codec 声明
  src/pe_pending_files.cpp     # Win32 文件/目录/ACL/UTF-8 助手
  src/pe_pending_json.cpp      # 严格 JSON codec + base64
  src/pe_pending_store.cpp     # Store 实现与 open 工厂
  src/pe_volume_scan.cpp       # WinPE 卷扫描定位工厂
```

Target：`aegra_adapter_windows_pe`（`Aegra::AdapterWindowsPe`，STATIC）。

## 验证

按 [ADR-0015](../adr/0015-no-project-test-suite.md) 无自动化测试。PE0 验证 = 构建
`aegra_contracts`、`aegra_adapter_crypto_sodium`、`aegra_adapter_windows_pe` 生产 Target +
源码规模检查；运行期人工验收（往返一致、ACL 核对、篡改拒绝、卷扫描）随 PE3/PE4 消费方执行，
或使用不入库的本地临时程序。

## 一次性启动不变量（PE1）

1. **固定产品 GUID**：启动条目与 ramdisk 设备选项对象使用两个固定 GUID，全部经
   `bcdedit /create {guid}` 显式创建——身份与成败只依赖退出码，不解析本地化文本。
2. **只动 bootsequence**：`arm_once` 仅设置一次性 `bcdedit /bootsequence`；永不修改
   `{default}`、displayorder 或 timeout。
3. **幂等 Arm**：`arm_once` 先执行完整 disarm 再重建；任一步失败 best-effort 回滚，
   不留半成品条目。
4. **is_armed 语义**：条目存在 **且** bootsequence 引用它；GUID 匹配大小写不敏感。
5. **提权前置**：所有操作先检查 Token 提权，未提权返回 kUnauthorized。
6. **等待不可取消**：单条 bcdedit 命令等待不可取消（避免 BCD 半写状态）；取消只在
   命令之间生效。

## Definition of Done

- [x] PE0：`contracts::PePendingJobV1` / `PeRestoreResultV1` 与验证、绑定 preimage
- [x] PE0：`crypto_sodium::pe_envelope` seal/open（AD = SHA-256(binding)）与 Job Key 生成
- [x] PE0：`ports::IPePendingJobStore` + 本模块 Pending Store 文件实现与卷扫描
- [x] PE1：`ports::IOneTimeBootController` + bcdedit 控制器与固件探测
- [ ] PE1 运行期人工验收：现成 WinPE WIM 上 arm → 重启进 PE → 再重启回 Windows，
      `{default}` 不变；arm 幂等；disarm 后 `bcdedit /enum all` 无产品条目；BIOS 与
      UEFI 虚拟机各一轮
- [ ] 随 PE3/PE4 的运行期人工验收（Pending 往返、ACL、篡改、多卷）
