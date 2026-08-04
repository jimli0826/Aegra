# 个人版 Repository Descriptor 与 Catalog V1

## 1. 范围

本文定义个人版受管理 Archive Store 的 Repository Descriptor 和 Recovery Point Catalog Entry。`.bkf`
Archive Group 的权威格式仍由[个人版 `.bkf` V6](PERSONAL_BACKUP_FORMAT_V6.md)定义。本格式不定义 Chunk
Index、Pack、全局去重、任务数据库或凭据存储。

所有持久化文件使用 UTF-8 JSON、固定字符串 key 和 `schema_version`。Writer 输出规范 key，Reader 拒绝
重复 key、非 JSON object 根、未知关键版本、错误类型、超长字符串、绝对对象名和根目录逃逸。整数必须在
各字段规定的非负范围内。

## 2. Repository Descriptor

固定路径：`aegra.repository`。

```json
{
  "schema_version": 1,
  "kind": "aegra_personal_repository",
  "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
  "created_utc_ms": 1785600000000,
  "archive_prefix": "archives",
  "catalog_prefix": "catalog/recovery-points",
  "deletion_prefix": "catalog/deletions",
  "staging_prefix": "staging",
  "layout_version": 1
}
```

### 2.1 字段

| 字段 | 约束 |
| --- | --- |
| `schema_version` | 必须为 `1` |
| `kind` | 必须为 `aegra_personal_repository` |
| `repository_uuid` | 小写 RFC 4122 UUID 文本，创建后不可变 |
| `created_utc_ms` | 非负 Unix UTC 毫秒，仅用于审计 |
| `archive_prefix` | V1 固定为 `archives` |
| `catalog_prefix` | V1 固定为 `catalog/recovery-points` |
| `deletion_prefix` | V1 固定为 `catalog/deletions` |
| `staging_prefix` | V1 固定为 `staging` |
| `layout_version` | 必须为 `1` |

Descriptor 不保存显示名称、Storage URI、访问凭据、Archive 口令、默认压缩或 UI 偏好。这些属于本机
Service 配置。创建 Descriptor 使用 `staging/<operation_uuid>/aegra.repository` 暂存并发布；已经存在时
不得覆盖或生成新的 Repository UUID。

## 3. Catalog Entry

固定 key：`catalog/recovery-points/<file_uuid>.entry`。文件名 UUID 必须与内容及 `.bkf` Header 一致。

```json
{
  "schema_version": 1,
  "kind": "aegra_personal_recovery_point",
  "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
  "file_uuid": "11111111-2222-4333-8444-555555555555",
  "backup_set_uuid": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
  "parent_uuid": null,
  "backup_type": "full",
  "archive_main_key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf",
  "split_part_count": 1,
  "has_sidecar": true,
  "format_version": 6,
  "created_utc_ms": 1785600000000,
  "logical_size_bytes": 107374182400,
  "stored_size_bytes": 42949672960,
  "source_count": 1,
  "structural_state": "complete",
  "catalog_generation": 1
}
```

### 3.1 身份与链字段

| 字段 | 约束 |
| --- | --- |
| `repository_uuid` | 必须匹配 Descriptor |
| `file_uuid` | 必须匹配文件名和 Archive Header |
| `backup_set_uuid` | 必须匹配 Archive Header |
| `parent_uuid` | 全量为 `null`；增量/差异为父 `file_uuid` |
| `backup_type` | `full`、`incremental` 或 `differential` |
| `format_version` | 必须匹配 Archive Header，目前为 `6` |

### 3.2 物理与统计字段

| 字段 | 约束 |
| --- | --- |
| `archive_main_key` | Repository 相对规范 key，必须位于 `archives/` |
| `split_part_count` | 实际发现且验证的总分卷数，范围 `1` 至 `1000`；非分卷为 `1` |
| `has_sidecar` | Sidecar 是否存在且通过 Header/Archive 身份校验 |
| `created_utc_ms` | 来自认证 Manifest；未认证扫描时为 `0` |
| `logical_size_bytes` | 来自认证 Manifest/Footer；未知时为 `0` |
| `stored_size_bytes` | Archive Group 各分卷对象大小之和；未知时为 `0` |
| `source_count` | 来自认证 Manifest；未认证扫描时为 `0` |
| `structural_state` | V1 持久化值固定为 `complete` |
| `catalog_generation` | V1 从 `1` 开始，替换同一 Entry 时递增 |

Catalog 只发布结构完整的 Recovery Point，因此不持久化 `discovered` 或 `incomplete` Entry。扫描报告和
SQLite 投影可以表达 `discovered`、`unavailable`、`corrupt`、`metadata_authenticated`、`chain_complete`
和 `verified`，但这些不是 Catalog Entry 的提交状态。

## 4. Deletion Tombstone

固定 key：`catalog/deletions/<operation_uuid>.tombstone`。Tombstone 是不可变 Delete Plan 的持久化副本，
用于隐藏正在删除的 Recovery Point，并在进程或网络故障后继续执行。

```json
{
  "schema_version": 1,
  "kind": "aegra_personal_deletion",
  "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
  "operation_uuid": "99999999-8888-4777-8666-555555555555",
  "created_utc_ms": 1785600000000,
  "targets": [
    {
      "file_uuid": "11111111-2222-4333-8444-555555555555",
      "catalog_generation": 1,
      "archive_main_key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf",
      "members": [
        {
          "key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf.bhx",
          "generation": "storage-generation-3"
        },
        {
          "key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf.001",
          "generation": "storage-generation-2"
        },
        {
          "key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf",
          "generation": "storage-generation-1"
        }
      ]
    }
  ]
}
```

`targets` 按后代到祖先排序；每个 `members` 固定按 Sidecar、续卷从后向前、首卷的顺序保存。首卷
必须是最后一个 member，且所有 key 必须位于 `archives/` 并与目标 `file_uuid` 的 Archive Group 一致。每个
member 保存计划时观察到的 Storage generation；计划时对象已不存在则为 `null`。执行时已有 generation
必须条件匹配，`null` member 必须仍不存在，防止重启续作删除同 key 的新对象。
Tombstone 使用条件创建，同一 operation UUID 的不同内容是冲突。存在有效 Tombstone 的目标不得出现在
可恢复列表中；执行完成后先删除 Catalog Entry，最后删除 Tombstone。Tombstone 不保存 SecretRef。

## 5. 对象名规则

- 使用 `/` 分隔，禁止前导或尾随 `/`。
- 每段非空，禁止 `.`、`..`、NUL、反斜杠、冒号和控制字符。
- `file_uuid` 和 `operation_uuid` 使用规范小写 RFC 4122 文本。
- Archive 主文件固定为 `<file_uuid>.bkf`，续卷固定追加 `.001` 至 `.999`；产品上限可以更低。
- Sidecar 固定为 `<file_uuid>.bkf.bhx`。
- 日期目录采用认证 Manifest 的 UTC 年月；无法认证时新备份不得自行选择未知时间。

## 6. 发布协议

新 Recovery Point 的 operation ID 在一次重试序列中保持不变：

```text
1. 写 staging/<operation_uuid>/ 下的 Archive Group
2. 完成并验证末卷 Footer、分卷身份和 Sidecar
3. 发布 Sidecar
4. 从最后一个续卷向第一个续卷发布
5. 最后发布首卷 archive_main_key
6. 重新以 Reader 打开并执行结构检查
7. 条件创建 Catalog Entry
```

步骤 5 是 Recovery Point 数据可见性边界。步骤 7 失败时 Recovery Point 仍然有效，扫描器必须根据首卷
补建 Catalog。相同 `file_uuid` 的既有首卷或 Catalog Entry 与本次内容不一致时返回冲突，不得覆盖。

## 7. 扫描与重建

1. 读取并验证 Descriptor。
2. 列举 `catalog/recovery-points/`，解析 Entry 并检查 Repository UUID 和规范 key。
3. 列举 `archives/` 下的 `.bkf` 首卷候选，不把 `.bkf.NNN`、`.bhx` 或 `staging/` 作为候选。
4. 对每个候选验证 Header、连续分卷、身份字段、Chunk 序列和末卷 Footer。
5. 从固定 Header 建立 `file_uuid -> parent_uuid` 图，不执行目录级口令猜测。
6. 缺失或冲突的 Catalog Entry 根据 Archive 重建；冲突 Entry 保留诊断后替换。
7. 只有调用方提供对应 CredentialRef 并成功解析 Secret 后，才认证 Manifest 并补全受保护摘要。

重复 `file_uuid` 对应不同 `archive_main_key` 时必须报告冲突并停止自动修复。缺父点不影响单个 Archive
的结构完整状态，但该 Recovery Point 的链状态为 incomplete，不能恢复、挂载或作为自动保留删除依据。

## 8. 删除协议

Delete Plan 包含 `operation_uuid`、所选 `file_uuid` 集合及扫描时观察到的 Catalog generation。执行前重新
加载链图；发现计划外后代、generation 变化或重复 UUID 冲突时拒绝。

```text
1. 发布 catalog/deletions/<operation_uuid>.tombstone
2. 删除所选点的 Sidecar
3. 删除续卷，从最后一卷向第一卷
4. 最后删除每个点的首卷
5. 删除对应 Catalog Entry
6. 删除 Tombstone
```

删除按后代到祖先顺序执行。首次执行前校验图和 generation；Tombstone 成功发布后，其不可变内容就是
重试依据，不能从已经部分删除的 Repository 重新推导 Plan。任何失败保留 Tombstone 并返回部分完成；
使用相同 operation ID 重试必须安全。不存在的目标成员视为已经完成，但 key 身份冲突不能视为成功。
V1 不通过修改 `parent_uuid` 修复链。

## 9. 安全与隐私

- Descriptor 和 Catalog Entry 默认明文，因为它们只保存恢复点结构信息；源路径、卷标、主机名和客户
  Metadata 不得写入 Catalog。
- Archive SecretRef 只保存在受保护的本机 SQLite/凭据系统中，以 `repository_uuid + file_uuid` 关联。
- Catalog 数据始终视为不可信输入；恢复前必须重新打开 Archive 并执行正式认证与 Chain Reader 校验。
- 列举、读取和删除必须受 Repository 根和固定前缀限制，不接受调用方提供任意绝对路径。
