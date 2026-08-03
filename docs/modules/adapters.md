# `adapters` 模块开发文档

## 目标

实现 `ports` 定义的外部能力。每个 Adapter 独立 Target，只依赖所实现的 Port、必要 Contracts 和自己的第三方库。

## Adapter 分类

- Storage：Local、SMB、S3、Azure。
- Windows：Disk、Volume、VSS、Partition、BCD/WinRE。
- 控制面：PostgreSQL（企业版）、个人版 SQLite 控制面（`Aegra::AdapterSqlite`）。
- 虚拟化：VMware、Hyper-V。
- 挂载：Dokan 与虚拟磁盘格式呈现。
- 传输：HTTP/gRPC/Named Pipe。
- 测试与本地验证：Memory Block Source/Sink、Memory Backup Session/Reader。
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

个人版 Archive Adapter 组合 `format`、`IBackupSession`、`IRecoveryPointReader`、libsodium 和 Zstandard。当前数据面支持一个 volume，并可在完整 chunk 边界透明分卷。写入期间所有分卷与 Sidecar 使用 partial 路径，完整 Footer 和加密 Sidecar 均写完后，先发布 Sidecar/续卷，最后发布首卷；Abort 和析构清理本次创建的 partial 文件。Reader 在解析 CBOR 前必须完成 Header/Envelope 范围校验和 AEAD 认证，随后发现并验证连续分卷。普通恢复不依赖 `.bhx`；增量比较通过显式 API 加载并认证 Sidecar。

增量 Session 在创建时验证显式父 Archive 及父 Sidecar，继承备份集 UUID，并把输入完整源转换为连续变化区间组成的稀疏层；新 Sidecar 仍保存完整状态。`PersonalArchiveReader` 可以 inspect 单个稀疏层，`PersonalArchiveChainReader` 接受显式 base-first 层列表并验证链关系，对通用 Restore Pipeline 提供连续覆盖视图。链恢复不读取 Sidecar；链发现和逐层凭据选择属于 Application，不由 Adapter 扫描目录猜测。

SMB、S3 和 Azure Storage Adapter 还必须实现个人版 Repository 所需的细粒度对象 Port。Adapter
负责 Repository 相对 key 到本地路径或对象 key 的安全映射，并显式报告原子 rename、条件创建和列举
一致性 capability；不得把 URI 判断、临时发布或删除重试逻辑泄漏到 Application。具体契约见
[个人版 Repository 模块](personal_repository.md)。

Archive Reader 分别限制 metadata、chunk stored payload 和展开后的 chunk logical size。ZERO run、压缩块和其它稀疏表示在分配恢复缓冲区前都必须通过 logical size 上限检查。

密码 Adapter 使用 Argon2id v1.3 派生 master key、HKDF-SHA256 分离 metadata、Chunk Payload 和 Sidecar key，并使用 XChaCha20-Poly1305 detached tag；内容散列使用 SHA-256。每个 Chunk 使用独立随机 nonce，Header 和 BlockEntry 作为 AAD，Reader 认证完整 ciphertext 后才解压。KDF 参数、salt 和 nonce 都持久化在对应格式字段中；读取前先执行产品上下限检查。压缩 Adapter 要求调用者提供期望输出大小和硬上限。

## 通用规则

- 第三方错误在边界转换为稳定 `ErrorCode`，异常不向核心传播。
- 凭据通过 `ICredentialResolver` 获取，不保存和打印明文。
- 资源使用 RAII；长操作响应取消。
- Adapter 之间不 include 实现文件，不使用共享全局 SDK Session。
- [ADR-0001](../adr/0001-personal-format-crypto-and-codec-dependencies.md) 仅允许 Personal Archive 通过 PRIVATE target dependency 组合两个无状态算法 Adapter；不得据此放宽有状态 Adapter 的依赖规则。
- 每个 Adapter 运行对应 Port 的 Contract Test Suite。

Windows Disk、Volume 和 VSS 的具体边界、公开接口与测试要求见
[Windows Adapter 开发文档](windows_adapters.md)。在线 Volume 必须先由独立 VSS Session 变成稳定
Snapshot Device Object，Block Source 本身不创建快照。

Worker 的 Windows 时钟、CNG 随机源和 Credential Manager Resolver 也由独立 Windows System Adapter
实现；Secret 必须复制到锁页内存并在析构前清零，不允许锁页失败时降级。

`Aegra::AdapterWindowsIpc` 实现本地 Worker Named Pipe Client 与 Service Named Pipe Listener。它只接受
受限逻辑名称，使用 4 字节 little-endian 长度前缀和帧上限，支持一个 Reader/Writer 并发及
`CancelIoEx` 取消；它不解析 JSON。Worker 侧 ACL 仍由父进程决定（[ADR-0008](../adr/0008-worker-session-named-pipe-protocol.md)）；
Service 侧提供显式本地 ACL 与调用方身份校验（[ADR-0014](../adr/0014-windows-service-ipc-security.md)、
[windows_ipc.md](windows_ipc.md)）。

## Dokan/虚拟磁盘约束

从旧项目保留以下经过验证的设计知识：

- Mount Host 独立进程隔离 Dokan 回调和故障。
- 原始 backing 永远只读；写入进入独立 COW overlay 与持久化位图。
- 读操作按 COW block 选择 overlay 或 backing，部分写执行 read-modify-write。
- 格式层只负责 VHDX/VMDK/QCOW2 元数据与偏移翻译，数据平面通过 `IRandomAccessReader`。
- 锁顺序固定为布局锁，再到 backing/overlay 锁；禁止持锁调用未知回调。
- Dokan C 回调使用静态跳板进入实例，不使用全局实例。
- 对原始备份视图的任何写入都不得修改 Recovery Point。

旧文档中的具体类名、64KB 固定值、`/MT` 和旧 vcxproj 不是新实现约束；这些需要基准、格式和部署测试重新决定。

## 个人版 SQLite 控制面

S2 实现 `Aegra::AdapterSqlite` / `SqliteControlPlaneDatabase`，实现 `ports/control_plane.h` 的细粒度
Store。只保存 Repository 连接（含 SecretRef）、Job、Schedule、Event/Audit 与 schema version；不保存
明文凭据或 Recovery Point 权威数据。Schema 迁移在打开事务中完成；Job 支持状态机 CAS 与启动时
running/cancelling → interrupted 收敛。详见 [control_plane_sqlite.md](control_plane_sqlite.md)。

## PostgreSQL 约束

只有 Management Service 侧 Adapter 访问 PostgreSQL。使用参数化 SQL、显式事务、tenant scope、RLS 第二层防护和 Transactional Outbox。Recovery Point 投影必须支持全量重建与 generation 对账。

## 测试

除真实集成套件外，单元测试不得依赖云账户、生产数据库、物理磁盘或安装的 Dokan 驱动。故障注入覆盖短读、超时、限流、断线、重复请求和清理失败。
