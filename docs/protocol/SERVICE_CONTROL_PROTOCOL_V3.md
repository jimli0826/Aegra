# 本地 Service 控制面协议 V3（详细说明）

| 属性 | 内容 |
| --- | --- |
| 状态 | 权威 wire 说明（与 codec 同步） |
| Schema | `schema_version = 3` |
| API | `api_version = 3` |
| 决策依据 | [ADR-0013](../adr/0013-service-control-protocol-v3.md)、[ADR-0011](../adr/0011-local-service-desktop-ipc.md) |
| 编解码实现 | `apps/service`（`service_protocol_*_json.cpp`）、`apps/desktop`（`service_protocol*.cpp`） |
| 契约 DTO | `src/contracts`（不依赖 JSON） |

本文档描述 **Desktop ↔ 本机 Management Service** 上每一条 request/response/event 的用途、字段、枚举与示例。  
字段集合为 **精确集合**（`exact_keys`）：多字段、少字段或未知字段一律拒绝。  
冲突时以 Contracts 校验与 Service/Desktop codec 为准；行为变更须同步更新本文档。

---

## 目录

1. [传输与帧](#1-传输与帧)
2. [根消息 envelope](#2-根消息-envelope)
3. [公共规则](#3-公共规则)
4. [公共结构与枚举](#4-公共结构与枚举)
5. [查询类协议（kind 1–12）](#5-查询类协议kind-112)
6. [命令类协议（kind 32–47）](#6-命令类协议kind-3247)
7. [异步 Event](#7-异步-event)
8. [能力与未接线行为](#8-能力与未接线行为)
9. [维护说明](#9-维护说明)

---

## 1. 传输与帧

| 项 | 规则 |
| --- | --- |
| 通道 | Windows Named Pipe（逻辑名 `control`，物理名见 ADR-0011） |
| 帧 | 4 字节 little-endian 长度前缀 + UTF-8 JSON body |
| 最大 frame | 64 KiB |
| 会话 | 一连接可多请求；当前 Host 按请求串行处理（event 异步发送见 S3 能力） |
| 身份 / ACL | 见 [ADR-0014](../adr/0014-windows-service-ipc-security.md) |

---

## 2. 根消息 envelope

### 2.1 Request（`message_type = 1`）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | number | 必须为 `3` |
| `message_type` | number | 必须为 `1` |
| `request_id` | string | 会话内响应对账；不提供幂等 |
| `kind` | number | 见下文 kind 表 |
| `idempotency_key` | string \| null | **查询必须 null**；**命令必须非空字符串**（1–128 字节） |
| `payload` | object | 与 `kind` 强绑定；精确字段集 |

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "11111111-2222-4333-8444-555555555555",
  "kind": 1,
  "idempotency_key": null,
  "payload": {
    "minimum_api_version": 3,
    "maximum_api_version": 3
  }
}
```

### 2.2 Response（`message_type = 2`）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | number | `3` |
| `message_type` | number | `2` |
| `request_id` | string | 回显请求 |
| `kind` | number | `1` QueryResult / `2` CommandAccepted / `3` RequestFailed |
| `request_kind` | number | 所响应的 request kind |
| `boundary_error_code` | number | 成功必须为 `0`（None）；失败为稳定错误码 |
| `message_code` | string | 稳定业务/错误码；**非本地化文本** |
| `message_arguments` | array | `{ "name", "value" }`，按 name 排序 |
| `payload` | object \| null | 成功时为强类型结果；失败必须 `null` |

**Response kind：**

| 值 | 名称 | 含义 |
| ---: | --- | --- |
| 1 | QueryResult | 查询成功，`payload` 为查询结果 |
| 2 | CommandAccepted | 命令已接受或幂等重放，`payload` 为 `CommandAcknowledgement` |
| 3 | RequestFailed | 失败，`payload = null` |

命令成功 **只表示控制面已接受/重放**，不表示 Backup/Restore 等数据面完成。长任务完成看 Job 列表与 task event。

### 2.3 Event（`message_type = 3`）

见 [§7](#7-异步-event)。

---

## 3. 公共规则

### 3.1 查询 vs 命令

| 类别 | kind 范围 | `idempotency_key` | 成功 response.kind |
| --- | --- | --- | --- |
| Query | 1–12 | 必须 `null` | `1` QueryResult |
| Command | 32–47 | 必须非空 | `2` CommandAccepted |

### 3.2 幂等（命令）

1. 客户端为 **一次用户意图** 生成稳定 `idempotency_key`（常见 UUID）。
2. Service 在控制面 `commands` 表按 key 唯一存储：`request_fingerprint`、`command_id`、可选 `resource_id`。
3. **同 key + 同请求** → `disposition = 2`（Replayed），不重复副作用。
4. **同 key + 不同请求** → Conflict（实现完整路径会校验 fingerprint）。
5. **新意图**（再次点创建）→ 新 key。
6. `request_id` 仅匹配本次 RPC 响应，重试时可换新；幂等依赖 `idempotency_key`。

### 3.3 分页

多数列表使用：

```json
{
  "maximum_results": 50,
  "continuation_token": null
}
```

| 规则 | 说明 |
| --- | --- |
| 上限 | 每页最多 100 |
| token | 不透明，≤1024 字节；不得解析/拼接/跨 query 复用 |
| 有效性 | 仅对相同 caller、kind、filter、排序有效 |
| frame | 64 KiB 上限下 Service 可返回更少项 |

响应列表页通用形状：

```json
{
  "items": [ ... ],
  "continuation_token": null
}
```

（Recovery Point Catalog 页结构见 kind 2，略有不同。）

### 3.4 安全与脱敏

- 跨进程密钥定位用 `SecretRef` 字符串（Archive 口令为 `dpapi-lm:<entropy_id>:<base64>`，
  Schedule 的 entropy 为 `schedule_id`）；口令明文仅允许在约定的 **一次性命令字段** 中出现，且不得记日志。
- 响应只含稳定 `message_code` 与结构化参数；不含异常原文、路径秘密、明文密码。
- Desktop 不传 Archive 绝对路径、Volume GUID、链数组等到控制面（使用 opaque ID）。

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

---

## 4. 公共结构与枚举

### 4.1 `ServicePageRequest` / 通用列表页

**请求 `page`：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `maximum_results` | number | 1–100 |
| `continuation_token` | string \| null | 首页 null |

**响应页（多数列表）：**

| 字段 | 类型 |
| --- | --- |
| `items` | array |
| `continuation_token` | string \| null |

### 4.2 `CommandAcknowledgement`（命令成功 payload）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `command_id` | string | 稳定命令 ID |
| `disposition` | number | `1` Accepted / `2` Replayed |
| `resource_id` | string \| null | 如 job_id、schedule_id、connection_id |
| `event_subscription` | object \| null | 仅 SubscribeTaskEvents 时非 null |

**`event_subscription`（lease）：**

| 字段 | 类型 |
| --- | --- |
| `subscription_id` | string |
| `resume_token` | string |
| `next_sequence` | number |
| `maximum_unacknowledged_events` | number |

```json
{
  "command_id": "cmd-abc",
  "disposition": 1,
  "resource_id": "sch-001",
  "event_subscription": null
}
```

### 4.3 常用枚举

**ServiceState（ServiceInfo.state）**

| 值 | 含义 |
| ---: | --- |
| 1 | Starting |
| 2 | Ready |
| 3 | Stopping |

**BackupType / PersonalBackupType**

| 值 | 含义 |
| ---: | --- |
| 1 | Full |
| 2 | Incremental |
| 3 | Differential（当前产品多处拒绝） |

**JobOperation**

| 值 | 含义 |
| ---: | --- |
| 1 | Backup |
| 2 | Restore |
| 3 | Verify |
| 4 | Export |

**ServiceJobState**

| 值 | 含义 |
| ---: | --- |
| 1 | Queued |
| 2 | Running |
| 3 | Cancelling |
| 4 | Succeeded |
| 5 | Failed |
| 6 | Cancelled |
| 7 | Interrupted |

**ScheduleTriggerKind**

| 值 | 含义 |
| ---: | --- |
| 1 | Daily |
| 2 | Weekly |

**RepositoryConnectionState**

| 值 | 含义 |
| ---: | --- |
| 1 | Available |
| 2 | Unavailable |

**RepositoryCatalogState**

| 值 | 含义 |
| ---: | --- |
| 1 | NotConfigured |
| 2 | CatalogReady（≠ Archive 已认证 / Restore Ready） |

**RecoveryPointChainState（Catalog 摘要）**

| 值 | 含义 |
| ---: | --- |
| 1 | Complete |
| 2 | Incomplete |

**SourceKind / SourceAvailability**

| 枚举 | 值 | 含义 |
| --- | ---: | --- |
| SourceKind | 1 | Volume |
| SourceAvailability | 1 | Available |
| SourceAvailability | 2 | Unavailable |

**AuditSeverity**

| 值 | 含义 |
| ---: | --- |
| 1 | Information |
| 2 | Warning |
| 3 | Error |
| 4 | Critical |

**MountSessionState**

| 值 | 含义 |
| ---: | --- |
| 1 | Mounting |
| 2 | Mounted |
| 3 | Unmounting |
| 4 | Failed |

**TaskPhase**

| 值 | 含义 |
| ---: | --- |
| 0 | Unspecified |
| 1 | Preparing |
| 2 | Reading |
| 3 | Transforming |
| 4 | Writing |
| 5 | Committing |
| 6 | Completed |

**TaskOutcome**

| 值 | 含义 |
| ---: | --- |
| 1 | Succeeded |
| 2 | SucceededWithWarning |
| 3 | Failed |
| 4 | Cancelled |

**ServiceEventKind**

| 值 | 含义 |
| ---: | --- |
| 1 | TaskProgress |
| 2 | TaskCompleted |
| 3 | MountSessionChanged |

### 4.4 嵌套 DTO 字段集

**`TaskProgress`（8 字段）**

`schema_version`, `job_id`, `trace_id`, `phase`, `logical_bytes`, `processed_bytes`, `stored_bytes`, `message_code`

**`TaskResult`（10 字段）**

`schema_version`, `job_id`, `trace_id`, `outcome`, `error_code`, `logical_bytes`, `stored_bytes`, `chunk_count`, `message_code`, `warning_codes`

**`ScheduleTrigger`（4 字段）**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `kind` | number | Daily/Weekly |
| `local_minute_of_day` | number | 0–1439 |
| `weekday_mask` | number | bit0=Sunday … bit6=Saturday；Daily 常用 0 |
| `timezone_id` | string | 如 `"UTC"` |

**`message_arguments` 元素**

```json
{ "name": "job_id", "value": "job-…" }
```

---

## 5. 查询类协议（kind 1–12）

以下每条：**`idempotency_key` 必须为 `null`**；成功时 `response.kind = 1`。

---

### 5.1 kind 1 — GetServiceInfo

**用途：** 握手；协商 API 版本；读取 Service 状态与 capability 列表。Desktop 必须按 capability 启停 UI，不得仅凭 kind 号猜测能力。

**请求 payload（2 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `minimum_api_version` | number | 客户端可接受下限 |
| `maximum_api_version` | number | 客户端可接受上限 |

**成功 payload — `ServiceInfo`（5 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `minimum_api_version` | number | Service 支持下限 |
| `api_version` | number | 当前选用版本（3） |
| `state` | number | ServiceState |
| `service_version` | string | 构建/产品版本串 |
| `capabilities` | string[] | 如 `service.info`、`repository.list` |

**示例请求：** 见 §2.1。

**示例成功 payload：**

```json
{
  "minimum_api_version": 3,
  "api_version": 3,
  "state": 2,
  "service_version": "0.0.0-dev",
  "capabilities": ["service.info", "repository.list", "backup.start"]
}
```

**失败要点：** 版本范围不相交 → `UnsupportedVersion` / `service.api_version_unsupported`。

---

### 5.2 kind 2 — ListRecoveryPoints

**用途：** 分页列出某 Repository connection 的 Catalog Recovery Point 摘要。

**请求 payload（2 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `repository_connection_id` | string \| null | 多连接时指定；单库模式可为 null |
| `page` | object | `maximum_results` + `continuation_token`（见 §4.1） |

**成功 payload — `ServiceRecoveryPointPage`（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `repository_connection_id` | string \| null |
| `catalog` | object |

**`catalog`（4 字段）：**

| 字段 | 类型 |
| --- | --- |
| `state` | number | RepositoryCatalogState |
| `repository_uuid` | string |
| `items` | array of RecoveryPointSummary |
| `continuation_token` | string \| null |

**`RecoveryPointSummary`（10 字段）：**

`file_uuid`, `backup_set_uuid`, `parent_uuid` (string\|null), `backup_type`, `chain_state`, `created_utc_ms`, `logical_size_bytes`, `stored_size_bytes`, `source_count`, `has_sidecar`

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 2,
  "idempotency_key": null,
  "payload": {
    "repository_connection_id": "conn-01",
    "page": {
      "maximum_results": 50,
      "continuation_token": null
    }
  }
}
```

**示例成功 payload 片段：**

```json
{
  "repository_connection_id": "conn-01",
  "catalog": {
    "state": 2,
    "repository_uuid": "01234567-89ab-4cde-8f01-23456789abcd",
    "items": [
      {
        "file_uuid": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        "backup_set_uuid": "ffffffff-0000-4000-8000-000000000001",
        "parent_uuid": null,
        "backup_type": 1,
        "chain_state": 1,
        "created_utc_ms": 1720000000000,
        "logical_size_bytes": 1000000000,
        "stored_size_bytes": 400000000,
        "source_count": 2,
        "has_sidecar": true
      }
    ],
    "continuation_token": null
  }
}
```

---

### 5.3 kind 3 — ListRepositoryConnections

**用途：** 分页列出本机已登记的 Repository 连接（不返回磁盘根路径）。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `state` | number \| null | 过滤 RepositoryConnectionState；null=全部 |

**成功 payload：** `{ "items": [RepositoryConnectionSummary…], "continuation_token": … }`

**`RepositoryConnectionSummary`（6 字段）：**

`connection_id`, `display_name`, `locator`, `state`, `is_default`, `capabilities`

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 3,
  "idempotency_key": null,
  "payload": {
    "page": { "maximum_results": 100, "continuation_token": null },
    "state": null
  }
}
```

---

### 5.4 kind 4 — ListSourceInventory

**用途：** 列出本机可备份 Source（Volume 等）；`source_id` 由 Service 生成，Desktop 不得伪造设备路径。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `include_unavailable` | bool |

**成功 payload：** 通用列表页。

**`SourceInventoryItem`（16 字段）：**

`source_id`, `display_name`, `kind`, `availability`, `capacity_bytes`, `free_bytes`, `disk_capacity_bytes`, `is_system`, `is_read_only`, `is_selectable`, `disk_number`, `mount_letter`, `volume_label`, `health_status`, `partition_style`, `media_type`

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 4,
  "idempotency_key": null,
  "payload": {
    "page": { "maximum_results": 100, "continuation_token": null },
    "include_unavailable": true
  }
}
```

---

### 5.5 kind 5 — ListJobs

**用途：** 分页列出控制面 Job 摘要与可选进度。

**请求 payload（3 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `operation` | number \| null | JobOperation 过滤 |
| `state` | number \| null | ServiceJobState 过滤 |

**成功 payload：** 通用列表页。

**`JobSummary`（11 字段）：**

`job_id`, `trace_id`, `operation`, `state`, `created_utc_ms`, `started_utc_ms` (null\|number), `completed_utc_ms` (null\|number), `progress` (null\|TaskProgress), `message_code`, `source_ids` (string[]), `repository_connection_id` (null\|string)

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 5,
  "idempotency_key": null,
  "payload": {
    "page": { "maximum_results": 50, "continuation_token": null },
    "operation": 1,
    "state": null
  }
}
```

---

### 5.6 kind 6 — ListSchedules

**用途：** 分页列出备份计划。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `enabled` | bool \| null | null=全部 |

**成功 payload：** 通用列表页。

**`ScheduleSummary`（10 字段）：**

`schedule_id`, `display_name`, `enabled`, `source_ids`, `repository_connection_id`, `backup_type`, `trigger`, `next_run_utc_ms` (null\|number), `exclude_page_and_hibernation_files`, `encryption_enabled`

**不含** 密码或 SecretRef。

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 6,
  "idempotency_key": null,
  "payload": {
    "page": { "maximum_results": 100, "continuation_token": null },
    "enabled": null
  }
}
```

---

### 5.7 kind 7 — ListEvents

**用途：** 分页查询审计/事件日志（控制面 audit events）。

**请求 payload（5 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `minimum_severity` | number \| null |
| `from_utc_ms` | number \| null |
| `to_utc_ms` | number \| null |
| `correlation_id` | string \| null |

**成功 payload：** 通用列表页。

**`AuditEventSummary`（6 字段）：**

`event_id`, `created_utc_ms`, `severity`, `message_code`, `message_arguments`, `correlation_id`

---

### 5.8 kind 8 — ListMountSessions

**用途：** 分页列出当前 Mount 会话。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `page` | ServicePageRequest |
| `state` | number \| null | MountSessionState |

**成功 payload：** 通用列表页。

**`MountSessionSummary`（8 字段）：**

`session_id`, `recovery_point_id`, `state`, `mount_point`, `source_disk_number`,
`disk_size_bytes`, `started_utc_ms`, `message_code`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `session_id` | string | 稳定 id（如 `mount-` + hex） |
| `recovery_point_id` | string | 恢复点 id |
| `state` | number | MountSessionState |
| `mount_point` | string | 盘符列表展示，如 `H:` 或 `H: I:`；尚未分配时可为空 |
| `source_disk_number` | uint32 | Manifest 源盘号 |
| `disk_size_bytes` | uint64 | 源盘/数据大小（字节） |
| `started_utc_ms` | uint64 | 会话开始时间 |
| `message_code` | string | 状态消息码 |

---

### 5.9 kind 9 — PrepareRestore

**用途：** Restore 预检；返回短期 `preflight_token` 与容量/链信息。**不**接受路径/key/链数组。

**请求 payload（6 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `repository_connection_id` | string | |
| `recovery_point_id` | string | |
| `target_source_id` | string | 整盘：`disk.N`；卷：`vol.{guid}`（Inventory opaque id） |
| `source_disk_number` | uint32 | 整盘：Manifest 源盘号；卷还原传 `0` |
| `source_volume_index` | uint32 | 卷：Manifest `volumes[].volume_index`；整盘传 `0` |
| `archive_password` | string | 打开加密 Archive；未加密 `""`；不记日志 |

**整盘还原（Full 或 Incremental tip）：** `target_source_id` 必须为 `disk.N`；Service 经
`resolve_chain` 得到 base-first 链，校验目标非系统盘、容量 ≥ 源盘、链完整；指纹前缀 `diskc|…`。
Worker 再认证链并要求 tip 含可用 `raw_layout`。`chain_depth` 为链长度（Full-only 为 1）。系统盘目标在线拒绝（需 PE）。

**卷还原（Full 或 Incremental tip）：** `target_source_id` 必须为 `vol.…`；Service 校验目标非系统、
非只读、Available、容量 ≥ 源卷 `total_size`；指纹前缀 `volc|{volume_index}|{size}|…`。
Start 时 `target_ref` 为 Inventory `stable_key`（canonical Volume GUID Path）。

**成功 payload — `RestorePreflight`（10 字段）：**

`preflight_token`, `repository_connection_id`, `recovery_point_id`, `target_source_id`, `logical_size_bytes`, `target_capacity_bytes`, `chain_depth`, `expires_utc_ms`, `restore_eligible`, `message_code`

**示例请求（整盘）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 9,
  "idempotency_key": null,
  "payload": {
    "repository_connection_id": "conn-01",
    "recovery_point_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
    "target_source_id": "disk.2",
    "source_disk_number": 0,
    "source_volume_index": 0,
    "archive_password": ""
  }
}
```

**示例请求（卷）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 9,
  "idempotency_key": null,
  "payload": {
    "repository_connection_id": "conn-01",
    "recovery_point_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
    "target_source_id": "vol.xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "source_disk_number": 0,
    "source_volume_index": 1,
    "archive_password": ""
  }
}
```

---

### 5.10 kind 10 — ResolveRecoveryPointChain

**用途：** 解析恢复点祖先链（base-first 层列表）及 restore/mount/verify 资格。

**请求 payload — `RecoveryPointRef`（3 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `repository_connection_id` | string | |
| `recovery_point_id` | string | |
| `archive_password` | string | 打开加密 Archive 时用；未加密 `""`；不记日志 |

**成功 payload — `RecoveryPointChainResult`（7 字段）：**

`repository_connection_id`, `recovery_point_id`, `layers`[], `restore_eligible`, `mount_eligible`, `verify_eligible`, `message_code`

**`RecoveryPointChainLayer`（6 字段）：**

`recovery_point_id`, `backup_type`, `parent_recovery_point_id` (null\|string), `structural_state`, `authentication_state`, `chain_state`

**结构/认证/链完整枚举：**

| 枚举 | 值 | 含义 |
| --- | ---: | --- |
| structural_state | 1/2/3 | Complete / Incomplete / Corrupt |
| authentication_state | 1–4 | NotAttempted / Authenticated / Failed / CredentialRequired |
| chain_state（层） | 1/2/3 | Complete / Incomplete / Invalid |

---

### 5.11 kind 11 — PlanDeleteRecoveryPoints

**用途：** 生成删除计划（含受影响成员与过期时间），**不**立即删除。

**请求 payload：** 同 kind 10 的 `RecoveryPointRef`（3 字段，含可选 `archive_password`）。

**成功 payload — `DeletePlanSummary`（6 字段）：**

`plan_token`, `operation_id`, `repository_connection_id`, `root_recovery_point_id`, `targets`[], `expires_utc_ms`

**`DeletePlanTargetSummary`（3 字段）：**

`recovery_point_id`, `catalog_generation`, `member_count`

实际删除见 kind 47 `ExecuteDeletePlan`。

---

### 5.12 kind 12 — GetRecoveryPointLayout

**用途：** 打开指定恢复点 Archive Manifest，返回 Source Disk/Volume 几何，供 Restore UI 绘制源盘布局。

**请求 payload：** 同 `RecoveryPointRef`（3 字段）。

**成功 payload — `RecoveryPointLayout`（4 字段）：**

`repository_connection_id`, `recovery_point_id`, `disks`[], `volumes`[]

**`RecoveryPointSourceDisk`（6 字段）：**

`disk_number`, `disk_size_bytes`, `partition_style`, `model`, `media_type`, `partitions`[]

**`RecoveryPointSourcePartition`（9 字段）：**

`partition_number`, `offset_bytes`, `size_bytes`, `is_active`, `mbr_type`, `gpt_type_guid`, `gpt_name`, `volume_label`, `filesystem`

**`RecoveryPointSourceVolume`（6 字段）：**

`volume_index`, `letter`, `label`, `filesystem`, `total_size_bytes`, `extents`[]

**`RecoveryPointSourceExtent`（5 字段）：**

`disk_number`, `partition_number`, `physical_offset`, `volume_offset`, `length`

**示例请求：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 12,
  "idempotency_key": null,
  "payload": {
    "repository_connection_id": "conn-01",
    "recovery_point_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
    "archive_password": ""
  }
}
```

---

## 6. 命令类协议（kind 32–47）

以下每条：**`idempotency_key` 必须为非空字符串**；成功时 `response.kind = 2`，`payload` 为 `CommandAcknowledgement`（§4.2）。

---

### 6.1 kind 32 — AddRepositoryConnection

**用途：** 新增 Repository 连接登记（控制面 + 校验 locator）。

**请求 payload — `RepositoryConnectionInput`（3 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `display_name` | string | 显示名 |
| `locator` | string | 稳定定位符（非 UI 随意路径展示） |
| `credential_ref` | string \| null | SecretRef 值；无凭据用 null |

**成功 ack：** `resource_id` 常为新 `connection_id`。

**示例：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 32,
  "idempotency_key": "idem-add-repo-1",
  "payload": {
    "display_name": "Local Disk E",
    "locator": "local://…",
    "credential_ref": null
  }
}
```

---

### 6.2 kind 33 — ImportRepositoryConnection

**用途：** 导入已有 Repository（payload 形状同 kind 32）。

**请求 payload：** 同 `RepositoryConnectionInput`（3 字段）。

---

### 6.3 kind 34 — TestRepositoryConnection

**用途：** 测试连接可用性（不改变 default）。

**请求 payload — `ResourceRef`（1 字段）：**

```json
{ "resource_id": "conn-01" }
```

---

### 6.4 kind 35 — SetDefaultRepository

**用途：** 将指定 connection 设为默认。

**请求 payload：** `ResourceRef`（`resource_id` = connection_id）。

---

### 6.5 kind 36 — RemoveRepositoryConnection

**用途：** 删除控制面中的连接引用（不删除 Repository 数据面内容的业务规则见模块文档）。

**请求 payload：** `ResourceRef`。

---

### 6.6 kind 37 — StartBackup

**用途：** 按已有 Schedule 提交备份 Job（queued intent）。源卷、Repository、exclude/加密与口令密文均由
Service 从 `schedules` 记录展开；Client **不得** 再提交这些字段。

**请求 payload（2 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schedule_id` | string | 必填；对应已创建 Schedule |
| `backup_type` | number | 1 Full / 2 Incremental（本次运行类型；增量父点由 Service 自动选择） |

Service 展开：`source_ids`、`repository_connection_id`、`exclude_page_and_hibernation_files`、
`encryption_enabled`、`backup_set_uuid`；加密时从 `archive_password_protected` 取
`dpapi-lm:<schedule_id>:<base64>` 交给 Worker。

**成功 ack：** `resource_id` 为 `job_id`。

**示例（全量）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 37,
  "idempotency_key": "idem-backup-run-1",
  "payload": {
    "schedule_id": "sch-001",
    "backup_type": 1
  }
}
```

**示例（增量）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 37,
  "idempotency_key": "idem-backup-sched-run-1",
  "payload": {
    "schedule_id": "sch-001",
    "backup_type": 2
  }
}
```

---

### 6.7 kind 38 — CancelJob

**用途：** 请求取消进行中的 Job。

**请求 payload：** `ResourceRef`（`resource_id` = job_id）。

---

### 6.8 kind 39 — StartVerify

**用途：** 启动对指定恢复点的 Verify Job。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `repository_connection_id` | string |
| `recovery_point_id` | string |

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 39,
  "idempotency_key": "idem-verify-1",
  "payload": {
    "repository_connection_id": "conn-01",
    "recovery_point_id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"
  }
}
```

---

### 6.9 kind 40 — StartRestore

**用途：** 在用户确认后，用预检 token 启动 Restore Job。

**请求 payload（5 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `preflight_token` | string | 来自 kind 9 |
| `confirmed` | bool | 必须为 `true` |
| `archive_password` | string | 与预检一致；加密 Archive 必填；未加密 `""`；不记日志 |
| `preserve_disk_signature` | bool | 保留源盘 MBR signature / GPT DiskId（默认 true） |
| `auto_expand_last_partition` | bool | 目标更大时扩展末数据分区 + NTFS/ReFS（默认 true） |

- **整盘（指纹 `diskc|…`）：** 向 Worker 提交 `disk_restore=true` + `\\.\PhysicalDriveN`，并透传
  `preserve_disk_signature` / `auto_expand_last_partition`。
- **卷（指纹 `volc|…`）：** 向 Worker 提交 `disk_restore=false` + `source_volume_index` +
  Volume GUID Path `target_ref`；盘签名/扩容选项忽略。

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 40,
  "idempotency_key": "idem-restore-1",
  "payload": {
    "preflight_token": "pft-…",
    "confirmed": true,
    "archive_password": "",
    "preserve_disk_signature": true,
    "auto_expand_last_partition": true
  }
}
```

---

### 6.10 kind 41 — MountRecoveryPoint

**用途：** 挂载恢复点只读会话（Dokan 等由 Service/Mount Host 处理）。

**请求 payload（5 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `repository_connection_id` | string | |
| `recovery_point_id` | string | |
| `source_disk_number` | uint32 | Manifest `disk_number` |
| `preferred_drive_letter` | string \| null | 单字母 `"D"`–`"Z"` 或 null（自动选空闲盘符） |
| `archive_password` | string | 加密 Archive 密码；未加密 `""`；不记日志 |

---

### 6.11 kind 42 — UnmountSession

**用途：** 卸载 Mount 会话。

**请求 payload：** `ResourceRef`（`resource_id` = session_id）。

---

### 6.12 kind 43 — UpsertSchedule

**用途：** 创建或更新备份计划。

**请求 payload（10 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schedule_id` | string \| null | **null=创建**；非空=更新 |
| `display_name` | string | |
| `enabled` | bool | |
| `source_ids` | string[] | 1–100 有序 |
| `repository_connection_id` | string | |
| `backup_type` | number | 创建常用 1 Full |
| `trigger` | object | ScheduleTrigger |
| `exclude_page_and_hibernation_files` | bool | 创建后冻结 |
| `encryption_enabled` | bool | 创建后冻结 |
| `archive_password` | string | **仅创建且加密** 时 1–32 字符；更新必须 `""` |

**创建后更新不变量（Service 强制）：**

| 可变 | 不可变 |
| --- | --- |
| `display_name`, `enabled`, `repository_connection_id`, `trigger` | `source_ids`, `backup_type`, exclude/encryption、密码 |

加密创建：口令经 DPAPI `CRYPTPROTECT_LOCAL_MACHINE` 保护（`pOptionalEntropy` = `schedule_id`）、
Base64 编码后写入 SQLite `schedules.archive_password_protected`
（`dpapi-lm:<schedule_id>:<base64>`）。`ScheduleSummary` 不返回该字段。

**创建示例（加密）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 43,
  "idempotency_key": "idem-create-schedule-1",
  "payload": {
    "schedule_id": null,
    "display_name": "Windows (C:), Data (D:)",
    "enabled": true,
    "source_ids": ["vol-c", "vol-d"],
    "repository_connection_id": "conn-01",
    "backup_type": 1,
    "trigger": {
      "kind": 1,
      "local_minute_of_day": 120,
      "weekday_mask": 0,
      "timezone_id": "UTC"
    },
    "exclude_page_and_hibernation_files": true,
    "encryption_enabled": true,
    "archive_password": "secret-password"
  }
}
```

**更新示例（改仓库 + 启用状态；冻结字段原样回传）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 43,
  "idempotency_key": "idem-update-schedule-1",
  "payload": {
    "schedule_id": "sch-001",
    "display_name": "Windows (C:), Data (D:)",
    "enabled": false,
    "source_ids": ["vol-c", "vol-d"],
    "repository_connection_id": "conn-02",
    "backup_type": 1,
    "trigger": {
      "kind": 2,
      "local_minute_of_day": 180,
      "weekday_mask": 62,
      "timezone_id": "UTC"
    },
    "exclude_page_and_hibernation_files": true,
    "encryption_enabled": true,
    "archive_password": ""
  }
}
```

**成功 ack：** `resource_id` = `schedule_id`。

---

### 6.13 kind 44 — DeleteSchedule

**用途：** 删除计划（及控制面记录；凭据清理策略见实现）。

**请求 payload：** `ResourceRef`（`resource_id` = schedule_id）。

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 44,
  "idempotency_key": "idem-del-sched-1",
  "payload": {
    "resource_id": "sch-001"
  }
}
```

---

### 6.14 kind 45 — SubscribeTaskEvents

**用途：** 订阅 task/mount 事件流；返回 lease（含 `resume_token` 与窗口）。

**请求 payload（3 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `resume_token` | string \| null | 首次 null；重连时原 token |
| `after_sequence` | number | 首次 0；重连为最后已处理序号 |
| `maximum_unacknowledged_events` | number | 1–128，默认 64 |

**成功 ack：** `event_subscription` 非 null（§4.2 lease）。

**示例（首次订阅）：**

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 45,
  "idempotency_key": "idem-sub-1",
  "payload": {
    "resume_token": null,
    "after_sequence": 0,
    "maximum_unacknowledged_events": 64
  }
}
```

未声明 `task.events` capability 时不得当作可用能力。

---

### 6.15 kind 46 — AcknowledgeEvents

**用途：** 确认已连续处理到 `through_sequence`，释放背压窗口。

**请求 payload（2 字段）：**

| 字段 | 类型 |
| --- | --- |
| `subscription_id` | string |
| `through_sequence` | number | ≥1 |

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 46,
  "idempotency_key": "idem-ack-1",
  "payload": {
    "subscription_id": "sub-01",
    "through_sequence": 12
  }
}
```

---

### 6.16 kind 47 — ExecuteDeletePlan

**用途：** 在用户确认后执行 kind 11 生成的删除计划。

**请求 payload（2 字段）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `plan_token` | string | |
| `confirmed` | bool | 必须 `true` |

```json
{
  "schema_version": 3,
  "message_type": 1,
  "request_id": "…",
  "kind": 47,
  "idempotency_key": "idem-exec-del-1",
  "payload": {
    "plan_token": "plan-…",
    "confirmed": true
  }
}
```

---

## 7. 异步 Event

**根 envelope（`message_type = 3`）：**

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | number | 3 |
| `message_type` | number | 3 |
| `subscription_id` | string | |
| `sequence` | number | 从 1 单调递增 |
| `kind` | number | ServiceEventKind |
| `message_code` | string | |
| `message_arguments` | array | |
| `payload` | object | 与 event kind 绑定 |

### 7.1 kind 1 — TaskProgress

**payload：** `TaskProgress`（§4.4）。

### 7.2 kind 2 — TaskCompleted

**payload：** `TaskResult`（§4.4）。

### 7.3 kind 3 — MountSessionChanged

**payload：** `MountSessionSummary`（§5.8）。

**背压：** 未确认事件窗口默认 64、最大 128；满窗暂停推送；token 失效需重新订阅并先用 ListJobs/ListMountSessions 对齐权威状态。

**示例 Event：**

```json
{
  "schema_version": 3,
  "message_type": 3,
  "subscription_id": "sub-01",
  "sequence": 3,
  "kind": 1,
  "message_code": "job.progress",
  "message_arguments": [],
  "payload": {
    "schema_version": 1,
    "job_id": "job-01",
    "trace_id": "tr-01",
    "phase": 4,
    "logical_bytes": 1000,
    "processed_bytes": 400,
    "stored_bytes": 200,
    "message_code": "backup.writing"
  }
}
```

---

## 8. 能力与未接线行为

- Desktop **必须先** `GetServiceInfo` 读取 `capabilities`，再决定页面与动作。
- 合法但未声明 capability 的请求：返回失败（如 Conflict + `service.capability_unavailable`），**无副作用**。
- 当前实现进度以 `service_host.md` / 完成计划为准；协议 kind 表是契约全集，不等于全部已生产可用。

---

## 9. 维护说明

| 变更类型 | 同步位置 |
| --- | --- |
| 新增/修改 kind 或字段 | 本文档 + Contracts DTO + Service/Desktop codec |
| 不可逆协议决策 | ADR（如 0013） |
| 传输/ACL | ADR-0011 / 0014 |
| 业务不变量（如 Schedule 冻结字段） | 模块文档 + Contracts 注释 |

**权威顺序：** 已接受 ADR → 本文档（wire 细节）→ Contracts 校验 → 模块开发文档。

**实现锚点：**

- Request encode/decode：`src/apps/service/src/service_protocol_request_json.cpp`
- Response/Event：`src/apps/service/src/service_protocol_response_json.cpp`
- Desktop codec：`src/apps/desktop/src/client/service_protocol*.cpp`
- Kind 枚举：`src/contracts/include/aegra/contracts/service.h`
- DTO：`src/contracts/include/aegra/contracts/service_control.h` 等
