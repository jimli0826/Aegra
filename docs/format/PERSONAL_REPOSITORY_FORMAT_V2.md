# 个人版 Repository Descriptor 与 Catalog V2

| 属性 | 内容 |
| --- | --- |
| 状态 | 权威格式规范 |
| Catalog schema | 2 |
| Descriptor schema | 1（布局未变；与 Catalog V2 并存） |
| 决策依据 | [ADR-0016](../adr/0016-file-set-backup-and-restore-boundary.md)、[ADR-0010](../adr/0010-personal-repository-authority-and-catalog.md) |
| Archive 权威 | [个人版 `.bkf` V7](PERSONAL_BACKUP_FORMAT_V7.md) |

> 产品未发布：生产只接受本文版本。不实现 Catalog V1 双读、字段 alias、数据库式 migration 或 format
> conversion。扫描器对 `schema_version != 2` 的 Entry 统一拒绝并报告 conflict/unsupported。

## 1. 范围

本文定义个人版受管理 Archive Store 的 Repository Descriptor 与 Recovery Point Catalog Entry。`.bkf`
Archive Group 的权威格式由 V7 定义。本格式不定义 Chunk Index、Pack、全局去重、任务数据库或凭据存储。

所有持久化文件使用 UTF-8 JSON、固定字符串 key 和 `schema_version`。Writer 输出规范 key；Reader 拒绝
重复 key、非 object 根、未知关键版本、错误类型、超长字符串、绝对对象名和根目录逃逸。整数必须在各字段
规定的非负范围内，且不超过 `INT64_MAX` 当跨进程投影到 Service 时。

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
| `created_utc_ms` | 非负 Unix UTC 毫秒 |
| `archive_prefix` | 固定 `archives` |
| `catalog_prefix` | 固定 `catalog/recovery-points` |
| `deletion_prefix` | 固定 `catalog/deletions` |
| `staging_prefix` | 固定 `staging` |
| `layout_version` | 必须为 `1` |

Descriptor 不保存显示名称、Storage URI、凭据、Archive 口令或 UI 偏好。

## 3. Catalog Entry V2

固定 key：`catalog/recovery-points/<file_uuid>.entry`。文件名 UUID 必须与内容及 `.bkf` Header 一致。

```json
{
  "schema_version": 2,
  "kind": "aegra_personal_recovery_point",
  "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
  "file_uuid": "11111111-2222-4333-8444-555555555555",
  "backup_set_uuid": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
  "parent_uuid": null,
  "backup_type": "full",
  "content_kind": "volume_set",
  "archive_main_key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf",
  "split_part_count": 1,
  "has_sidecar": true,
  "format_version": 7,
  "created_utc_ms": 1785600000000,
  "logical_size_bytes": 107374182400,
  "stored_size_bytes": 42949672960,
  "source_count": 1,
  "source_volume_ids": [
    "\\\\?\\Volume{11111111-2222-4333-8444-555555555555}\\"
  ],
  "file_entry_count": 0,
  "file_stream_count": 0,
  "structural_state": "complete",
  "catalog_generation": 1
}
```

### 3.1 身份与链字段

| 字段 | 约束 |
| --- | --- |
| `schema_version` | 必须为 `2` |
| `kind` | `aegra_personal_recovery_point` |
| `repository_uuid` | 匹配 Descriptor |
| `file_uuid` | 匹配文件名与 Archive Header |
| `backup_set_uuid` | 匹配 Archive Header |
| `parent_uuid` | volume full 为 `null`；volume inc/diff 为父 `file_uuid`；**file_set 必须 `null`** |
| `backup_type` | `full` / `incremental` / `differential`；file_set 仅 `full` |
| `content_kind` | `volume_set` 或 `file_set`；必须匹配 Header |
| `format_version` | 必须为 `7` |

### 3.2 物理与统计字段

| 字段 | 约束 |
| --- | --- |
| `archive_main_key` | Repository 相对 key，位于 `archives/` |
| `split_part_count` | 1..1000；非分卷为 1 |
| `has_sidecar` | volume 链可 true；**file_set 必须 false** |
| `created_utc_ms` | 认证 Manifest；未认证扫描为 0 |
| `logical_size_bytes` | 认证后；未知 0 |
| `stored_size_bytes` | 各分卷对象大小之和；未知 0 |
| `source_count` | volume：有序 volume 数；file_set：selection root 数；未认证扫描可为 0 |
| `source_volume_ids` | **仅 volume_set**：有序 canonical Volume GUID Path，长度=`source_count` 且唯一；**file_set 必须为 `[]`** |
| `file_entry_count` | file_set 认证后 entry 总数；volume_set 必须 0；未认证 0 |
| `file_stream_count` | file_set 认证后 stream 总数；volume_set 必须 0；未认证 0 |
| `structural_state` | 持久化固定 `complete` |
| `catalog_generation` | 从 1 起，替换同一 Entry 时递增 |

### 3.3 禁止字段

Catalog **不得**保存：

- 文件路径、相对组件、文件名、目录摘要；
- ACL / security descriptor / owner SID；
- File Index locator、page digest、stream offset；
- SecretRef、口令、token；
- 完整 selection 列表以外的客户树内容（`source_volume_ids` 仅 volume 几何）。

无凭据扫描仅从 V7 Header 重建：`file_uuid`、`backup_set_uuid`、`parent_uuid`、`content_kind`、
`format_version`、分卷结构状态。`file_entry_count` / 逻辑大小等在认证后补全。

## 4. Deletion Tombstone

固定 key：`catalog/deletions/<operation_uuid>.tombstone`。

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
      "content_kind": "volume_set",
      "archive_main_key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf",
      "members": [
        {
          "key": "archives/2026/08/11111111-2222-4333-8444-555555555555.bkf.bhx",
          "generation": "storage-generation-3"
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

规则：

- `targets` 按后代到祖先排序；
- `members`：Sidecar（若有）→ 续卷从后向前 → 首卷最后；
- file_set 目标不得包含 `.bhx` member；
- 条件创建；不保存 SecretRef。

`content_kind` 在 tombstone target 上为可选增强字段；若存在必须与删除前 Catalog 一致。执行删除不依赖
该字段解析 Archive。

## 5. 对象名规则

- `/` 分隔，禁止前导/尾随 `/`；
- 段非空，禁止 `.` `..` NUL `\` `:` 与控制字符；
- UUID 小写 RFC 4122；
- 主文件 `<file_uuid>.bkf`，续卷 `.001`–`.999`；
- Sidecar 仅 volume：`<file_uuid>.bkf.bhx`；
- 日期目录来自认证 Manifest UTC 年月。

## 6. 发布协议

```text
1. 写 staging/<operation_uuid>/ 下 Archive Group
2. 验证末卷 Footer、分卷身份；（volume）Sidecar
3. 发布 Sidecar（若有）
4. 续卷从后向前发布
5. 最后发布首卷 archive_main_key
6. Reader 结构检查
7. 条件创建 Catalog Entry V2
```

步骤 5 是数据可见性边界。步骤 7 失败时扫描器必须能从首卷重建 Catalog。相同 `file_uuid` 内容冲突不得覆盖。

## 7. 扫描与重建

1. 验证 Descriptor；
2. 列举 Catalog Entry；`schema_version` 必须为 2，否则报告并跳过自动修复为 V2 以外版本；
3. 列举 `archives/` 下 `.bkf` 首卷；
4. 验证 V7 Header、分卷、Footer；读取明文 `content_kind`；
5. 建 `file_uuid -> parent_uuid` 图（**file_set 不参与 volume parent 几何 / sidecar 选父**）；
6. 缺/冲突 Entry 按 Archive 重建 V2 字段；
7. 仅在调用方提供 Credential 并成功后认证 Manifest/Index，补全 `logical_size_bytes`、
   `file_entry_count`、`file_stream_count`、`source_volume_ids`（volume）等。

## 8. 删除协议

与 ADR-0010 相同顺序：tombstone → sidecar → 续卷 → 首卷 → Catalog Entry → tombstone。计划外后代、
generation 变化或 UUID 冲突时拒绝。

## 9. volume_set 与 file_set 差异摘要

| 项 | volume_set | file_set |
| --- | --- | --- |
| `content_kind` | `volume_set` | `file_set` |
| `backup_type` | full/inc/diff | full only |
| `parent_uuid` | 链规则 | 必须 null |
| `has_sidecar` | 按实现 | 必须 false |
| `source_volume_ids` | 几何匹配必填（认证后） | `[]` |
| `file_entry_count` | 0 | 认证后 ≥ 0 |
| 增量选父 | 使用 volume 几何 + sidecar | 不适用 |

## 10. 拒绝规则（Catalog Reader）

- 未知 `schema_version` / `kind` / `content_kind` / `backup_type`；
- `format_version != 7`；
- file_set 且（`has_sidecar==true` 或 `parent_uuid!=null` 或 `backup_type!="full"` 或
  `source_volume_ids` 非空）；
- volume_set 且认证后 `source_volume_ids.length != source_count`；
- 字符串超长（UUID 字段固定；`archive_main_key` ≤ 512 字节；path 段规则见 §5）；
- 额外未知关键字段：V2 exact_keys 模式下拒绝未列出字段。

V2 Entry exact key 集合：

```text
schema_version, kind, repository_uuid, file_uuid, backup_set_uuid, parent_uuid,
backup_type, content_kind, archive_main_key, split_part_count, has_sidecar,
format_version, created_utc_ms, logical_size_bytes, stored_size_bytes, source_count,
source_volume_ids, file_entry_count, file_stream_count, structural_state,
catalog_generation
```
