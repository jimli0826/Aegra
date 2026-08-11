# Explorer `.bkf` Shell Extension 开发计划（ES0-ES11）

| 属性 | 内容 |
| --- | --- |
| 状态 | Shell Extension on vendored MSF; password dialog + managed Incremental chain open implemented; Explorer manual matrix pending registration |
| 日期 | 2026-08-11 |
| 目标 | 在 Windows Explorer 中把 Aegra V7 `.bkf` 作为只读虚拟文件夹打开 |
| 权威 COM 框架 | 开源 MSF，已 vendoring 到 `third_party/msf`（`ShellFolderImpl` / `IEnumIDListImpl` / PIDL / RGS） |
| 参考实现 | 上游 msf_host 宿主模式；Aegra `ArchiveShellModel` 接 V7 Reader + NTFS Adapter |
| 支持范围 | `volume_set` 通过 NTFS 解析；`file_set` 通过 V7 File Index/Stream Reader |
| 明确排除 | Mount Host、Dokan、VHDX、盘符挂载、旧 Archive 格式兼容 |

## 1. 计划用途

本文是实现 Agent 的权威执行顺序和验收清单。Agent 必须按 ES0→ES11 推进；不得在前置 Gate 未通过时提前接入后续层，
不得为了演示绕过模块边界、输入校验、密码生命周期或生产构建门禁。

本文不直接批准架构例外。ES0 必须先形成并接受 ADR，正式替换当前
`MODULAR_ARCHITECTURE.md` 中“Shell Extension 只通过 IPC 请求 Mount Host”的决策。ADR 未接受前不得修改生产代码。

## 2. 产品行为

用户在 Explorer 中双击 `.bkf` 后，当前文件作为只读 Shell Folder 打开：

- `volume_set`：显示 Disk → Volume → NTFS 目录树；Volume 内容直接由 NTFS Parser 从 Archive 块流解析；
- `file_set`：显示 V7 tip File Index 中的目录树；文件内容由完整 base-first Chain Reader 读取；
- 两种类型共享同一套 PIDL、详情列、导航、`IDataObject`、`IStream`、文件复制和默认应用打开行为；
- Full、Incremental、加密和分卷 Archive 由 current V7 Reader 支持；
- 所有操作只读；不支持 Rename、Delete、New、Paste、写回或 Archive 修复；
- 不创建 Mount Session，不加载或调用 Dokan，不创建 VHDX，不分配盘符；
- 不支持 current V7 以外的 Archive、Catalog 或开发期格式，不增加 dual-read/fallback。

### 2.1 两类内容的权威读取路径

```text
Explorer / COM Shell Folder
        |
        v
ArchiveShellSession
        |
        +-- content_kind=volume_set
        |     -> PersonalArchiveChainReader
        |     -> PersonalArchiveVolumeRandomReader
        |     -> NtfsVolumeReader
        |     -> MFT / $I30 / unnamed $DATA
        |
        +-- content_kind=file_set
              -> PersonalFileArchiveChainReader
              -> Namespace / Entry ID / Stream / Chunk B+trees
              -> read_stream()
```

`file_set` 不包含 NTFS Boot Sector、MFT 或卷字节镜像，禁止把它伪装成 NTFS 输入。`volume_set` 必须经过 NTFS
Parser 浏览，禁止转向 Mount/Dokan 实现。分发只依据 V7 `content_kind`：`peek_archive_content_kind` 做路由，
随后**只**打开匹配的 volume_set 或 file_set reader；open 完成元数据认证并校验 manifest `content_kind`。
不得“先尝试一种，失败后再尝试另一种”。密码相关分支只依赖 `ErrorCode::kUnauthorized`（及是否提供空密码），
禁止解析英文错误文案。`IStream::Read` / NTFS `ReadFileData` 按 `kMaximumStreamReadBytes`（1 MiB）分块。

## 3. 设计不变量

1. Shell Extension 是 x64 in-process COM DLL，由 `explorer.exe` 加载；所有异常必须在 COM 边界转换为 HRESULT。
2. 生产实现使用 C++20、RAII、`Result<T>`、显式取消和有界缓存。
3. Shell Extension 是唯一 Composition Root；具体 Personal Archive 与 NTFS Adapter 只在该 App 中装配。
4. NTFS Adapter 只依赖 `base`、`ports`；输入为 `IRandomAccessReader`，不知道 `.bkf`、COM、路径或 Explorer。
5. Personal Archive Adapter 不依赖 NTFS Adapter；Adapter 之间不直接创建或引用具体实现。
6. `base`、`contracts`、`ports`、`format` 不增加 Windows、ATL、COM 或 NTFS 依赖。
7. 本地文件 open/read/write/seek/flush/close 只使用 Win32 API 和 RAII Handle；禁止 iostream 文件流。
8. 不使用业务全局单例、全局 Session Map 或全局密码缓存。COM Module 必需状态由 ES0 ADR 限定为最小例外。
9. 密码不进入 PIDL、注册表、日志、异常、临时文件名或长期缓存；敏感缓冲析构前清零。
10. PIDL 不保存指针、C++ 对象、Archive offset、密码或绝对 NTFS 路径。
11. 所有外部长度、offset、record size、runlist、page token 和 PIDL 数据先验证再使用；加法和乘法防溢出。
12. Explorer 关闭、Folder 释放或 Archive 被替换后，所有 Enumerator/Stream 在有界时间内失败并释放资源。
13. 不新增任何测试代码、测试 Target、CTest、fixture 或测试脚本；使用生产构建、静态检查和人工场景验收。
14. 不修改与本功能无关的 Desktop、Service、Worker 或 Mount 页面；保留工作树中全部既有用户修改。

## 4. 参考实现采用与拒绝项

### 4.1 采用的模式

- `.bkf` File Root/Folder Junction 注册；
- `IPersistFolder3 + IShellFolder2 + IEnumIDList` 的 Shell Namespace 结构；
- 自包含 PIDL、名称/大小/时间详情列、只读属性和默认 Open；
- `IDataObject + CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS + IStream` 的复制模型；
- `DllGetClassObject`、`DllCanUnloadNow`、`DllRegisterServer`、`DllUnregisterServer`；
- Archive Reader → Volume Reader → NTFS Parser 的直接读取链；
- 普通文件默认打开时物化到安全临时目录后调用系统关联程序。

### 4.2 禁止直接移植的实现

- `BackupShellModel::Instance()`、全局 session/password map；
- `std::ifstream`、`std::ofstream`、`std::fstream`；
- `bool` 吞掉错误原因的 Parser API；
- `ParseMFT()` 全卷扫描和无界 `unordered_map`；
- 以绝对 NTFS path 字符串作为 item identity；
- 不受限的临时文件释放和永不回收的明文 cache；
- 超过 Aegra 函数/文件/复杂度限制的源文件；
- 旧 `BackupReader`、旧 Archive ABI、旧格式探测。

COM 表面层使用仓库内 `third_party/msf`（header-only ATL 模板，源自 msf-main）。Archive 打开、枚举、读取必须走 Aegra V7 Adapter/Ports，不得链接旧 backup 引擎。

## 5. 目标模块与依赖

### 5.1 新增 Target

| Target | 类型 | 直接依赖 | 职责 |
| --- | --- | --- | --- |
| `aegra_adapter_ntfs` | STATIC | `Aegra::Base`, `Aegra::Ports` | 从随机访问卷视图只读解析 NTFS |
| `aegra_shell_extension` | SHARED DLL | Base、Contracts、Ports、Format、PersonalRepository、LocalStorage、PersonalArchive、NTFS、Crypto、Compression、Windows Shell libs | COM Composition Root 与 Explorer 行为 |

`aegra_shell_extension` 不得链接 Qt、Service、Worker、Application Service、Dokan 或 Mount Host。

### 5.2 预计目录

```text
src/
├── adapters/
│   └── ntfs/
│       ├── CMakeLists.txt
│       ├── include/aegra/adapters/ntfs/ntfs_reader.h
│       └── src/
│           ├── ntfs_boot_sector.cpp
│           ├── ntfs_mft_reader.cpp
│           ├── ntfs_record_parser.cpp
│           ├── ntfs_attribute_parser.cpp
│           ├── ntfs_runlist.cpp
│           ├── ntfs_directory_index.cpp
│           ├── ntfs_file_reader.cpp
│           ├── ntfs_fixup.cpp
│           └── ntfs_validation.cpp
└── apps/
    └── shell_extension/
        ├── CMakeLists.txt
        ├── resources/
        │   ├── aegra_shell_extension.def
        │   ├── aegra_shell_extension.rc
        │   └── backup.ico
        └── src/
            ├── dll_module.cpp
            ├── class_factory.cpp
            ├── registry.cpp
            ├── shell_folder.cpp
            ├── shell_item.cpp
            ├── enum_id_list.cpp
            ├── shell_folder_view.cpp
            ├── shell_data_object.cpp
            ├── file_content_stream.cpp
            ├── archive_shell_content.h
            ├── archive_shell_session.cpp
            ├── archive_chain_resolver.cpp
            ├── volume_ntfs_shell_content.cpp
            ├── file_set_shell_content.cpp
            ├── materialized_file_cache.cpp
            └── password_dialog.cpp
```

`personal_archive_volume_random_reader.cpp` 放入现有 `adapters/personal_archive`，因为它只负责 Archive Volume 的
随机读取视图，不知道 NTFS。

## 6. Agent 执行规则

每个 Agent/工作包开始时必须：

1. 读取 `AGENTS.md`、Aegra skill、工程规范、模块架构、受影响模块文档及本计划；
2. 检查 `git status --short`，保留所有已有用户修改；
3. 只修改工作包列出的文件范围；跨范围需求先更新计划或移交下一工作包；
4. 先记录基线 Target 能否构建，再开始修改；
5. 不新增测试代码；人工样本只能使用现有生产程序生成，不能提交 fixture；
6. 完成后构建列出的生产 Target、运行 `CheckSourceLimits` 和 `git diff --check`；
7. 报告实际验证结果，不把未执行的人工场景写成通过；
8. 不以“后续再修”为由提交反向依赖、无界缓存、明文密码、宽松 parser 或兼容分支。

并行 Agent 只能领取文件所有权不重叠且前置条件已完成的工作包。ES2 的 NTFS Adapter 内部 translation unit 可并行，
但公共头、解析不变量和错误模型必须先由一个 owner 冻结；ES5/ES6 不得与公共 PIDL 格式并行修改。

## 7. 工作包总览

| ID | 优先级 | 工作包 | 前置 | 主要文件所有权 | 交付 Gate |
| --- | --- | --- | --- | --- | --- |
| ES0 | P0 | ADR、架构与产品范围 | 无 | docs/adr, docs/architecture, docs/modules | ADR Accepted |
| ES1 | P0 | Explorer 注册/COM 最小 Spike | ES0 | apps/shell_extension skeleton | `.bkf` 显示固定虚拟项 |
| ES2 | P0 | NTFS Adapter | ES0 | adapters/ntfs | 生产工具/手工样本可列 NTFS 根 |
| ES3 | P0 | Volume RandomAccessReader | ES0 | adapters/personal_archive | 任意 offset 卷读取人工核对 |
| ES4 | P0 | Archive Session 与链解析 | ES1, ES3 | shell session/resolver | 两种 content kind 正确分发 |
| ES5 | P0 | 统一 PIDL/Shell Folder/Enumerator | ES1, ES4 | shell item/folder/enum | 两种 provider 共用导航表面 |
| ES6 | P0 | `file_set` Provider | ES4, ES5 | file_set_shell_content | Full/Incremental 文件树可浏览 |
| ES7 | P0 | `volume_set` NTFS Provider | ES2, ES3, ES5 | volume_ntfs_shell_content | Disk/Volume/NTFS 树可浏览 |
| ES8 | P1 | IStream、复制和默认打开 | ES6, ES7 | stream/data object/cache | 文件可复制和只读打开 |
| ES9 | P1 | 加密、分卷与生命周期收口 | ES6, ES7, ES8 | session/password/cache | 错误密码/缺层/缺卷安全失败 |
| ES10 | P1 | 安全、性能、错误和国际化 | ES9 | shell extension/ntfs/docs | 边界矩阵通过 |
| ES11 | Gate | 发布验证与文档完成 | ES10 | CMake/docs | Debug/Release 与人工验收记录 |

## 8. ES0：ADR、架构与产品范围

### 8.1 修改范围

- 新增 `docs/adr/0023-in-process-explorer-archive-browsing.md`；
- 新增 `docs/architecture/EXPLORER_ARCHIVE_BROWSING.md`；
- 更新 `docs/architecture/MODULAR_ARCHITECTURE.md`；
- 更新 `docs/modules/apps.md`、`docs/modules/adapters.md`、`docs/modules/README.md`；
- 更新产品限制与错误码文档。

### 8.2 ADR 必须冻结

- Shell Extension 从 IPC-only 改为 in-process Archive/NTFS/File Index 浏览；
- `volume_set` 必须走 NTFS Parser，`file_set` 必须走 V7 File Index；
- Shell Extension 直接链接的模块和明确禁止的依赖；
- Explorer 稳定性风险、COM Module 最小全局状态例外及缓解；
- 密码提示、密钥生命周期和临时文件策略；
- Repository Catalog 链解析与 standalone Archive 规则；
- 只支持 current V7，无旧格式兼容；
- x64-only 首版和 Windows 版本范围；
- NTFS compressed/EFS/reparse/ADS 的首版行为；
- 注册、升级和卸载所有权。

### 8.3 Gate

ADR 状态必须为 Accepted；文档不能同时保留“只通过 Mount Host”和“进程内解析”两种权威说法。

## 9. ES1：Explorer 注册与 COM 最小 Spike

### 9.1 目标

先验证 `.bkf` File Root/Folder Junction 在目标 Windows/Explorer 上可用，不连接 Archive Reader。

### 9.2 实现

- 新建 x64 `aegra_shell_extension` SHARED Target；
- 实现 ATL 或最小原生 COM module、class factory 和 DLL 四个标准导出；
- 实现 `IPersistFolder3`、`IShellFolder2`、固定 `IEnumIDList`；
- 注册独立 Aegra CLSID/ProgID、`.bkf` junction、默认图标和 `ThreadingModel=Apartment`；
- `DllRegisterServer`/`DllUnregisterServer` 只修改 Aegra 自己的 HKCU keys；
- 枚举一个固定只读项，用于确认导航、卸载和 Explorer 重启行为。

### 9.3 禁止

- 不接 Personal Archive、NTFS、密码或 Repository；
- 不复制参考项目的 CLSID；
- 不依赖输出目录偶然存在的 DLL；
- 不修改系统级关联或删除其它产品关联。

### 9.4 验收

- x64 Debug/Release 单 Target 构建；
- 当前用户注册后 `.bkf` 可在 Explorer 内进入；
- 反注册后 Aegra CLSID/ProgID 清理完整；
- 重启 Explorer 后可再次加载；
- `DllCanUnloadNow` 与 COM 引用计数行为正确。

## 10. ES2：NTFS Adapter

### 10.1 公共合同

`NtfsVolumeReader::open(IRandomAccessReader&, CancellationToken)` 借用 reader；reader 必须比 NTFS Reader 长寿。
一个实例不是线程安全对象，调用者串行化；返回 DTO 不含 HANDLE、COM、ATL 或 Archive 类型。

能力至少包括：

- `volume_info()`；
- `list_directory(file_reference, maximum_results, continuation, cancellation)`；
- `describe_entry(file_reference, cancellation)`；
- `read_file(file_reference, offset, destination, cancellation)`。

### 10.2 解析范围

- Boot Sector geometry、OEM、sector/cluster/MFT record 大小；
- Update Sequence Array fixup；
- `$MFT` resident/non-resident layout；
- FILE record header、in-use、sequence 和 base record；
- `$STANDARD_INFORMATION`、`$FILE_NAME`、unnamed `$DATA`；
- resident/non-resident data；
- signed runlist delta、sparse run 和范围合并；
- `$ATTRIBUTE_LIST` extension records；
- `$INDEX_ROOT`、`$INDEX_ALLOCATION`、INDX fixup 和 `$I30`；
- stale file reference/sequence rejection；
- root MFT record 5；
- UTF-16 名称原样保留，不做 UTF-8 损失转换。

### 10.3 强制边界

- 每个结构在字段读取前验证最小大小、对齐、offset+length 和 record 边界；
- run count、attribute count、extension depth、index depth、record size 和单次 buffer 有产品上限；
- MFT/Index 使用固定容量 LRU，禁止全量 `ParseMFT()`；
- sparse range 零填充；重叠、回退、越卷或算术溢出视为损坏；
- reparse item 不跟随；named ADS 不作为普通文件；
- compressed/EFS 返回稳定 unsupported，不返回错误明文数据；
- 不使用 packed struct reinterpret-cast 直接信任磁盘数据，字段通过边界安全读取函数解析。

### 10.4 人工验收

使用现有生产备份流程生成临时 NTFS `volume_set` Archive，人工核对：root、普通目录、resident 文件、fragmented
文件、空文件、Unicode、sparse、attribute list；对截断 Boot/MFT/INDX/runlist 的临时副本确认稳定失败。样本不提交仓库。

## 11. ES3：Personal Archive Volume RandomAccessReader

### 11.1 实现

新增 `PersonalArchiveVolumeRandomReader : IRandomAccessReader`：

- 构造输入为已打开的 `IRecoveryPointReader`、认证 Manifest 和 `volume_index`；
- 将该 Volume 的 Chunk Descriptor 建成有序 locator；
- `read_at` 支持跨 Chunk 任意 offset 读取；
- FREE/未映射区零填充；
- 使用固定容量解压后 Chunk cache；
- short-read 仅允许 EOF；
- 校验 descriptor overlap、gap 规则、source index、logical size 和卷上限；
- 不知道 NTFS，不依赖 `aegra_adapter_ntfs`。

### 11.2 性能注意

初始实现可以在 open 时扫描 Chunk Descriptor，但必须记录大 Archive 成本。若 descriptor 数量导致不可接受的 O(N)
打开或常驻内存，Agent 必须先提出格式/Reader 索引设计，不得隐藏无界行为。不得为此新增开发期格式兼容路径。

### 11.3 验收

- 从卷首、Chunk 边界、跨 Chunk、EOF 前后读取并与恢复输出人工比对；
- Incremental Chain Reader 上读取与 Full 最终视图一致；
- 缺 Chunk、短 payload、错误 source index 和取消返回稳定错误。

## 12. ES4：Archive Session 与链解析

### 12.1 `ArchiveShellSession`

Session 拥有：

- Archive 文件 identity、路径和取消源；
- base-first Archive Reader/Chain Reader；
- 已认证 Manifest；
- `IArchiveShellContent` concrete provider；
- Session-local secure credential state；
- provider/cache 生命周期。

Folder、Enumerator、Stream 可以通过 `shared_ptr` 共享 Session；不得形成 COM/Session 引用环。

### 12.2 Archive 路径规则

- 仅接受 Explorer junction 提供的规范化普通文件路径；
- 拒绝 device namespace、目录、reparse escape 和非 `.bkf` 主文件；
- 使用 Win32 file identity、size、last-write 检测浏览期间替换；
- 分卷由 current Reader 按 current V7 contract 解析；
- 不读取或解释旧 Header/version。

### 12.3 链解析

- standalone Full：允许单层打开；
- standalone Incremental：返回 `shell.parent_missing`；
- 受管理 Local Repository：从 Archive 向上有界定位 `aegra.repository`，验证所选路径确实对应 Catalog
  `archive_main_key`，再用 `RecoveryPointGraph::resolve_chain()` 解析 base-first 链；
- 禁止扫描同目录并按文件名/时间猜父层；
- 禁止直接访问 SQLite/Service 控制面；
- Catalog、Header 和链 identity/content kind 不一致时失败关闭。

### 12.4 Content 分发

- `volume_set` → `VolumeNtfsShellContent`；
- `file_set` → `FileSetShellContent`；
- unknown/current-invalid content kind → stable unsupported/corrupt；
- 不实现探测 fallback。

## 13. ES5：统一 PIDL、Shell Folder 与 Enumerator

### 13.1 PIDL v1

PIDL 固定头包含 magic、version、kind、payload size、attributes、logical size 和时间。Kind：

```text
Disk
Volume
NtfsDirectory
NtfsFile
FileSetDirectory
FileSetFile
StatusItem
```

Kind payload：

- Disk：`disk_number`；
- Volume：`volume_index`；
- NTFS：`volume_index + MFT record number + sequence number`；
- File Set：`entry_id + stream_index`；
- 所有可见项附带有界 UTF-16 display name。

PIDL 由 `CoTaskMemAlloc` 分配，大小、对齐、UTF-16、kind/payload 对应关系必须严格验证。PIDL version 不匹配直接拒绝，
产品未发布不保留旧 PIDL parser。

### 13.2 Shell Folder

- `CreateEnumIDList` 调用统一 provider `list_children`；
- `BindToObject` 只允许目录 kind；
- `GetAttributesOf` 强制只读，文件夹/文件能力准确；
- `CompareIDs` 文件夹优先，再按名称和稳定 identity；
- `GetDisplayNameOf/GetDetailsOf` 只使用已验证 PIDL/descriptor；
- Rename/Delete/Paste/Drop 写入返回 access denied/not implemented；
- status item 只用于在根显示可操作的稳定错误，不伪装成可读文件。

### 13.3 Enumerator

- 惰性分页，不预取整个目录；
- 持有独立 continuation 和 current-page cursor；
- 遵守 `SHCONTF_FOLDERS`、`SHCONTF_NONFOLDERS`、`SHCONTF_INCLUDEHIDDEN`；
- Clone 复制枚举位置和 Session；
- 取消/Archive changed/Session closed 后稳定失败。

## 14. ES6：`file_set` Provider

### 14.1 打开

- Full 使用 `PersonalFileArchiveReader`；
- Incremental 使用 `PersonalFileArchiveChainReader`；
- tip Index 为唯一 namespace；parent 层只用于 stream 内容解析；
- 打开认证 Header/Footer/root pages，不触发完整 Verify。

### 14.2 映射

- root → `parent_entry_id=0`；
- directory → `entry_id`；
- file → `entry_id + unnamed main stream_index`；
- `list_children` 原样使用 Reader continuation token；
- 名称从 `EncodedName` 保持 UTF-16；
- 大小、attributes、creation/access/write/change time 来自 `FileEntryDesc`；
- 空文件无 extent，读取立即 EOF；
- directory 必须无 stream；普通文件必须符合 current V7 单 unnamed main stream contract。

### 14.3 读取

`read_file` 先按 `entry_id` describe，固定 main stream identity，再调用 Chain Reader `read_stream`。不得相信 PIDL
缓存的 stream index 作为唯一权威；Archive identity/index digest 变化必须使 Session 失效。

### 14.4 验收

- Full/Incremental 根和深目录分页；
- local/parent stream、空文件、Unicode 和大文件随机 seek；
- 缺父层、错误 parent stream、损坏访问页在实际访问时稳定失败；
- 普通 browse 不执行全量 `verify_recoverability`。

## 15. ES7：`volume_set` NTFS Provider

### 15.1 层级

```text
Archive root -> Disk -> Volume -> NTFS root -> directories/files
```

- Disk/Volume 来自已认证 Manifest，不从 NTFS 猜测；
- Volume filesystem 明确为 NTFS 时才创建 Parser；
- ReFS/FAT/exFAT/RAW Volume 显示为不可进入的 status/unsupported item；
- Volume Reader 的 offset 0 对应该卷 Boot Sector，不使用整盘物理 offset；
- NTFS root 使用 file reference 5。

### 15.2 映射

- NTFS directory → MFT reference + sequence；
- NTFS file → MFT reference + sequence；
- 名称、大小、attributes 和时间来自已验证 NTFS record/index；
- 进入目录时重新校验 file reference，拒绝 stale sequence；
- hidden/system 行为依据 Explorer flags，不擅自过滤产品认为敏感的普通系统文件；
- reparse 不跟随，不允许 namespace escape。

### 15.3 验收

- 单/多 Disk、单/多 Volume；
- NTFS root、深目录、大目录和 Unicode；
- Full/Incremental final block view；
- 非 NTFS Volume 有清晰状态且不调用 Parser；
- 不产生 Dokan/VHDX/盘符/overlay 文件。

## 16. ES8：IStream、复制和默认打开

### 16.1 `IStream`

同一实现同时服务两种 provider：

- `Read` 调用 provider `read_file(identity, offset, span)`；
- `Seek` 支持 SET/CUR/END，检查 signed delta 和 u64 溢出；
- `Stat` 返回只读 stream、名称和大小；
- `Clone` 共享 Session/item，复制独立 cursor；
- `Write`/`SetSize`/写锁返回 `STG_E_ACCESSDENIED`；
- 每次 Reader 调用有固定 buffer 上限，短读只允许 EOF；
- HRESULT 映射保留稳定 message code 供 UI 错误展示。

### 16.2 Explorer 复制

- 实现 `IDataObject`；
- 提供 `CFSTR_FILEDESCRIPTORW` 和 indexed `CFSTR_FILECONTENTS`；
- 多文件 selection descriptor 顺序稳定；
- folder 递归复制不进入首个里程碑，未实现时明确拒绝，不静默只复制一层；
- 不向 Explorer 暴露 Archive 临时物理路径。

### 16.3 默认打开

- 文件由 provider 流式写入 `%LOCALAPPDATA%\Aegra\ShellCache\<session>\<entry>`；
- 目录和文件使用随机/session identity，不拼 Archive 相对路径；只保留安全 leaf extension；
- Cache 目录限制为当前用户 ACL；
- Win32 create/write/flush/close，临时名完成后原子 rename；
- 写入前检查文件大小、剩余空间、单 Session 和全局 Cache 配额；
- `ShellExecuteExW` 后保留 TTL lease，过期/下次启动清理；
- 失败、取消和异常路径删除未发布临时文件。

## 17. ES9：加密、分卷与生命周期

### 17.1 密码

- 首次用空密码打开；Unauthorized 时显示 Shell-owned password dialog；
- password edit control 禁止系统明文回显；
- 密码存入可清零缓冲，只在同步 Reader open 调用期暴露 view；
- Session 持有 credential；另有 **进程内** path→password 表（对齐 backup `g_passwordCache`），供 Explorer 路径变体创建的新 Session 复用；不写盘、Cancel/错误时 SecureZero 清除；
- 错误密码允许重试，Cancel 映射 `ERROR_CANCELLED`；
- 首版同一链要求同一密码。若产品要求逐层不同密码，必须先更新 ADR/API，不得在 Agent 内临时增加密码数组 UI。
- 调试：`%TEMP%\aegra_shell_extension.log` 与 `OutputDebugString`（`[aegra_shell]`），禁止记录密码明文。

### 17.2 分卷和链

- 主 `.bkf`、part 和 Footer 规则完全委托 current Reader；
- 缺 part → `shell.split_incomplete`；
- Catalog 链缺层 → `shell.parent_missing`；
- content kind/backup set/parent UUID 不一致 → corrupt/conflict；
- 不按文件名猜 part/parent，不支持旧命名 fallback。

### 17.3 生命周期

- Root Folder 创建 Session；subfolder/enumerator/stream 共享；
- Session cancellation 在最后引用释放或 Archive changed 时触发；
- 不持锁调用 COM callback、ShellExecute 或未知代码；
- Explorer 进程退出由 RAII 关闭 Handle、Reader 和安全缓冲；
- COM object、Session、Stream 和 DataObject 无引用环；
- DLL 仅在 module lock 与对象引用均为零时允许卸载。

## 18. ES10：安全、性能、错误与国际化

### 18.0 Shell UI 语言

- 按 `GetUserDefaultUILanguage()` 选择 UI 语言（与 backup ShellExtension 一致）；
- 支持：`en`、`zh-CN`、`zh-TW`、`ja`、`de`；未知语言回退 English；
- 实现：`shell_strings.cpp` 字符串表（Unicode 转义，源文件 ASCII 安全）；密码框、列标题、错误提示、Disk/Volume 显示名与类型名均走 `load_shell_string`；
- 普通文件/文件夹类型名优先 `SHGetFileInfo(SHGFI_TYPENAME)`（跟随系统关联语言），失败时回退字符串表；
- 业务分支仍使用稳定 message code / `ErrorCode`，不解析本地化文案。

### 18.1 产品上限建议（ES0 最终冻结）

| 项目 | 初始上限 |
| --- | ---: |
| 单次目录页 | 256 items |
| 单次 stream read | 1 MiB |
| NTFS record | 64 KiB |
| NTFS index record | 1 MiB |
| attribute list extension depth | 32 |
| NTFS path/navigation depth | 256 |
| NTFS MFT record cache | 1024 records |
| NTFS index page cache | 256 records |
| materialized single file | 由可配置配额限制，默认 16 GiB |
| Shell Cache total | 默认 32 GiB |
| standalone repository-parent walk | 16 ancestors |

这些是配置可收紧的产品上限，不得超过 current Archive/contract 上限。Agent 必须在 ADR/产品限制文档中确认实际值。

### 18.2 稳定 message code

```text
shell.archive_not_found
shell.archive_changed
shell.unsupported_version
shell.unsupported_content_kind
shell.password_required
shell.password_invalid
shell.password_cancelled
shell.parent_missing
shell.split_incomplete
shell.archive_corrupt
shell.volume_not_found
shell.filesystem_not_ntfs
shell.item_not_found
shell.read_failed
shell.cache_limit
shell.cache_write_failed
ntfs.invalid_boot_sector
ntfs.invalid_geometry
ntfs.corrupt_mft_record
ntfs.fixup_failed
ntfs.attribute_out_of_bounds
ntfs.runlist_corrupt
ntfs.index_corrupt
ntfs.compressed_unsupported
ntfs.efs_unsupported
ntfs.entry_not_found
ntfs.read_failed
```

Agent 应复用已有通用 format/file_recover 错误；只有 Shell/NTFS 边界需要新稳定码。不得根据英文错误文本做业务分支。

### 18.3 性能目标

- `file_set` open 保持 current Reader O(1) root authentication，不全量扫描 Index；
- `file_set` list 保持 O(log N + K)；
- NTFS Volume 首次打开只读 Boot/MFT 必要记录，不全量扫描 MFT；
- 目录枚举只读目标 `$I30` 路径；
- 所有 cache 固定容量并可在内存压力下清退；
- Shell call 不执行无界循环；分页和 read 均可取消；
- 性能结论以人工 trace/profiling 记录为准，不凭感觉声明达标。

### 18.4 安全审查

- Shell DLL 的直接/延迟 DLL 依赖可控，第三方 DLL 不从当前目录或 Archive 目录搜索；
- COM/PIDL/NTFS/Archive 四个外部输入边界均 fail closed；
- 无秘密、文件内容、security descriptor 或大 payload 日志；
- Cache ACL、路径规范化、设备名、separator、ADS 注入和 reparse escape 全部审查；
- Parser 深度/计数/大小上限阻止恶意 Archive 导致 Explorer OOM/长时间挂起；
- 所有 C++ 异常在 DLL export/COM method 边界转换。

## 19. ES11：发布验证

### 19.1 构建

使用固定工具链：

```text
C:\Program Files\Microsoft Visual Studio\18\Insiders
```

至少执行：

```powershell
scripts\build.cmd Debug
scripts\build.cmd Release
cmake -DAEGRA_SOURCE_ROOT=D:/Work/OpenSource/Aegra -P cmake/CheckSourceLimits.cmake
git diff --check
```

分阶段直接 Target：

```text
aegra_base
aegra_ports
aegra_format
aegra_personal_repository
aegra_adapter_storage_local
aegra_adapter_personal_archive
aegra_adapter_ntfs
aegra_shell_extension
```

### 19.2 静态检查

- 搜索 Shell/NTFS/PersonalArchive 新代码中的 `ifstream|ofstream|fstream|filebuf`；
- 检查 adapters 间无具体实现依赖；
- 检查 Shell Extension 不链接 Service/Qt/Dokan/Mount Host；
- 检查函数、lambda、嵌套、参数和文件行数；
- 检查 COM/C ABI 未暴露 STL、异常、RTTI 或 vendor object；
- 检查无旧格式/version/schema alias/fallback；
- 检查密码、SecretRef、file content 无日志路径；
- 检查所有新增英文生产标识符/注释符合命名规则。

### 19.3 人工矩阵

| 维度 | 必须覆盖 |
| --- | --- |
| Explorer | Windows 10/11 x64、Details/List、前进后退、地址栏、刷新、Explorer restart |
| 注册 | register、重复 register、unregister、卸载后无孤儿 CLSID/ProgID |
| `file_set` | Full/Incremental、local/parent stream、空/大/Unicode、深/大目录、损坏访问页 |
| `volume_set` | Full/Incremental、单/多 Disk、单/多 NTFS Volume、resident/non-resident、fragmented、sparse |
| 非 NTFS | ReFS/FAT/exFAT/RAW 清晰 unsupported，不进入 NTFS Parser |
| Archive | 加密/未加密、密码错误/取消、分卷完整/缺 part、缺 parent、Archive browse 中被替换 |
| I/O | 顺序 read、随机 seek、跨 Chunk/run、EOF、复制、取消、磁盘空间不足 |
| Cache | 原子发布、ACL、配额、TTL、异常残留清理、同名文件隔离 |
| 损坏输入 | Boot、MFT、USA、attribute、runlist、INDX、File Index、stream/chunk 引用 |
| 生命周期 | 关闭 Tab、释放 Folder、Stream 存活、DLL unload、Explorer crash/restart |

临时损坏样本和人工操作记录放在交付说明或外部临时目录，不提交仓库 fixture。

## 20. Agent 每工作包交付模板

Agent 最终报告必须包含：

```text
工作包：ESn
修改文件：
生产 Target 构建：Debug/Release + 结果
静态/架构检查：命令 + 结果
人工验证：实际执行场景 + 结果
未执行场景：原因和所需环境
标准例外：ADR/文档依据；无则写“无”
工作树保护：确认未覆盖既有用户修改
下一工作包前置是否满足：是/否 + 证据
```

禁止只报告“代码已完成”而不列构建和人工验证证据。

## 21. 最终 Definition of Done

- 双击 current V7 `.bkf` 可在 Explorer 中作为只读虚拟文件夹打开；
- `volume_set` 经 Archive Chain → Volume Random Reader → NTFS Parser 浏览和读取；
- `file_set` 经 File Archive Chain → V7 File Index/Stream 浏览和读取；
- 两种类型共享一致的 Shell navigation、详情、复制和默认打开体验；
- 全流程不使用 Mount Host、Dokan、VHDX、盘符或 overlay；
- Full/Incremental、加密和分卷行为符合 current V7/Catalog V2；
- 非 NTFS Volume、损坏输入、缺层、缺 part、错误密码和 Archive replacement 均 fail closed；
- 无业务全局单例、无明文密码缓存、无 iostream 文件 I/O、无无界 Parser cache；
- COM/DLL ABI 无 STL/异常泄漏，所有对象和 Handle 由 RAII 管理；
- Debug/Release 全量生产构建、源码限制、依赖、安全和 `git diff --check` 通过；
- 人工验收矩阵有真实记录，未执行项明确标注；
- ADR、架构、模块、产品限制、错误码和发布状态文档与实现同步；
- 未覆盖任何与本功能无关的用户工作树修改。
