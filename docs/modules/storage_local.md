# Windows Local Object Storage Adapter 开发文档

## 目标

在 Windows 本地文件系统上实现 `ports/object_storage.h` 的细粒度对象能力，供个人版受管理 Archive Store
以及后续企业 Local CAS Repository 复用。实现必须提供隔离的 staging、原子单对象发布、opaque
generation、分页列举和 generation 保护删除。

本模块不识别 Repository Descriptor、Catalog、Recovery Point 或 `.bkf` 内容，不实现 Scanner、Delete Plan、
保留策略、凭据、SQLite 或企业 Chunk Index，也不把 UNC/SMB 路径伪装成本地存储。SMB 使用独立 Adapter。

## 目录、Target 与依赖

```text
src/adapters/storage_local/
├── CMakeLists.txt
├── include/aegra/adapters/storage_local/
│   ├── local_object_storage.h
│   └── windows_scratch_store.h
└── src/
    ├── local_object_storage.cpp
    ├── local_staged_writer.cpp
    ├── local_storage_internal.h
    ├── local_storage_paths.cpp
    ├── windows_scratch_index.cpp
    ├── windows_scratch_internal.h
    ├── windows_scratch_io.cpp
    └── windows_scratch_store.cpp
```

- Target：`aegra_adapter_storage_local` / `Aegra::AdapterStorageLocal`。
- 仅在 Windows 构建，只依赖 `Aegra::Ports`、C++ 标准库和 Windows SDK。
- 公共头不包含 `Windows.h`，平台句柄和实现状态通过私有 state 隔离。
- 禁止依赖 `personal_repository`、Memory Adapter、数据库、Archive Adapter、`windows_disk` 或 Application。

## 公共接口

`LocalObjectStorage::open()` 接受：

- `root`：本地绝对或可转换为绝对路径的 Repository 根；
- `LocalRootMode::kOpenExisting`：根不存在时返回 `kNotFound`；
- `LocalRootMode::kCreateIfMissing`：逐级创建缺失目录；
- `maximum_read_size`：单次 `read_range()` 的硬上限，必须大于零。

`LocalRepositoryStorageFactory::create_empty()` 在根缺失时创建目录，在根已存在时要求目录完全为空；非目录、
非空目录和不安全路径均拒绝。它只提供初始化所需的空 Storage Access，不写 Repository Descriptor；Descriptor
由 Application 暂存、条件发布并读回验证。

一个实例同时实现 `IObjectReader`、`IStagedObjectWriter`、`IPrefixEnumerator`、`IObjectPublisher`、
`IObjectDeleter` 和 `IObjectStorageCapabilities`。对象 key 使用 Port 的 Repository 相对 key 规则；
`.aegra-internal` 及其后代是 Adapter 保留命名空间；为避免默认 Windows 文件系统别名，保留前缀按 ASCII
大小写不敏感比较。

实例的能力对象可以并发调用。读操作获取共享锁，写、发布和删除获取独占锁；单个 staged write session
仍只允许一个调用方使用。一个 Repository 根在同一时刻只能由一个拥有写权限的 Aegra 进程管理；多进程
Repository operation coordination 属于上层 Repository/Gateway，不由本 Adapter 的进程内锁替代。外部工具
绕过 Adapter 修改文件不属于条件写协议，后续读取仍必须通过 generation 对账发现变化。

## 路径与安全边界

1. 打开根目录时先转为规范化绝对本地路径，拒绝 UNC 和扩展路径输入。
2. 从卷根到 Repository 根逐级检查目录；任何 reparse point 或非目录组件都返回冲突。
3. canonicalize 后再次逐级复核，并在内部保存 `\\?\` 扩展路径以支持长路径。
4. key 先验证相对路径规则和 UTF-8 到 UTF-16 转换，再转换为 Windows 首选分隔符。
5. 创建或打开对象前逐级检查对象父目录，拒绝 `.`、`..`、reparse point 和非目录组件。
6. 最终对象用 `FILE_FLAG_OPEN_REPARSE_POINT` 打开并通过句柄复核，目录和 reparse point 不能作为对象。
7. 列举遇到公开树中的 reparse point 返回冲突；内部目录完全排除在结果之外。

逐级检查可以阻止 Repository key 主动穿越根目录。它不是对具有同目录写权限的恶意本地进程的沙箱；
部署必须使用最小 ACL，防止不受信任进程在检查与 I/O 之间替换目录。

## Staging 与发布

调用方传入的 staging key 是完成后可见的普通对象 key。实际流式写入先落到：

```text
.aegra-internal/writes/<staging-key>
```

开始写入使用 create-new，禁止覆盖已有 partial。Session 可以多次顺序 `write()`；空写成功。未成功完成的
Session 在 `abort()` 或析构时关闭句柄并删除本次 partial。`complete()` 的顺序固定为：

```text
FlushFileBuffers(partial) -> close -> MoveFileEx(partial, public-staging, WRITE_THROUGH)
```

进程崩溃可能留下内部 partial，但它不会进入公开枚举，也不能被发布。孤立 partial 的安全扫描和回收属于
后续 Repository Maintenance；当前实现不得按时间猜测并删除可能仍在使用的写会话。

只有完成后的公开 staging 对象可以发布。create-only 使用不带 replace 标志的原子 rename，目标已存在返回
冲突。generation 条件替换先在实例独占锁内读取并校验目标 generation，再使用 `ReplaceFileW` 发布。
发布成功后重新读取目标属性；若属性读取失败，返回 `kOutcomeUnknown`，调用方必须通过 Reader 对账，不能
盲目重试 mutation。

## Generation、读取与列举

generation 是 opaque string，由 volume serial、file ID、last-write time 和 size 组合。它必须满足：

- Adapter 关闭再打开后，同一未修改文件 generation 稳定；
- rename 后对象身份仍可识别；
- 文件替换、大小变化或 last-write time 变化后 generation 改变；
- 调用方不得解析、排序或持久化推断其字段。

范围读取允许到 EOF 短读，`offset == size` 返回零；`offset > size`、超过 Windows 有符号文件偏移上限或
无效 key 返回 `kInvalidArgument`。一次调用不读取超过 `maximum_read_size`。

列举按 key 的字节序排序，使用上一页最后一个 key 作为 opaque continuation token，并限制在
`kMaximumObjectListResults`。token 必须属于原 prefix。Local 文件系统声明强 read-after-write 和强 list
consistency；该声明以单一受管 Repository 根和 Adapter 协调写入为前提。

## 删除、取消与错误

删除先通过句柄获取当前 generation，再校验可选 expected generation，最后使用
`SetFileInformationByHandle(FileDispositionInfo)` 删除已校验的文件对象，避免路径在校验后被替换时误删新
对象。缺失对象视为成功。

同一 Adapter 生命周期内，`operation_id + key` 记录成功删除；相同身份改变 expected generation 返回冲突。
记录不持久化，跨进程重试依赖“缺失即成功”和 expected generation 保护。上层持久化 Deletion Tombstone
仍是个人版部分删除恢复的权威。

取消在进入单对象 mutation 前检查。已经进入不可安全中止的 publish/delete 后必须返回实际结果或
`kOutcomeUnknown`，不得伪报取消。Windows 错误映射到稳定 Aegra 错误码；消息只包含操作名和原生数值错误
码，不包含 Repository 路径或客户对象名。

## Scratch Store（ADR-0025 / SR2）

`WindowsScratchStoreFactory` 实现 `ports::IScratchStoreFactory`，在本地绝对路径上创建稀疏文件 Scratch：

- Win32 `CreateFileW` / `ReadFile` / `WriteFile` / `SetFilePointerEx` / `FlushFileBuffers` /
  `DeviceIoControl(FSCTL_SET_SPARSE)`；禁止 iostream 文件流；公共头不包含 `Windows.h`。
- 页面大小 65536；已写页面以 page index 为键的有序 map 跟踪，并保存 payload CRC32；未写页面读零；
  CRC 不匹配返回 `kCorruptData`。
- `allocated_bytes = written_pages * page_size`；超过 `maximum_allocation_bytes` 返回
  `kInsufficientSpace`（消息含 `restore.shrink_scratch_insufficient`）。
- `logical_size_bytes` 是完整源卷地址空间，和物理配额分离；创建前用 `GetDiskFreeSpaceExW` 证明硬配额可用。
- `memory_budget_bytes` 限制内存索引；溢出时将较旧条目 spill 到伴生 `.idx`（有序记录，二分查找），
  查找保持 O(log n)，禁止按写记录线性扫描。
- 打开时若 `forbidden_volume_guid_utf8` 非空，用 `GetVolumePathNameW` +
  `GetVolumeNameForVolumeMountPointW` 解析 Scratch 卷 GUID，与禁止 GUID 大小写不敏感相等则
  `kConflict`。不依赖 `windows_disk` Adapter。
- backing、`.idx` 和 merge 临时文件均使用 create-new 独占创建，绝不覆盖同名文件；
  `close_and_discard` 只删除本实例实际创建并拥有的文件。
- Factory 不创建父目录；Worker 在 Target 发生任何写入前创建
  `<data_dir>/staging/<job_id>/ntfs-shrink/`，逐级拒绝 reparse point，并在 Store 释放后只清理本次创建的空目录。

## 验证

构建 Local Storage 生产 Target，按公共 Object Storage 契约审查 open/create/reopen、staging、generation、
路径拒绝、reparse point 和只读删除语义；并构建验证 Scratch factory。涉及符号链接、空间不足、ACL
拒绝或进程崩溃时，仅在隔离的非生产目录执行管理员人工验证。

## Definition of Done

- Windows Debug/Release 使用 VS 2026 Insiders 构建并遵循公共 Storage Contract。
- root/key/reparse-point/扩展路径边界与本文件一致。
- staging、发布、generation、删除、取消和 unknown outcome 语义有完整文档与验证记录。
- Scratch 硬配额、CRC、卷冲突拒绝与 `.idx` spill 语义与本文件一致。
- Target 只依赖 Ports 和 Windows SDK，公共头不泄漏 Windows 类型。
- 函数、文件、格式、静态检查和秘密扫描通过。

## 当前状态

阶段 12C 已实现上述 Local Object Storage Adapter。SR2 已实现 Windows sparse Scratch Adapter。
Repository Scanner、Catalog Reconcile、Delete Plan 和 Tombstone 执行仍由后续 `personal_repository`
阶段实现。
