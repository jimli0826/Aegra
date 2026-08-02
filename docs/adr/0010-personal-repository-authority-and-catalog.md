# ADR-0010：个人版 Repository 权威边界与可重建目录

- 状态：Accepted
- 日期：2026-08-02
- 决策者：Aegra 项目
- 关联模块：personal_repository、application、ports、adapters/storage_*、apps/service

## 背景

个人版需要管理本地、SMB、S3 和 Azure 中的 `.bkf`，支持恢复点发现、备份链选择、验证、保留和删除。
它不使用企业版 CAS Repository，也不维护 Repository 级全局 Chunk Index。旧项目证明了“Repository
描述文件 + 每恢复点目录项 + `.bkf` 数据目录”适合个人版，但旧实现没有严格区分权威数据与查询投影，
链感知删除、云存储事务和崩溃恢复语义也不完整。

本项目尚未发布。本决策定义新的正式个人版 Repository，不读取或迁移旧项目的 `aegra.repo`、Snapshot
JSON、SQLite Schema 或目录布局。

## 决策

1. 个人版 Repository 是受管理的 Archive Store，不是企业 CAS Repository。实现位于独立
   `personal_repository` 模块，不复用企业 `repository` 的 Pack、Index、Catalog 或 Gateway。
2. `.bkf` Archive Group 是 Recovery Point 的恢复权威。首卷、连续续卷、末卷 Footer 和认证后的
   Manifest 共同决定数据是否可恢复；`.bhx` 只决定该点能否作为后续增量或差异基线。
3. Repository 根目录保存版本化 `aegra.repository` Descriptor，以及每个 Recovery Point 的独立 Catalog
   Entry。两者用于发现和查询，但都不能覆盖 `.bkf` 声明的 UUID、链关系、备份类型或数据几何。
4. Catalog Entry 只保存不需要解密客户 Metadata 的结构摘要和 Repository 相对对象名，不保存口令、
   密钥、SecretRef、挂载状态、计划、保留策略、任务或 Verify 历史。
5. 个人版 Service 可以使用 SQLite 保存 Repository 连接、SecretRef、任务、计划、策略、Verify 历史、
   扫描游标和 UI 查询投影。SQLite 丢失后仍可从 Repository 重建 Recovery Point 与链图。
6. Recovery Point 状态分层表达：`discovered`、`structurally_complete`、`metadata_authenticated`、
   `chain_complete`、`verified`。目录项不得把较弱状态伪装成较强状态；Verify 结果不是 Repository 权威。
7. Archive 写入沿用 ADR-0003：所有 partial 先完成，Sidecar 和续卷先发布，首卷最后发布。Catalog Entry
   只能在首卷发布并通过结构检查后发布；Catalog 发布失败不撤销已提交 Archive，后续扫描负责补建。
8. 扫描只枚举受管理前缀中的首卷候选，先读取固定 Header 并验证分卷/Footer，再按 `file_uuid` 建立候选
   集合和父子图。扫描阶段不对无关文件批量执行 KDF；只有具备明确 CredentialRef 的受信任调用才认证
   Metadata。
9. 链解析以 `file_uuid`、`backup_set_uuid` 和 `parent_uuid` 为准。时间戳只用于显示和排序，不能选择父层。
   完整恢复链必须从全量点开始，逐层直接相连，且通过现有 Chain Reader 的身份与几何校验。
10. 删除先生成不可变 Delete Plan。若所选点存在未包含在 Plan 中的后代则拒绝；第一版只支持删除叶子、
    完整后代子树或整个 Backup Set，不实现链重写或合并。
11. 删除顺序固定为：先发布独立 Deletion Tombstone、删除 Archive Group 的非首卷成员、最后删除首卷、
    再删除 Catalog Entry 和 Tombstone。部分失败保留 Tombstone 并允许同一 operation ID 幂等重试。
12. Storage Port 按能力拆分，至少支持受限前缀列举、范围读取、暂存写、发布、存在性/属性查询和删除。
    Adapter 必须声明条件发布、原子 rename 和强一致列举能力；Application 不根据 URI 猜测后端语义。
13. 所有对象名是 Repository 根下的规范化相对 key，禁止绝对路径、`.`、`..`、设备路径、空段和根逃逸。
    Repository Descriptor 使用随机 UUID 识别实例，复制 Repository 时身份不自动改变。

## 持久化布局

```text
repository-root/
├── aegra.repository
├── catalog/
│   ├── recovery-points/
│   │   └── <file_uuid>.entry
│   └── deletions/
│       └── <operation_uuid>.tombstone
├── archives/
│   └── YYYY/MM/
│       ├── <file_uuid>.bkf
│       ├── <file_uuid>.bkf.001
│       └── <file_uuid>.bkf.bhx
└── staging/
    └── <operation_uuid>/
```

`staging/` 永远不可作为 Recovery Point 枚举结果。日期目录仅用于分散对象和人工运维，不是身份或链关系
来源。Descriptor 和 Catalog Entry 的具体 schema 由个人版 Repository 格式规范定义。

## 备选方案

- 只使用本机 SQLite：数据库丢失或从另一台机器连接存储时无法发现恢复点，不采用。
- 只扫描 `.bkf`，不保存 Repository Catalog：本地小目录可行，但远程对象存储的反复全量扫描成本过高，
  也无法持久化删除恢复状态，不采用。
- 单一可变 Catalog 文件：并发更新、对象存储条件写和单点损坏的代价高于每恢复点独立 Entry，不采用。
- 复用企业 CAS Repository：会引入 Pack、全局 Chunk Index、Gateway 和维护对象，违背个人版单 Archive
  交付与可携带性目标，不采用。
- Adapter 根据 leaf 路径自动扫描并尝试所有口令：扩大路径信任边界且可触发大量昂贵 KDF，不采用。
- 删除中间增量并自动重写后代：需要创建新 Recovery Point 和新的加密数据，第一版风险过高，不采用。

## 影响

- 个人版 Repository 可以脱离本机 SQLite 移动、连接和重建；Catalog 丢失只影响查询性能。
- 一个已提交 Archive 可能短暂没有 Catalog Entry，扫描必须能发现并修复这种状态。
- Catalog Entry 损坏、伪造或与 Header 冲突时，以 Archive 为准并重建 Entry，不能静默修改 Archive 身份。
- S3/Azure 等不支持 rename 的后端需要通过暂存 key 和最后发布首卷实现同样的可见性边界。
- Repository 密码不作为统一数据解锁权威；每个 Archive 的 CredentialRef 仍由本机控制面管理。

## 验证

- Descriptor 和 Catalog codec 覆盖 golden、roundtrip、未知关键版本、非法 key、越界和损坏输入。
- Memory Storage Contract Test 覆盖提交前不可见、Catalog 发布失败后重建、条件冲突和幂等删除。
- 扫描测试覆盖缺卷、缺 Footer、孤立续卷、重复 UUID、Catalog/Header 冲突和无凭据扫描。
- 链图测试覆盖断链、环、跨 Backup Set 父引用、多个子节点、差异链和深度上限。
- 删除测试覆盖叶子、完整子树、遗漏后代拒绝、部分失败重试和首卷最后删除。
- Local Storage Adapter 覆盖路径逃逸、partial 崩溃残留、原子发布和进程重启后的重建。
