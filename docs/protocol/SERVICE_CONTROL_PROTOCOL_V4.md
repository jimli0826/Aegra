# 本地 Service 控制面协议 V4（详细说明）

> [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md) 已将 file_set Incremental 变化判断改为
> metadata signature（`write_time + logical_size`）。current V4 的 Schedule/Job/Recovery Point 字段直接承载
> Full/Incremental，不增加 V5 或 V4 兼容分支；目标控制面语义见
> [增量设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md) §4。
>
> [ADR-0022](../adr/0022-volume-set-chunk-local-deduplication.md) 在 current V4 Schedule/TaskResult 中
> 增加 volume_set 单 Chunk 去重选项与统计；同样不增加协议兼容分支。

| 属性 | 内容 |
| --- | --- |
| 状态 | 权威 wire 说明 |
| Schema | `schema_version = 4` |
| API | `api_version = 4` |
| 决策依据 | [ADR-0017](../adr/0017-service-control-protocol-v4.md)、[ADR-0016](../adr/0016-file-set-backup-and-restore-boundary.md)、[ADR-0011](../adr/0011-local-service-desktop-ipc.md)、[ADR-0014](../adr/0014-windows-service-ipc-security.md) |
| 编解码 | `apps/service`、`apps/desktop`（实现于 F6/F9）；契约 DTO 在 `src/contracts` |
| 取代 | [SERVICE_CONTROL_PROTOCOL_V3.md](SERVICE_CONTROL_PROTOCOL_V3.md)（开发期文档，生产不实现） |

字段集合为 **精确集合**（`exact_keys`）：多字段、少字段或未知字段一律拒绝。  
冲突时以 Contracts 校验与 Service/Desktop codec 为准；行为变更须同步更新本文档。

产品未发布：**不**实现 V3 解析、协商或 fallback。`schema_version != 4` 统一拒绝。

---

## 1. 传输与帧

| 项 | 规则 |
| --- | --- |
| 通道 | Windows Named Pipe（逻辑名 `control`，物理名见 ADR-0011） |
| 帧 | 4 字节 little-endian 长度前缀 + UTF-8 JSON body |
| 最大 frame | 1 MiB |
| 会话 | 一连接可多请求；event 异步发送见 capability |
| 身份 / ACL | ADR-0014 |

---

## 2. 根消息 envelope

### 2.1 Request（`message_type = 1`）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | number | 必须为 `4` |
| `message_type` | number | 必须为 `1` |
| `request_id` | string | 会话内响应对账；不提供幂等 |
| `kind` | number | 见 kind 表 |
| `idempotency_key` | string \| null | 查询必须 null；命令必须 1–128 字节 |
| `payload` | object | 与 kind 强绑定；精确字段集 |

```json
{
  "schema_version": 4,
  "message_type": 1,
  "request_id": "11111111-2222-4333-8444-555555555555",
  "kind": 1,
  "idempotency_key": null,
  "payload": {
    "minimum_api_version": 4,
    "maximum_api_version": 4
  }
}
```

### 2.2 Response（`message_type = 2`）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | number | `4` |
| `message_type` | number | `2` |
| `request_id` | string | 回显 |
| `kind` | number | `1` QueryResult / `2` CommandAccepted / `3` RequestFailed |
| `request_kind` | number | 所响应的 request kind |
| `boundary_error_code` | number | 成功 `0`；失败为稳定 ErrorCode |
| `message_code` | string | 稳定业务码；非本地化文本 |
| `message_arguments` | array | `{ "name", "value" }`，按 name 排序 |
| `payload` | object \| null | 成功强类型；失败必须 null |

命令成功只表示控制面已接受/重放，不表示数据面完成。

### 2.3 Event（`message_type = 3`）

字段同 V3 语义，`schema_version = 4`。见 §8。

---

## 3. 公共规则

### 3.1 查询 vs 命令

| 类别 | kind 范围 | `idempotency_key` | 成功 response.kind |
| --- | --- | --- | --- |
| Query | 1–15 | null | 1 |
| Command | 32–48 | 非空 | 2 |

### 3.2 幂等

同 ADR-0013/V3：同 key + 同 fingerprint → Replayed；同 key + 不同请求 → Conflict。`request_id` 不提供幂等。

### 3.3 分页

| 规则 | 说明 |
| --- | --- |
| `maximum_results` | 1–100 |
| token | opaque，≤1024 字节；禁止解析/拼接/跨 query 复用 |
| 绑定 | caller、kind、filter、排序、（文件浏览）session |
| frame | **编码后整帧 ≤ 1 MiB**。所有返回 `items[]`（或等价数组）的查询必须按编码大小收口：若按 `maximum_results` 取出的一页编码后超限，Service 必须减少本页项数并设置 `continuation_token`，使后续页可续读 |
| encode 失败 | 编码结果仍超过 1 MiB（例如单条记录过大）时，`encode_service_response` 必须明确失败，不得在 send 时静默断连 |

Job list 的 `progress`：仅合并 Worker 监督器缓存中的真实 progress；无缓存时保持 `null`，**不得**注入 synthetic `1/1` 完成进度。

### 3.4 安全与脱敏

- 跨进程密钥：`SecretRef`；口令明文仅允许约定一次性命令字段，不得记日志。
- 不返回 Archive 绝对路径、分卷路径、security descriptor、node 可逆路径、完整选择列表。
- Desktop 不传 Volume GUID / device path 作为浏览或恢复目标输入（使用 opaque token / source_id）。

### 3.5 Request kind 总表

| kind | 名称 | 类型 |
| ---: | --- | --- |
| 1 | GetServiceInfo | Query |
| 2 | ListRecoveryPoints | Query |
| 3 | ListRepositoryConnections | Query |
| 4 | ListSourceInventory | Query |
| 5 | ListJobs | Query |
| 6 | ListSchedules | Query |
| 7 | ListEvents | Query |
| 8 | ListMountSessions | Query |
| 9 | PrepareRestore | Query |
| 10 | ResolveRecoveryPointChain | Query |
| 11 | PlanDeleteRecoveryPoints | Query |
| 12 | GetRecoveryPointLayout | Query |
| 13 | BrowseFileSources | Query |
| 14 | ListRecoveryPointEntries | Query |
| 15 | PrepareFileRestore | Query |
| 16 | GetServiceSettings | Query |
| 32 | AddRepositoryConnection | Command |
| 33 | ImportRepositoryConnection | Command |
| 34 | TestRepositoryConnection | Command |
| 35 | SetDefaultRepository | Command |
| 36 | RemoveRepositoryConnection | Command |
| 37 | StartBackup | Command |
| 38 | CancelJob | Command |
| 39 | StartVerify | Command |
| 40 | StartRestore | Command |
| 41 | MountRecoveryPoint | Command |
| 42 | UnmountSession | Command |
| 43 | UpsertSchedule | Command |
| 44 | DeleteSchedule | Command |
| 45 | SubscribeTaskEvents | Command |
| 46 | AcknowledgeEvents | Command |
| 47 | ExecuteDeletePlan | Command |
| 48 | StartFileRestore | Command |
| 49 | UpdateServiceSettings | Command |

---

## 4. 公共结构与枚举

### 4.1 分页与 CommandAcknowledgement

同 V3 形状（见历史 V3 文档 §4.1–4.2），版本无关字段名保持不变。

### 4.2 ContentKind

| 值 | 名称 |
| ---: | --- |
| 1 | VolumeSet |
| 2 | FileSet |

### 4.3 FileEntryKind

| 值 | 名称 |
| ---: | --- |
| 1 | Directory |
| 2 | File |
| 3 | Reparse |
| 4 | Other |

### 4.4 FileRecursion

| 值 | 名称 |
| ---: | --- |
| 1 | SelfOnly |
| 2 | Recursive |

### 4.5 FileConflictPolicy

| 值 | 名称 |
| ---: | --- |
| 1 | Fail |
| 2 | Replace |
| 3 | Rename |

默认恢复策略为 `Fail`。

### 4.6 FileNodeSelectability

| 值 | 名称 |
| ---: | --- |
| 1 | Selectable |
| 2 | NotSelectable |
| 3 | Unsupported |

### 4.7 既有枚举

`ServiceState`、`BackupType`、`JobOperation`、`ServiceJobState`、
`RepositoryConnectionState`、`RepositoryCatalogState`、`RecoveryPointChainState`、`SourceKind`、
`SourceAvailability`、`AuditSeverity`、`MountSessionState`、`TaskPhase`、`TaskOutcome`、
`ServiceEventKind` 数值与 V3 相同。

`ScheduleTriggerKind` 在 V3 Daily(1)/Weekly(2) 上增加 Monthly(3)。`ScheduleTrigger` 增加
`day_of_month_mask`（bit0=每月 1 日 … bit30=31 日），并将 `local_minute_of_day` 替换为
`local_minutes_of_day`（1–8 个 0–1439 的分钟值，排序去重；任意两时刻间隔 ≥ 30 分钟，含跨午夜）。
Daily/Weekly 的 `day_of_month_mask` 必须为 0；Monthly 必须非 0，且 `weekday_mask` 必须为 0。
所选日在当月不存在时（如 31 日对 2 月）跳到下一个含该日的月份。下次运行取各时刻中最早的候选。

`SourceKind` 扩展：

| 值 | 名称 |
| ---: | --- |
| 1 | Volume |
| 2 | FileSelection（仅 Inventory/摘要展示用，不是设备路径） |

### 4.8 ProtectionSpec（控制面）

互斥 tagged union（JSON object exact keys）：

**volume_set**

```json
{
  "content_kind": 1,
  "volume_set": {
    "source_ids": ["src-..."]
  },
  "file_set": null
}
```

**file_set（创建）**

```json
{
  "content_kind": 2,
  "volume_set": null,
  "file_set": {
    "selections": [
      {
        "node_token": "<opaque>",
        "recursion": 2,
        "display_label": "Documents"
      }
    ],
    "options": {
      "unreadable_policy": 1
    }
  }
}
```

| 约束 | 规则 |
| --- | --- |
| selections | 1..100 |
| node_token | 1..512 字节 opaque |
| display_label | 1..256 UTF-8 字节；非权威 |
| 更新 Schedule | `file_set` 必须 null 或省略变更；不得提交新 token 改源 |
| 查询 Schedule | 返回 `selection_id`、`display_label`、`entry_kind`、`recursion`、`display_chain` |

`unreadable_policy` 固定 `1=fail_job`。其它值拒绝。FI0 已删除 `reparse_policy`（reparse 一律 backup strict fail）。

### 4.9 RecoveryPointSummary V4

exact keys（13）：

`file_uuid`, `backup_set_uuid`, `parent_uuid`, `backup_type`, `content_kind`, `chain_state`,
`created_utc_ms`, `logical_size_bytes`, `stored_size_bytes`, `deduplicated_block_count`,
`deduplicated_logical_bytes`, `source_count`, `has_sidecar`

- `content_kind`：1 或 2；
- 两个 dedup 字段来自 Catalog V2；file_set 固定 0；
- file_set：`has_sidecar` false；`backup_type` 为 Full(1) 或 Incremental(2)；Incremental 时
  `parent_uuid` 为直接父层 file_uuid，Full 时 `parent_uuid` null。

### 4.9a JobSummary V4（FI7）

exact keys（17）：

`job_id`, `trace_id`, `operation`, `state`, `content_kind`, `created_utc_ms`, `started_utc_ms`,
`completed_utc_ms`, `progress`, `message_code`, `source_ids`, `schedule_id` (string|null),
`repository_connection_id`, `requested_backup_type` (u8|null), `effective_backup_type` (u8|null),
`effective_parent_uuid` (string|null), `incremental_downgrade_reason` (u8|null)

- 备份 Job：`schedule_id` 必填（拥有该 Job 的 Schedule）；`requested_backup_type` 为创建时请求类型；
  终端成功后 `effective_*` 来自 TaskResult；
- 非备份：`schedule_id` 必须为 null；后四字段可为 null；volume 备份通常无 `incremental_downgrade_reason`。
- Desktop 以 `schedule_id` 将 Job 状态绑定到 Schedule 行；不得仅用 source 重叠推断。

### 4.10 TaskProgress / TaskResult

在 V3 字段基础上：

**TaskProgress** exact keys：

`schema_version`(=4), `job_id`, `trace_id`, `phase`, `logical_bytes`, `processed_bytes`,
`stored_bytes`, `discovered_entries`, `processed_entries`, `message_code`

- 枚举阶段未知总量时：`logical_bytes` 可为 `null`；`discovered_entries` 递增；
- 所有整数 ≤ `INT64_MAX`；`processed_* <= total_*` 当 total 已知。

**TaskResult** exact keys：

`schema_version`(=4), `job_id`, `trace_id`, `outcome`, `error_code`, `logical_bytes`,
`stored_bytes`, `chunk_count`, `entry_count`, `stream_count`, `deduplicated_block_count`,
`deduplicated_logical_bytes`, `message_code`, `warning_codes`,
`partial_restore` (object|null), `requested_backup_type` (u8|null), `effective_backup_type`
(u8|null), `effective_parent_uuid` (string|null), `incremental_downgrade_reason` (u8|null)

`deduplicated_block_count` / `deduplicated_logical_bytes` 为非负整数；volume_set backup 成功时来自已提交
Footer，其它任务为 0。file_set backup 成功时 `requested_backup_type` / `effective_backup_type` 为
1=full 或 2=incremental；volume 任务与非 backup 结果四个 incremental 投影字段均为 null。

`partial_restore`（仅文件恢复 partial）：

```json
{
  "entries_requested": 10,
  "entries_restored": 7,
  "entries_failed": 3,
  "bytes_restored": 1000,
  "stable_error_codes": ["file_restore.target_collision"]
}
```

不包含路径。

---

## 5. 既有查询 kind 1–12（V4 差异）

除下列差异外，字段语义与 V3 相同，但根 `schema_version`/`api_version` 为 4，且 exact_keys 按 V4 DTO。

| kind | V4 差异 |
| ---: | --- |
| 1 GetServiceInfo | api 仅 4；capabilities 可含 `file.*` |
| 2 ListRecoveryPoints | item 含 `content_kind` |
| 4 ListSourceInventory | 仍以 Volume 为主；不返回任意文件系统树 |
| 5 ListJobs | summary 含 `content_kind` 与 requested/effective backup 投影（FI7）；请求增加 `scope`（1=all / 2=active / 3=terminal）与可选 `from_utc_ms`/`to_utc_ms`（按 `created_utc_ms`）。热路径用 active；Task Log 用 terminal + 时间窗 |
| 6 ListSchedules | summary 含 `content_kind` 与 file selection 安全摘要 |
| 9 PrepareRestore | **仅 volume_set** RP；file_set RP 必须用 kind 15 |
| 12 GetRecoveryPointLayout | volume geometry；file_set 返回稳定 unsupported |
| 16 GetServiceSettings | 控制面偏好：`job_retention_months` ∈ {1,3,6}，默认 3；`updated_utc_ms` |

非法对 file_set 使用 volume-only API → `service.content_kind_mismatch`。

### Job retention（kind 16 / 49）

| 规则 | 说明 |
| --- | --- |
| 存储 | 控制面 SQLite `service_settings` 单行（`id=1`），非 Desktop 本地 QSettings |
| 取值 | `job_retention_months` 仅允许 `1`、`3`、`6`；默认 `3` |
| 月长 | 保留窗口按 **30 天/月** 计算（`months * 30 * 86400000` ms），便于确定性 cutoff |
| 删除对象 | 仅终端 Job（succeeded/failed/cancelled/interrupted）且 `completed_utc_ms < cutoff`；**不**删除 queued/running/cancelling |
| 触发 | Service 启动时 purge；`UpdateServiceSettings` 成功后同事务 purge |
| Get 请求 | kind 16，payload `{}`，capability `service.settings` |
| Get 响应 | `{ "job_retention_months": 3, "updated_utc_ms": 0 }` |
| Update 请求 | kind 49 命令，`idempotency_key` 必填，payload `{ "job_retention_months": 1\|3\|6 }` |
| Update 响应 | `CommandAcknowledgement`；message_code `service.settings_updated` |

---

## 6. 新增查询

### 6.1 kind 13 — BrowseFileSources

**用途：** 分页浏览本机可保护文件树；返回 opaque `node_token`。  
**Capability：** `file.browse`

**请求 payload（exact 3）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `parent_node_token` | string \| null | null = 列出授权 roots |
| `page` | object | maximum_results + continuation_token |
| `include_unavailable` | bool | 是否包含不可访问节点摘要 |

**成功 payload：**

```json
{
  "items": [
    {
      "node_token": "…",
      "display_name": "Users",
      "entry_kind": 1,
      "selectability": 1,
      "has_children": true,
      "is_directory": true,
      "availability": 1,
      "message_code": null
    }
  ],
  "continuation_token": null
}
```

**FileSourceNode** exact keys：

`node_token`, `display_name`, `entry_kind`, `selectability`, `has_children`, `is_directory`,
`availability`, `message_code`

规则：

- `display_name` 来自 UTF-16LE 的可展示投影，可含替换字符；**不是**恢复权威名称；
- 不返回绝对路径、volume path、file id；
- token TTL 默认 15 分钟；绑定 caller SID + connection session；
- 最大 active tokens / connection：4096（按 session 计数，不是进程全局）；
- 根列表首页（`parent_node_token=null` 且 `page.continuation_token=null`）必须先清空该
  session 上已有 node tokens，再 mint 本页；续页保留本轮根列表已发 token。Desktop 折叠/
  重载树不单独释放 token，依赖根列表重置与 TTL/断连清理；
- 根列表（`parent_node_token=null`）：先返回当前用户可映射的特殊目录（固定英文 label：
  Desktop、Downloads、Documents、Pictures、Music、Videos），再返回授权盘符卷；无法映射到授权卷
  的特殊目录省略。子目录排序仍为 `entry_kind`（dir first）+ display_name 二进制序 + token 次序稳定化；
- 过期/错绑 token → `file_browse.token_invalid`；
- 单 connection 超过 4096 active tokens → `file_browse.token_limit`；
- 未授权 volume → 不出现或 `availability=Unavailable`。

**示例请求：**

```json
{
  "schema_version": 4,
  "message_type": 1,
  "request_id": "…",
  "kind": 13,
  "idempotency_key": null,
  "payload": {
    "parent_node_token": null,
    "page": { "maximum_results": 50, "continuation_token": null },
    "include_unavailable": false
  }
}
```

### 6.2 kind 14 — ListRecoveryPointEntries

**用途：** 分页列出已认证 file_set Recovery Point 的子条目。  
**Capability：** `file.recover_browse`

**请求 payload（exact 5）：**

| 字段 | 类型 |
| --- | --- |
| `repository_connection_id` | string \| null |
| `recovery_point_id` | string |
| `parent_entry_id` | string | 十进制 u64 文本；根用 `"0"` |
| `page` | ServicePageRequest |
| `archive_secret_ref` | string \| null | 需要时 SecretRef |

**成功 payload：**

```json
{
  "repository_connection_id": "conn-01",
  "recovery_point_id": "…",
  "parent_entry_id": "0",
  "index_generation": "sha256:…",
  "items": [
    {
      "entry_id": "1",
      "display_name": "readme.txt",
      "entry_kind": 2,
      "logical_size_bytes": 1234,
      "has_children": false,
      "message_code": null
    }
  ],
  "continuation_token": null
}
```

**规则：**

- 必须先打开并认证 tip→Full 完整链，再分页 tip Index；失败区分
  `file_recover.credential_required` / `file_recover.credential_failed` /
  `file_recover.corrupt` / `file_recover.parent_missing` /
  `file_recover.parent_reference_invalid` / `file_recover.chain_depth_limit`；
- 响应 `index_generation` 为 tip Index root digest；continuation 另绑定完整
  `chain_generation`（各层 index digest）+ tip digest + parent_entry_id + reader offset；
- 不返回 stream offset、descriptor、Archive key；
- volume_set RP → `service.content_kind_mismatch`；
- Catalog-only 或链不完整 → 不得返回条目。

### 6.3 kind 15 — PrepareFileRestore

**用途：** 认证选择闭包与目标，生成短期 preflight token。  
**Capability：** `file.restore`

**请求 payload（exact 7）：**

| 字段 | 类型 |
| --- | --- |
| `repository_connection_id` | string \| null |
| `recovery_point_id` | string |
| `entry_ids` | string[] | 1..10000 个十进制 u64 |
| `target_node_token` | string | 浏览得到的目录 token |
| `conflict_policy` | number | FileConflictPolicy |
| `archive_secret_ref` | string \| null |
| `restore_security` | bool | true 时应用 Owner/Group/DACL/SACL；FAT32 目标必须为 false |

FI0：不再有 `restore_ads`；Archive 不含 ADS，恢复只写 unnamed main stream。

**成功 payload — FileRestorePreflight：**

```json
{
  "preflight_token": "…",
  "repository_connection_id": "conn-01",
  "recovery_point_id": "…",
  "entry_count": 42,
  "logical_size_bytes": 1000000,
  "target_free_bytes": 50000000000,
  "conflict_policy": 1,
  "expires_utc_ms": 1785603600000,
  "restore_eligible": true,
  "message_code": "file_restore.preflight_ok",
  "capability_warnings": []
}
```

durable preflight 保存：repository UUID、RP UUID、tip index root digest、chain generation digest、
chain depth、selected entry IDs、target root identity、policy、counts、expiration。
Prepare 在写前解析选择闭包内全部 parent stream 引用到 local owner。
**不**保存 Archive/target 绝对路径、Secret、文件树。

目标缺 ACL 能力且 `restore_security=true` → RequestFailed
`file_restore.target_capability_missing`。选择闭包存在超过目标单文件上限的文件 → RequestFailed
`file_restore.target_file_too_large`。FAT32 单文件上限为 4 GiB - 1。

---

## 7. 命令差异与新增

### 7.1 kind 37 — StartBackup

payload 保持 repository/schedule/job 引用形状；若 Schedule 为 file_set：

- 仅 Full；
- 重新校验 source availability；
- Job `content_kind=2`；source summary 使用 opaque selection IDs，不写路径。

### 7.2 kind 43 — UpsertSchedule

**请求 payload（exact keys）：**

| 字段 | 类型 |
| --- | --- |
| `schedule_id` | string \| null | null=创建 |
| `display_name` | string |
| `enabled` | bool |
| `trigger` | ScheduleTrigger |
| `repository_connection_id` | string \| null |
| `backup_type` | number | file_set 允许 Full(1) 或 Incremental(2)，禁止 Differential |
| `deduplication_enabled` | bool | volume_set 创建默认 true 且创建后冻结；file_set 必须 false |
| `protection` | ProtectionSpec | §4.8 |
| `encryption` | object \| null | 既有形状 |

创建 file_set：解析 token → durable selection；规范化/去重；事务写入。  
更新：保护源冻结；改变 `protection` 选择 → Conflict `schedule.source_frozen`。volume_set 更新不得改变
`deduplication_enabled`；该字段必须进入幂等请求指纹。Service 将 `backup_type` 一律写为 Incremental。
更换 `repository_connection_id` 清空增量 tip。

**列表 ScheduleSummary** 增加：`content_kind`, `deduplication_enabled`, `selection_summaries`（`selection_id`,
`display_label`, `entry_kind`, `recursion`, `display_chain`）。`display_chain` 为 volume-relative UI 名
（整卷选择则为 `[display_label]`），不是路径。file_set 的 `deduplication_enabled` 固定 false。

### 7.3 kind 48 — StartFileRestore

**Capability：** `file.restore`  
**幂等命令**

**请求 payload（exact 3）：**

| 字段 | 类型 |
| --- | --- |
| `preflight_token` | string |
| `confirmed` | bool | 必须 true |
| `archive_secret_ref` | string \| null |

**成功：** CommandAcknowledgement，`resource_id = job_id`。

规则：

- token 唯一占用；重放同 fingerprint 返回 Replayed；
- 重查 generation、index digest、target identity；
- 构造 Worker schema 4 File Restore Job；
- volume PrepareRestore token 不可用于本命令。

```json
{
  "schema_version": 4,
  "message_type": 1,
  "request_id": "…",
  "kind": 48,
  "idempotency_key": "intent-uuid-…",
  "payload": {
    "preflight_token": "…",
    "confirmed": true,
    "archive_secret_ref": "dpapi-lm:…"
  }
}
```

### 7.4 其它命令 32–47

字段级形状与 V3 相同（版本号 4），除非 Contracts 在 F1 明确收紧。`StartRestore`（40）仅 volume。

---

## 8. 异步 Event

Event envelope：

| 字段 | 类型 |
| --- | --- |
| `schema_version` | 4 |
| `message_type` | 3 |
| `subscription_id` | string |
| `sequence` | number | 从 1 单调递增 |
| `event_kind` | number |
| `message_code` | string |
| `message_arguments` | array |
| `payload` | object |

event kind 仍为 TaskProgress=1、TaskCompleted=2、MountSessionChanged=3。  
进度 `message_code` 可使用 `file_backup.*` / `file_restore.*`（见产品上限文档）。

窗口最大 128，默认 64；ack 语义不变。

---

## 9. 权限与幂等摘要

| kind | 权限要点 | 幂等 |
| ---: | --- | --- |
| 13 | 调用者可读浏览的 volume 子集 | 查询 |
| 14 | RP 所属 connection 授权 + 凭据 | 查询 |
| 15 | RP 授权 + 目标目录可写预检 | 查询（token 侧效仅内存/短期 store） |
| 43 | owner；file token 绑定 caller | 命令 |
| 48 | preflight owner；confirmed | 命令 |

---

## 10. Frame 上限示例

单 frame ≤ 1 MiB（1048576 字节）。Browse 页在 `maximum_results=100` 时若 display_name 较大，Service 必须减少
items 保证 framing。token 与 UUID 字段计入 frame。

---

## 11. 能力与未接线

F0 只冻结 wire。实现阶段：

- F6 完成后可声明 `file.browse` / `schedule.file_set`（及 file_set 备份编排相关能力）；
- F7 完成后可声明 `file.recover_browse`（及 `recovery_point.verify` 覆盖 file_set）；
- F8 完成后可声明 `file.restore`（PrepareFileRestore + StartFileRestore）。

未声明 capability 的 kind → `service.capability_unavailable`，无副作用。当前 runtime 在
`file.browse` 可用时一并声明 `file.restore`。

---

## 12. 维护说明

- 字段变更需要新 schema 或新 ADR；
- 同步更新 Contracts、Service codec、Desktop codec 与本文；
- 禁止为旧 Desktop 保留 V3 分支。
