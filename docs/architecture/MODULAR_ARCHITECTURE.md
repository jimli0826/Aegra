# Aegra 模块化目标架构

| 属性 | 内容 |
| --- | --- |
| 状态 | 目标架构 |
| 版本 | 1.0 |
| 日期 | 2026-08-01 |

## 1. 架构决议

1. 进程间隔离服务、任务执行、Repository Gateway、虚拟化 Connector、挂载、WinPE 和 Shell Extension。
2. 进程内采用模块化单体，内部模块优先静态库。
3. 核心采用 Ports and Adapters；只有应用入口负责依赖注入。
4. 控制面与数据面分离。
5. 个人版使用单文件 `.bkf`；企业版使用内容寻址、全局去重的 CAS Repository。
6. 企业控制面数据库使用 PostgreSQL。
7. Recovery Point 和 Chunk Index 的权威数据不存放在 PostgreSQL。
8. 产品尚未发布，不保留旧项目或试验格式的兼容实现。

## 2. 目标目录

```text
src/
├── base/
├── contracts/
├── ports/
├── format/
│   ├── manifest/
│   ├── personal_archive/
│   └── enterprise_repository/
├── pipeline/
│   ├── backup/
│   ├── restore/
│   ├── chunking/
│   └── transforms/
├── application/
├── personal_repository/
├── repository/
│   ├── client/
│   ├── gateway/
│   ├── index/
│   ├── catalog/
│   ├── pack/
│   └── maintenance/
├── virtualization/
│   ├── contracts/
│   └── application/
├── adapters/
│   ├── storage_local/
│   ├── storage_smb/
│   ├── storage_s3/
│   ├── storage_azure/
│   ├── windows_disk/
│   ├── windows_vss/
│   ├── postgres/
│   ├── vmware/
│   ├── hyperv/
│   └── dokan/
└── apps/
    ├── service/
    ├── worker/
    ├── repository_gateway/
    ├── vmware_connector/
    ├── hyperv_connector/
    ├── mount_host/
    ├── pe_restore/
    ├── desktop/
    └── shell_extension/
```

仓库不维护项目测试目录或测试 Target。详细设计、格式和 ADR 分别放在 `docs/architecture`、`docs/format`、`docs/adr`。

## 3. 依赖方向

```text
apps --------------------> application, adapters, personal repository, repository gateway
application -------------> pipeline, personal repository, repository client, contracts
pipeline ----------------> format, ports, contracts
personal repository -----> format, ports, contracts
repository client -------> ports, contracts
repository gateway ------> repository modules, adapters
adapters ----------------> ports, contracts
format, ports, contracts -> base
base --------------------> C++ standard library only
```

硬性规则：

- `base` 不依赖任何产品模块。
- `contracts` 只依赖 `base`。
- `ports` 只依赖 `base` 和必要的 `contracts`。
- `format` 不知道路径、数据库、VSS、虚拟机或 UI。
- `pipeline` 不依赖具体 Storage、Windows、PostgreSQL 或厂商 SDK。
- Adapter 只能实现 Port，不直接依赖另一个 Adapter 的实现。
- `application` 组织 Use Case，不接受 HTTP、Qt、数据库句柄或厂商类型。
- `apps` 是唯一 Composition Root。
- CMake Target 只声明直接依赖，CI 检测循环和反向依赖。

## 4. 进程模型

```text
Desktop GUI -> Management Service -> Task Worker
                         |          -> Connector Host
                         |          -> Mount Host / WinPE
                         +-> PostgreSQL

Task Worker -> Repository Gateway -> CAS Repository
```

- GUI 只调用版本化 IPC/HTTPS，不链接 Engine、Repository 或厂商 SDK。
- Service 管理租户、资源、策略、任务、权限和投影，不执行块处理。
- Worker 每任务独立运行，组装 Pipeline，任务完成后不保留权威状态。
- Gateway 是企业 Repository 的在线写入入口，负责提交、索引、租约和维护。
- Connector Host 隔离 VDDK、vSphere、Hyper-V 等厂商依赖。
- Shell Extension 保持轻量，只通过 IPC 请求 Mount Host。

## 5. 核心端口

端口按最小能力拆分：

```cpp
class IBlockSource;
class IBlockSink;
class ISequentialWriter;
class IRandomAccessReader;
class IObjectReader;
class IStagedObjectWriter;
class IPrefixEnumerator;
class IObjectPublisher;
class IObjectDeleter;
class ISnapshotSession;
class IProgressSink;
class ICredentialResolver;
class IClock;
```

物理磁盘、VMware、Hyper-V 和内存源统一适配 `IBlockSource`；恢复目标统一适配 `IBlockSink`。不得为每一种数据源复制 Backup/Restore Pipeline。

## 6. Backup 与 Restore Pipeline

```text
Snapshot -> Block Source -> Extent Enumerator -> Chunker
         -> Hash/Dedup -> Compress -> Encrypt -> Backup Session -> Commit

Recovery Point -> Manifest Validation -> Chunk Resolver
               -> Fetch/Decrypt/Decompress -> Logical Stream -> Block Sink
```

该图是逻辑顺序。个人版 Volume Set 的 Hash/Dedup 在 Personal Archive Session 内按 ADR-0022 限制为单
`VolumeChunk`；通用 Pipeline 不依赖其 BlockEntry 编码。企业版跨 Recovery Point 去重仍属于 CAS Repository。

Pipeline 负责并发、背压、块映射、转换、校验、进度、取消和提交协议；不创建云客户端、数据库连接、VSS Snapshot 或 VMware Session。

## 7. 个人版格式

`format/personal_archive` 负责 V6 `.bkf` Header、加密 CBOR Metadata、Chunk Stream、Footer 和 Sidecar。

- CBOR Map key 固定使用 UTF-8 `snake_case` text string；整数 key 无效。
- Archive Reader/Writer 通过通用 Manifest 和 Pipeline 接入。
- `.bkf` 是个人版物理格式，不作为企业 Repository 的 Pack 格式。
- 个人版可以使用 SQLite 保存 UI 状态和可重建查询缓存，不需要 PostgreSQL 或全局 Chunk Index。

个人版可以把一个 Storage Root 初始化为受管理 Archive Store，但它不等同于企业 CAS Repository：

- `.bkf` Archive Group 是 Recovery Point 的恢复权威；首卷最后发布并作为数据可见性标记。
- 根 Descriptor 和每 Recovery Point Catalog Entry 提供可携带、可重建的发现目录。
- `personal_repository` 负责扫描、链图、显式链解析和链感知删除计划，不解析 Chunk Payload。
- 本机 SQLite 只保存 Repository 连接、SecretRef、任务、计划、策略、验证历史和查询投影。
- SQLite 或 Catalog 丢失后可从 `.bkf` Header、分卷和 Footer 重建；恢复前仍须认证 Archive。

具体决策与格式见 [ADR-0010](../adr/0010-personal-repository-authority-and-catalog.md)和
[个人版 Repository V1](../format/PERSONAL_REPOSITORY_FORMAT_V1.md)。

## 8. 企业 CAS Repository

权威边界：

| 对象 | 权威位置 |
| --- | --- |
| Chunk Payload 与 Pack | Repository Object Store |
| Pack Local Index | Pack Footer |
| Repository Chunk Index | 不可变 Index Segment + Versioned Root |
| Recovery Point | Manifest + Commit Object |
| Recovery Point Catalog | Repository Catalog Segment |
| GC、Compaction、Tombstone | Repository Maintenance Objects |
| UI 查询、任务和权限 | PostgreSQL |

核心不变量：PostgreSQL 完全丢失后，只要 Repository 对象和密钥仍在，就可以发现 Recovery Point、重建 Chunk Index 并恢复数据。

Repository 写入顺序：

```text
Begin Transaction
-> Find Missing Chunks Batch
-> Upload Immutable Packs
-> Publish Index Segments and New Root Generation
-> Upload Recovery Point Manifest
-> Validate All Chunk References
-> Publish Commit Object
-> Update Repository Catalog
-> Update PostgreSQL Projection Asynchronously
```

只有 Commit Object 发布后 Recovery Point 才可见。对象原则上不可变；Root 更新使用 generation 和条件写。GC 从 Repository Catalog 与 Manifest 图计算可达性，不以 PostgreSQL 引用计数作为删除依据。

## 9. PostgreSQL 边界

PostgreSQL 是以下对象的权威库：租户、用户、角色、Agent、受保护资源、计划、策略、任务、审计、配额和 Transactional Outbox。

PostgreSQL 可以保存 Recovery Point 摘要、容量、去重率和维护状态等可重建投影，但禁止作为以下对象的权威来源：

- 完整 Chunk Index；
- Recovery Point 到 Chunk 的映射；
- Pack 内物理布局；
- Recovery Point Commit 状态的唯一副本；
- Repository 主密钥或明文凭据。

只有 Management Service 和受控后台任务访问 PostgreSQL。Worker、Mount、WinPE 和 Shell Extension 不直接访问。

## 10. 虚拟化边界

虚拟化拆分为 Inventory、Snapshot、CBT/RCT、Virtual Disk Reader/Writer、Provisioning 和 Lifecycle 能力。厂商 SDK 只存在于 Adapter/Connector Host。

多磁盘 VM 备份使用同一个 Snapshot Session；CBT/RCT generation 不匹配时退化为全量；快照必须通过 RAII 在成功、失败和取消路径清理。

## 11. 开发阶段

1. 建立 CMake、工程规范、`base/contracts/ports` 和架构检查。
2. 实现通用 Block Source/Sink、Backup/Restore Pipeline 与内存 Adapter。
3. 实现通用 Manifest 和个人版 `.bkf` Adapter。
4. 实现企业 Pack、Index、Manifest、Commit、Catalog、Local Object Store 和 Gateway MVP。
5. 实现 PostgreSQL 控制面及 Repository 投影重建。
6. 实现 GC、Compaction、Scrub 与灾难恢复工具。
7. 实现 VMware/Hyper-V Connector 和虚拟机备份恢复。

每个阶段必须保持主分支可构建并通过静态/架构检查；不得先创建反向依赖再承诺后续清理。

## 12. Definition of Done

- 依赖方向正确，公共接口不泄漏 Adapter 或厂商类型。
- 所有权、取消、失败和崩溃恢复路径明确并完成审查与必要的人工验证。
- 数据面权威信息可以脱离 PostgreSQL 读取和重建。
- 新格式有版本、校验、golden、roundtrip 和损坏输入规则。
- 直接 Target 可在干净环境独立构建。
- 工程规范中的规模、静态分析、安全和文档要求全部通过。
