# `adapters` 模块开发文档

## 目标

实现 `ports` 定义的外部能力。每个 Adapter 独立 Target，只依赖所实现的 Port、必要 Contracts 和自己的第三方库。

## Adapter 分类

- Storage：Local、SMB、S3、Azure。
- Windows：Disk、Volume、VSS、Partition、BCD/WinRE。
- 控制面：PostgreSQL（企业版）、个人版 SQLite 控制面（`Aegra::AdapterSqlite`）。
- 虚拟化：VMware、Hyper-V。
- 挂载：Dokan 与虚拟磁盘格式呈现（Mount Host）。
- NTFS：只读 `IRandomAccessReader` 卷视图解析（Shell Extension `volume_set` 浏览）。
- 传输：HTTP/gRPC/Named Pipe。
- 内存参考实现：Memory Block Source/Sink、Memory Backup Session/Reader。
- 格式组合：个人版 `.bkf` Session/Reader。
- 数据转换：libsodium metadata crypto、Zstandard block compression。

Memory Adapter 是正式的端口参考实现，只保存进程内临时数据，不定义持久化格式。除 Block Source/Sink 和
Backup Session 外，阶段 12B 已实现线程安全 `MemoryObjectStorage`：分别实现对象 Reader、Staged Writer、
Prefix Enumerator、Publisher、Deleter 和 capability Port，支持短读、分页、generation、条件冲突、取消、
RAII Abort、幂等删除及 publish/delete unknown outcome 故障注入。

阶段 12C 已实现 Windows `Aegra::AdapterStorageLocal`。它使用规范化扩展路径、逐级 reparse-point 检查和
最终文件句柄复核保护 Repository 根；partial 写入位于保留的 `.aegra-internal/writes`，完成后才移动到公开
staging key。发布支持 create-only rename 与 generation 条件替换，删除绑定已校验文件句柄。详细接口、
并发和崩溃语义见 [Windows Local Object Storage Adapter](storage_local.md)。

个人版 Archive Adapter 组合 `format`、`IBackupSession`、`IRecoveryPointReader`、libsodium 和 Zstandard。当前全量数据面支持一个 Archive 包含多个 volume，并可在完整 chunk 边界透明分卷；每个 chunk 通过 `source_index` 归属一个 Manifest Volume，Sidecar 为每个 Volume 保存独立块表。写入期间所有分卷与 Sidecar 使用 partial 路径，全部 Volume 完整写入且 Footer 和加密 Sidecar 均写完后，先发布 Sidecar/续卷，最后发布首卷；任一 Volume 失败时 Abort 和析构清理本次创建的 partial 文件。Reader 在解析 CBOR 前必须完成 Header/Envelope 范围校验和 AEAD 认证，随后发现并验证连续分卷和每个 Volume 的完整覆盖。普通恢复不依赖 `.bhx`；增量比较通过显式 API 加载并认证 Sidecar。

ADR-0022 为 volume_set 冻结单物理 `VolumeChunk` 去重。Archive Session 在逻辑序组装阶段维护当前 Chunk
SHA-256 候选表，命中后逐字节确认，并只回指更早的 RAW/COMPRESSED canonical；Chunk/part/source 切换时
清空窗口。FREE、ZERO 与 Incremental parent omission 先处理，canonical 后执行 zstd 与 AEAD。FREE
来自文件系统空闲簇或 pagefile/hiberfil/swapfile 排除 extent，不读取、不散列、不保存 payload；Reader
认证后将 FREE range 交给 Restore Pipeline 跳写。Reader/Verify 在
认证当前 record 后解析 DEDUP，拒绝前向、越界、链式和长度不匹配引用。哈希不进入 Archive、Sidecar 或日志。

`PersonalArchiveSession` 拥有持久 `BlockWorkerPool`：session 生命周期内固定 worker 线程，跨物理
Chunk 复用；每个 worker 持有可复用的 Windows CNG SHA-256 provider/hash object，以及
`ZstdCompressor`（`ZSTD_CCtx` + scratch），避免 per-block provider/context 创建。CNG 保持标准 SHA-256
摘要与 Sidecar/增量比较语义不变，并由 Windows CNG provider 自动选择平台硬件加速。VolumeChunk 使用 detached XChaCha20-Poly1305 原地加密 payload，
避免同时保留等长 plaintext/ciphertext 缓冲；nonce、AAD、tag 和线格式不变。Session 仍是单 writer；
调用线程完成当前 Chunk 的 hash/zstd/组装后，通过深度受限的顺序交接把 Prepared Chunk 交给 session 级
persist worker；persist worker 独占加密、分卷状态和 Win32 输出句柄，使当前 Chunk 的 WriteFile 与下一
Chunk 的 hash/zstd 重叠。交接要求前一 persist 完成后才能接受下一 Prepared Chunk，因此最多同时持有一个
正在写入和一个正在准备的 Chunk；commit 等待 persist 清空，abort 停止并 join worker 后再删除 partial。
pool 同时只服务一次 `parallel_for`。`PersonalArchiveReader` 同样在 reader 生命周期复用该 pool，Archive
chain 的所有底层 Reader 共享一个 pool，线程数不随链深增长；每个 worker 持有 `ZstdDecompressor`
（`ZSTD_DCtx`）。Reader 在生命周期内保持各 Archive part 的 Win32 顺序读取句柄打开；顺序恢复可显式
启用深度为一的 payload 预读，使下一 Chunk 的存储读取与当前 Chunk 的认证、解压重叠，随机读取默认
不启用预读。Volume/File Archive、分卷、sidecar、secondary index 和 index spool 的生产写路径统一使用
持久 Win32 顺序输出句柄；Volume Chunk 的 prefix/header/BlockEntry 批量合并写入，payload 单独大块写入，
不使用 `std::ofstream`。
Reader 先完整认证并原地解密 VolumeChunk payload，
再把独立 RAW/COMPRESSED canonical 并行写入最终逻辑 Chunk 的预验证、不重叠范围，最后顺序展开 DEDUP；
不再为每个压缩 block 分配临时输出或复制到最终 Chunk。nonce、AAD、tag 与恢复输出语义不变。

V7 `file_set` 由 `PersonalFileArchiveSession`（`IFileBackupSession`）与 `PersonalFileArchiveReader`
（`IFileRecoveryPointReader`）实现：entry 先写入 index spool，finalize 时写出 leaf index page 与 Footer；
Index page 使用独立 HKDF info `MYBACKUP-V7-FILE-INDEX-PAGE`。File stream chunk 默认
`COMPRESSION_ZSTD`：写入时对逻辑 block 做机会性 zstd（压得更小才标 `COMPRESSED`，否则 `RAW`）；
读取时按 BlockEntry flags 解压。Writer 支持 Full 与 Incremental：
- Full：`parent_uuid=0`，全部 stream 必须 `content_storage=local`；
- Incremental：`parent_uuid` 非 0、`CAP_FILE_METADATA_BASELINE` 置位、Manifest 含 fingerprint 与
  `change_detection_method=mtime_size_v1`；tip Index 完整，metadata signature 未变的 stream 仅写
  `parent_stream_index`（无 payload）；
- Footer：`entry_count`/`stream_count` 为 tip 全集；`file_stream_chunk_count`/`logical_bytes` 仅本层 local payload。
Abort/析构删除 partial 与 spool，不发布可见 RP。Writer finalize 对 spool 做紧凑 key 排序并流式写
leaf（M5）。Writer finalize 写出 Namespace 树及 Entry ID / Stream / Chunk 二级索引（ADR-0019），
internal child 含物理 `offset`；Entry ID leaf 记录含 Namespace leaf `page_offset` 以便
`describe_entry` 直接 seek。Reader open 认证 Header/Footer 与各非零 root（O(1) 页），
有界 LRU page cache（L31 Phase-2），**不**建 O(N) locator 表。`list_children` 沿
Namespace B+tree；`describe_entry` / `describe_stream_owner` / `read_stream` 经二级索引
O(log N) 定位后按需加载 leaf/chunk。Chain 非 tip `kDeferred`（M6）；全量父图与 Entry ID
唯一性仅在 `verify_index_and_parent_graph`（Verify 路径）。

**FI5/FI8 file chain reader：** `PersonalFileArchiveChainReader` 接受 base-first 层列表（Full root…tip），
在 open 时认证每层 Header/Footer（密码在 open 期间消费）并校验 parent_uuid、backup_set、
selection fingerprint、无环与深度 ≤ 128。tip 立即认证 Index roots；祖先层延迟到首次 stream 访问。
browse/`describe_entry` 只暴露 tip Index；`read_stream` 迭代解析 `content_storage=parent` 到最终
local 层，逐跳校验 stream kind / logical size，并维护 visited 防环。
`chain_generation_digest()` 为 base-first 各层 Index root digest 的 `+` 拼接（digest 来自 Footer，
不依赖 deferred root 认证）；`resolve_stream_reference` 仅解析 parent 引用不读 payload。
单层 `PersonalFileArchiveReader::read_stream` 拒绝 parent storage。
`verify_recoverability` 对每层跑父图校验、按 leaf 顺序认证 local payload，并解析 tip 全部 stream。
Service browse/restore/verify 与 Worker file_set Verify/Restore 均经 chain reader（单 Full 为一层链）。

增量 Session 接受一或多个 Volume：创建时验证显式父 Archive 及父 Sidecar，要求父层与本次 Manifest 的有序 `volume_index` / `volume_id` / `total_size` 及 Sidecar 每卷块记录数一致，继承备份集 UUID，并把各 Volume 的完整源转换为连续变化区间组成的稀疏层；新 Sidecar 仍为每个 Volume 保存完整状态。`PersonalArchiveReader` 可以 inspect 稀疏层，`PersonalArchiveChainReader` 接受显式 base-first 层列表、校验多 Volume 几何与链关系，并按 `source_index` 叠层，对通用 Restore Pipeline 提供连续覆盖视图。多 Volume **同时写多个独立目标**的显式映射尚未完成：Restore Pipeline 在未提供映射时对 `source_index != 0` 的 chunk 拒绝。链恢复不读取 Sidecar；链发现和逐层凭据选择属于 Application，不由 Adapter 扫描目录猜测。

SMB、S3 和 Azure Storage Adapter 还必须实现个人版 Repository 所需的细粒度对象 Port。Adapter
负责 Repository 相对 key 到本地路径或对象 key 的安全映射，并显式报告原子 rename、条件创建和列举
一致性 capability；不得把 URI 判断、临时发布或删除重试逻辑泄漏到 Application。具体契约见
[个人版 Repository 模块](personal_repository.md)。

Archive Reader 分别限制 metadata、chunk stored payload 和展开后的 chunk logical size。ZERO/FREE run、压缩块、
DEDUP 引用和其它稀疏表示在分配恢复缓冲区前都必须通过 logical size 与引用图上限检查。

密码 Adapter 使用 Argon2id v1.3 派生 master key、HKDF-SHA256 分离 metadata、Chunk Payload、File Index
page 和 Sidecar key，并使用 XChaCha20-Poly1305 detached tag；内容散列使用 SHA-256。每个 Chunk 使用独立
随机 nonce，Header 和 BlockEntry 作为 AAD，Reader 认证完整 ciphertext 后才解压。KDF 参数、salt 和 nonce
都持久化在对应格式字段中；读取前先执行产品上下限检查。压缩 Adapter 要求调用者提供期望输出大小和硬上限。

## Windows Filesystem Adapter（F4）

Target：`aegra_adapter_windows_filesystem` / `Aegra::AdapterWindowsFilesystem`。

依赖：`Aegra::Format`（platform_metadata envelope 编解码）、`Advapi32`（security descriptor I/O）、
`Shell32`/`Ole32`（`SHGetKnownFolderPath` 解析用户特殊目录）。

- `WindowsFileSnapshotView`：接收 composition root 注入的 snapshot root 映射（不创建 VSS）；分页枚举
  selection、no-follow 打开、主数据流读取；支持 NTFS/ReFS/FAT32，拒绝其它文件系统与 EFS。NTFS/ReFS
  entry 保存 stable File ID 和 self-relative SECURITY_DESCRIPTOR（V7 tag 1），SACL 不可读则
  `file_source.security_descriptor_unreadable`；FAT32 entry 使用 null identity 且不保存 security。
  递归 `FindFirstFileExW` / `FindNextFileW` 失败（访问拒绝、I/O、路径异常）按固定
  `unreadable_policy=fail_job` 返回 `file_source.unreadable`，不得当作空目录；Pipeline 失败
  触发 Archive Abort，避免生成不完整且可见的 Recovery Point。
  枚举时按产品策略跳过卷级系统目录 `$RECYCLE.BIN` 与 `System Volume Information`（与 Browse
  隐藏对齐；整卷 file_set 选择不会把它们写入 Archive）。
  整卷 selection 的 root entry 名使用 `display_label`（非法路径字符已剥离），不用 selection UUID。
- `WindowsFileTreeSink`：绑定 NTFS/ReFS/FAT32 目标根句柄；同目录 `.aegra-partial` staging、冲突策略
  fail/replace/rename、目录 metadata；文件/目录 metadata 应用时写回 Owner/Group/DACL/SACL
  （NTFS/ReFS 为 true，FAT32 为 false）。FAT32 同时声明 `maximum_file_size_bytes=0xFFFFFFFF`。
  F8 Worker restore 与 Service Prepare 空间/能力探测均经此 Sink。
  - `kFail`：目标已存在 → `file_restore.target_collision`（不覆盖）。
  - `kReplace`：清除目标只读后 `MOVEFILE_REPLACE_EXISTING`，失败则 Delete+rename。
  - `kRename`：目标已存在时发布为 `name (N).ext`（N=1..9999），耗尽 →
    `file_restore.rename_exhausted`。
  - **能力声明（FI0）**：`capabilities()` 声明 `supports_security_descriptor`、`free_bytes` 与
    `maximum_file_size_bytes`。不支持 reparse / hard link / sparse / ADS，Port 上无对应方法或 capability 位。
  - **Source 严格检测（FI0/FI2）**：完整枚举期间检测 reparse、`NumberOfLinks>1` 的文件、sparse 属性、
    命名 ADS，分别返回 `file_source.unsupported_reparse|hard_link|sparse|ads`，整 Job fail 且无 RP。
  - **Incremental 变化判断（ADR-0020）**：Windows source 必须为每个 entry 提供 snapshot-consistent
    `write_time` 与 `logical_size`。Pipeline 用 parent path index 比较 metadata signature；Windows Adapter
    不为 file_set Incremental 创建、查询或读取 USN Journal；历史 USN reader 及其 CMake source 已删除。
- `WindowsFileSourceBrowser`：Service 浏览用 opaque node token；不向调用方返回路径；reparse/sparse
  节点标记 `kUnsupported` 且不可选。根列表（`parent_node_token=null`）先返回当前用户可映射的特殊目录
  （Desktop / Downloads / Documents / Pictures / Music / Videos，Explorer 顺序），再返回授权盘符卷。
  特殊目录经 `SHGetKnownFolderPath` 解析，并绑定到已授权 volume 的 `volume_identity` + 相对组件；
  无法映射到授权卷或目录不存在时省略该项。Desktop 仍只看到 token 与 display_name，不接收绝对路径。

## 通用规则

- 第三方错误在边界转换为稳定 `ErrorCode`，异常不向核心传播。
- 凭据通过 `ICredentialResolver` 获取，不保存和打印明文。
- 资源使用 RAII；长操作响应取消。
- Adapter 之间不 include 实现文件，不使用共享全局 SDK Session。
- [ADR-0001](../adr/0001-personal-format-crypto-and-codec-dependencies.md) 仅允许 Personal Archive 通过 PRIVATE target dependency 组合两个无状态算法 Adapter；不得据此放宽有状态 Adapter 的依赖规则。
- 每个 Adapter 必须完整实现对应 Port 的边界、取消、错误和资源释放语义。

Windows Disk、Volume 和 VSS 的具体边界、公开接口与验证要求见
[Windows Adapter 开发文档](windows_adapters.md)。在线 Volume 必须先由独立 VSS Session 变成稳定
Snapshot Device Object，Block Source 本身不创建快照。

Worker 的 Windows 时钟、CNG 随机源和 DPAPI Credential Resolver 也由独立 Windows System Adapter
实现；Secret 必须复制到锁页内存并在析构前清零，不允许锁页失败时降级。Archive 口令使用
`dpapi-lm:<entropy_id>:<base64>`（Schedule 用 `schedule_id` 作 `pOptionalEntropy`），
不使用 Windows Credential Manager。

`Aegra::AdapterWindowsIpc` 实现本地 Worker Named Pipe Client 与 Service/Worker Named Pipe Listener。它只接受
受限逻辑名称，使用 4 字节 little-endian 长度前缀和帧上限，支持一个 Reader/Writer 并发及
`CancelIoEx` 取消；它不解析 JSON。Worker 侧 ACL 仍由父进程决定（[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)）；
Service 侧提供显式本地 ACL 与调用方身份校验（[ADR-0014](../adr/0014-windows-service-ipc-security.md)、
[windows_ipc.md](windows_ipc.md)）。

S3/S4 增加 `Aegra::AdapterWindowsProcess`、`WindowsSourceInventory` 与
`LocalRepositoryStorageFactory`。Process Adapter 只负责受控 executable/argument 启动、等待和终止；
Inventory 返回 opaque source ID 并在 Service 内解析为稳定设备 key；Storage Factory 只接受受信任 locator，
打开后继续复用 Local Object Storage 的根目录安全检查。

## NTFS Adapter（`aegra_adapter_ntfs`）

只读解析 `IRandomAccessReader` 呈现的 NTFS 卷视图（offset 0 = Boot Sector）。依赖仅 `Aegra::Base` 与
`Aegra::Ports`。公共入口 `NtfsVolumeReader`：

- `open(IRandomAccessReader&, CancellationToken)` 借用 reader（reader 必须更长寿）；
- `volume_info()`、`list_directory`、`describe_entry`、`read_file`；
- 返回 DTO 不含 HANDLE、COM、Archive 类型；
- 单实例非线程安全；调用方串行化。

解析范围与边界见 [ADR-0023](../adr/0023-in-process-explorer-archive-browsing.md) 与
[Explorer 进程内浏览](../architecture/EXPLORER_ARCHIVE_BROWSING.md)。禁止全量 MFT 扫描；MFT/Index
使用固定容量 LRU。compressed/EFS 返回稳定 unsupported；reparse 不跟随；named ADS 不暴露为普通文件。

`PersonalArchiveVolumeRandomReader` 位于 `adapters/personal_archive`：把 Archive 单卷 Chunk 视图适配为
`IRandomAccessReader`，不知道 NTFS。

## Dokan/虚拟磁盘约束

从旧项目保留以下经过验证的设计知识：

- Mount Host 独立进程隔离 Dokan 回调和故障（盘符/整盘挂载用例；非 Explorer 双击浏览权威路径）。
- 原始 backing 永远只读；写入进入独立 COW overlay 与持久化位图。
- 读操作按 COW block 选择 overlay 或 backing，部分写执行 read-modify-write。
- 格式层只负责 VHDX/VMDK/QCOW2 元数据与偏移翻译，数据平面通过 `IRandomAccessReader`。
- 锁顺序固定为布局锁，再到 backing/overlay 锁；禁止持锁调用未知回调。
- Dokan C 回调使用静态跳板进入实例，不使用全局实例。
- 对原始备份视图的任何写入都不得修改 Recovery Point。

旧文档中的具体类名、64KB 固定值、`/MT` 和旧 vcxproj 不是新实现约束；这些需要基准、格式和部署验证重新决定。

## 个人版 SQLite 控制面

S2 实现 `Aegra::AdapterSqlite` / `SqliteControlPlaneDatabase`，实现 `ports/control_plane.h` 的细粒度
Store。只保存 Repository 连接（含 SecretRef）、Job、Schedule、Event/Audit 与 schema version；不保存
明文凭据或 Recovery Point 权威数据。Schema 迁移在打开事务中完成；Job 支持状态机 CAS 与启动时
running/cancelling → interrupted 收敛。详见 [control_plane_sqlite.md](control_plane_sqlite.md)。
Command store 还持久化幂等请求指纹与 command/resource ID，支持 Service command replay 和冲突检测。

## PostgreSQL 约束

只有 Management Service 侧 Adapter 访问 PostgreSQL。使用参数化 SQL、显式事务、tenant scope、RLS 第二层防护和 Transactional Outbox。Recovery Point 投影必须支持全量重建与 generation 对账。

## 验证

构建每个受影响 Adapter Target，并通过代码审查与必要的人工运行验证短读、超时、限流、断线、重复请求和清理失败边界。验证不得使用生产云账户、生产数据库或客户数据。
