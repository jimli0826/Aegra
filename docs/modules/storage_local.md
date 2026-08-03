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
├── include/aegra/adapters/storage_local/local_object_storage.h
└── src/
    ├── local_object_storage.cpp
    ├── local_staged_writer.cpp
    ├── local_storage_internal.h
    └── local_storage_paths.cpp
```

- Target：`aegra_adapter_storage_local` / `Aegra::AdapterStorageLocal`。
- 仅在 Windows 构建，只依赖 `Aegra::Ports`、C++ 标准库和 Windows SDK。
- 公共头不包含 `Windows.h`，平台句柄和实现状态通过私有 state 隔离。
- 禁止依赖 `personal_repository`、Memory Adapter、数据库、Archive Adapter 或 Application。

## 公共接口

`LocalObjectStorage::open()` 接受：

- `root`：本地绝对或可转换为绝对路径的 Repository 根；
- `LocalRootMode::kOpenExisting`：根不存在时返回 `kNotFound`；
- `LocalRootMode::kCreateIfMissing`：逐级创建缺失目录；
- `maximum_read_size`：单次 `read_range()` 的硬上限，必须大于零。

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

## 测试

`aegra_storage_local_tests` 必须复用 `tests/ports/object_storage_contract.h`，并额外覆盖：

- open-existing、create-if-missing 和 Adapter 重开；
- 完成 staging 后重开再发布、未完成 Session 析构清理；
- generation 跨重开稳定及外部文件修改后变化；
- `..`、内部保留前缀和 reparse-point 父目录拒绝；
- 只读文件删除失败且原文件保留。

符号链接测试仅在当前 Windows 环境允许创建 symlink 时执行断言；不具备 Developer Mode 或相应权限的 CI
必须由管理员安全集成测试补充 reparse-point 场景。空间不足、ACL 拒绝和进程崩溃注入属于后续 Windows
集成套件，不得宣称已由单元测试覆盖。

## Definition of Done

- Windows Debug/Release 使用 VS 2026 Insiders 构建并通过公共 Storage Contract。
- root/key/reparse-point/扩展路径边界与本文件一致。
- staging、发布、generation、删除、取消和 unknown outcome 语义有测试。
- Target 只依赖 Ports 和 Windows SDK，公共头不泄漏 Windows 类型。
- 函数、文件、格式、静态检查和秘密扫描通过。

## 当前状态

阶段 12C 已实现上述 Local Object Storage Adapter。Repository Scanner、Catalog Reconcile、Delete Plan 和
Tombstone 执行仍由后续 `personal_repository` 阶段实现。
