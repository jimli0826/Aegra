# ADR-0023：进程内 Explorer `.bkf` 只读浏览

- 状态：Accepted
- 日期：2026-08-11
- 决策者：Aegra Maintainers
- 关联模块：apps/shell_extension、adapters/ntfs、adapters/personal_archive、personal_repository、ports、format
- 替代：`MODULAR_ARCHITECTURE.md` 中“Shell Extension 只通过 IPC 请求 Mount Host”的目标描述

## 背景

用户需要在 Windows Explorer 中双击 current V7 `.bkf` 后，把 Archive 作为只读虚拟文件夹浏览并复制文件。此前目标架构把 Shell Extension 定义为轻量 IPC 桥，由 Mount Host + Dokan/VHDX 呈现盘符或挂载点。该路径对“打开压缩包式”浏览过重，并引入：

- 额外进程、盘符/挂载点与 overlay 生命周期；
- Dokan 驱动与 VHDX 依赖；
- `volume_set` 与 `file_set` 两种内容模型被迫统一成虚拟磁盘语义。

`file_set` 不含 NTFS Boot/MFT，不能伪装成卷镜像；`volume_set` 需要从 Archive 块流直接解析 NTFS。参考实现 `backup/src/ShellExtension` 证明 Explorer File Root/Folder Junction + in-process Shell Folder 可行，但其全局 Session/密码 map、iostream 文件流、全量 MFT 扫描与绝对路径 identity 不符合 Aegra 工程与安全标准。

## 决策

1. **进程内浏览：** `aegra_shell_extension` 是 x64 in-process COM DLL，由 `explorer.exe` 加载。它作为 Composition Root 直接装配 Personal Archive Reader 与（仅 `volume_set`）NTFS Adapter，在 Explorer 中呈现只读 Shell Namespace。不创建 Mount Session，不加载 Dokan，不创建 VHDX，不分配盘符。

2. **内容分发（无探测 fallback）：**
   - `content_kind=volume_set` → `PersonalArchiveChainReader` → `PersonalArchiveVolumeRandomReader` → `NtfsVolumeReader` → Disk/Volume/NTFS 树；
   - `content_kind=file_set` → `PersonalFileArchiveChainReader` → tip File Index 目录树与 `read_stream`；
   - unknown/invalid content kind → 稳定 unsupported/corrupt。禁止“先试一种再试另一种”。

3. **依赖边界：**
   - 允许链接：Base、Contracts、Ports、Format、PersonalRepository、LocalStorage、PersonalArchive、NTFS、Crypto、Compression、Windows Shell COM 库；
   - 禁止链接：Qt、Service、Worker、Application Service、Dokan、Mount Host、企业 Gateway/PostgreSQL。
   - `aegra_adapter_ntfs` 只依赖 Base 与 Ports；输入为 `IRandomAccessReader`，不知道 `.bkf`、COM 或路径。
   - Personal Archive Adapter 不依赖 NTFS Adapter；Adapter 之间不直接创建对方实现。

4. **Mount Host 并存：** Mount Host / Dokan 路径保留给需要盘符、整盘镜像或后续虚拟化场景的挂载用例。Explorer 双击 `.bkf` 浏览**不以** Mount Host 为权威路径。文档不得再写“Shell Extension 只通过 IPC 请求 Mount Host”。

5. **COM Module 最小全局状态例外：** 允许模块级 `DllMain` 模块锁、对象引用计数与 `HINSTANCE`，仅用于 COM 卸载与资源定位。禁止业务全局 Session Map、全局密码缓存或全局 Archive 表。Session 由 Root Folder 拥有，经 `shared_ptr` 共享给子 Folder/Enumerator/Stream，不得形成 COM/Session 环。

6. **密码与密钥：**
   - 首次以空密码打开；`Unauthorized` 时显示 Shell 自有密码对话框；
   - Session 缓存只保存 Windows DPAPI 当前用户范围密文，不保存明文密码；
   - 使用缓存时临时解密到可清零缓冲，仅在同步 Reader `open` 期间暴露 view，调用后立即擦除；
   - 不进入 PIDL、注册表、日志、异常消息、临时文件名或跨 Session 缓存；
   - 首版同一链要求同一密码；错误密码可重试，Cancel → `ERROR_CANCELLED` / `shell.password_cancelled`。

7. **链解析：**
   - standalone Full：单层打开；
   - standalone Incremental：`shell.parent_missing`；
   - 受管理 Local Repository：从 Archive 路径向上有界（默认 ≤16）定位 `aegra.repository`，验证路径对应 Catalog `archive_main_key`，用 `RecoveryPointGraph::resolve_chain()` 得 base-first 链；
   - 禁止按文件名/时间猜父层；禁止直接访问 SQLite 控制面。

8. **只读与格式范围：** 仅 current V7；无旧 Archive/Catalog 兼容、dual-read 或 version fallback。不支持 Rename/Delete/New/Paste/写回/修复。产品未发布，PIDL v1 不匹配直接拒绝。

9. **平台：** 首版 x64-only；目标 Windows 10/11 x64 Explorer。注册与卸载只写当前用户 HKCU 下 Aegra 自有 CLSID/ProgID/`.bkf` junction keys。

10. **NTFS 首版行为：**
    - 支持：Boot geometry、USA fixup、`$MFT`、`$STANDARD_INFORMATION`、`$FILE_NAME`、unnamed `$DATA`、resident/non-resident、signed runlist、sparse 零填、`$ATTRIBUTE_LIST`、`$I30`、stale sequence 拒绝；
    - 不跟随 reparse；named ADS 不作为普通文件；
    - compressed/EFS → 稳定 unsupported（`ntfs.compressed_unsupported` / `ntfs.efs_unsupported`），不返回错误明文；
    - 有界 LRU（MFT/index），禁止全量 `ParseMFT()`。

11. **临时文件：** 默认打开物化到 `%LOCALAPPDATA%\Aegra\ShellCache\<session>\<entry>`；随机/session identity；当前用户 ACL；配额默认单文件 16 GiB、总 32 GiB；TTL 清理；失败路径删除未发布临时文件。

12. **产品上限（可配置收紧，不得松于格式硬上限）：** 目录页 256；stream read 1 MiB；NTFS record 64 KiB；index record 1 MiB；attribute list depth 32；导航深度 256；MFT cache 1024；index cache 256；repository-parent walk 16。

## 备选方案

- **IPC → Mount Host + Dokan：** 保留故障隔离，但 Explorer 浏览延迟高、依赖驱动/盘符，且 `file_set` 不适合虚拟磁盘语义；拒绝作为双击浏览权威路径。
- **仅 IPC 到 Service 拉列表/内容：** 每次数组与流经 Service 会放大控制面负载，且 Explorer 同步 Shell 调用对延迟敏感；拒绝。
- **直接移植参考 ShellExtension：** 全局单例、iostream、全量 MFT 与绝对路径 identity 违反工程标准；只借鉴 Shell 行为，拒绝移植实现。

## 影响

- 架构文档与 apps/adapters 模块文档同步描述 in-process 浏览；Mount Host 文档范围收窄为挂载用例。
- Explorer 进程内解析损坏/恶意 Archive 的风险升高；必须 fail closed、有界缓存/深度、COM 边界捕获全部 C++ 异常。
- Shell DLL 体积与直接依赖增加（Archive/Crypto/Compression/NTFS），仍禁止 Qt/Service/Dokan。
- 注册归当前用户；多用户机器上每用户独立注册。

## 验证

- 构建 `aegra_adapter_ntfs`、`aegra_adapter_personal_archive`、`aegra_shell_extension` 的 Debug/Release。
- 静态检查：无 iostream 文件流；Shell 不链接 Service/Qt/Dokan/Mount Host；源码行数/复杂度限制；无旧格式 fallback。
- 人工：注册后双击 `volume_set`/`file_set` Full/Incremental；复制与默认打开；错误密码/缺父/缺分卷/非 NTFS Volume/损坏输入 fail closed；反注册清理 CLSID/ProgID。
