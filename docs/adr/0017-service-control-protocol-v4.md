# ADR-0017：本地 Service 控制面协议 V4

- 状态：Accepted
- 日期：2026-08-07
- 决策者：Aegra 项目
- 关联模块：contracts、application、apps/service、apps/desktop
- 替代范围：ADR-0013 中的 Service wire schema 与 API version；传输、ACL 与 Repository 权威决策仍分别见
  ADR-0011、ADR-0014、ADR-0010
- 关联文档：[SERVICE_CONTROL_PROTOCOL_V4.md](../protocol/SERVICE_CONTROL_PROTOCOL_V4.md)、[ADR-0016](0016-file-set-backup-and-restore-boundary.md)
- 后续决策：file_set Incremental 的 StartBackup/Schedule 语义由 [ADR-0018](0018-file-set-incremental-usn-and-chain.md) 扩展；协议仍为 current V4，不做版本兼容

## 背景

ADR-0013 将控制面冻结为 schema/API 3，覆盖 Volume 备份、Repository、Job、Schedule 与 task event。文件集
保护需要：

- 分页、授权绑定的源文件浏览（opaque node token）；
- Schedule 的 tagged `ProtectionSpec`（volume_set | file_set）；
- 认证后 Recovery Point 文件树分页；
- 文件恢复 preflight 与 Start 命令；
- Catalog / Job 摘要上的 `content_kind`。

产品未发布，因此 V4 **直接替代** V3，不并行协商、不保留 V3 parser/fallback。

## 决策

### 1. 版本与传输

- Named Pipe、4 字节 LE 长度前缀、UTF-8 JSON、64 KiB frame 上限不变。
- 所有根消息 `schema_version = 4`。
- `GetServiceInfo` 仅接受 `minimum_api_version..maximum_api_version` 覆盖 **4**；Service 固定
  `api_version = 4`。范围不含 4 时返回 `UnsupportedVersion` / `service.api_version_unsupported`。
- 生产代码删除 schema 3 常量与解析分支；收到 `schema_version != 4` 一律拒绝，不解释旧 payload。

### 2. 继承的 envelope

Request / Response / Event envelope 字段名与语义同 V3，仅版本号为 4：

- Query：`idempotency_key` 必须 null；成功 `response.kind = 1`；
- Command：非空幂等键 1–128 字节；成功 `response.kind = 2`；
- 失败：`response.kind = 3`，`payload = null`，稳定 `boundary_error_code` + `message_code`；
- 分页：每页最多 100，token ≤ 1024 字节，opaque，绑定 caller/kind/filter。

### 3. 新增与扩展的 request kind

保留 V3 kind 数值的业务含义（1–12、32–47）。新增：

| kind | 类型 | 名称 |
| ---: | --- | --- |
| 13 | Query | `BrowseFileSources` |
| 14 | Query | `ListRecoveryPointEntries` |
| 15 | Query | `PrepareFileRestore` |
| 48 | Command | `StartFileRestore` |

`UpsertSchedule`（43）payload 改为携带 `ProtectionSpecInput` 互斥 tagged union；创建 file_set 时使用
node token，更新不得改变已解析 selection。

`ListRecoveryPoints` / Job / Schedule 摘要增加 `content_kind` 字段（exact_keys 同步升级）。

`StartBackup` 对 file_set Schedule 只允许 Full；Worker Job 为 schema 4。

### 4. 安全边界

- Desktop 永不发送绝对路径、Volume GUID、NT device path、VSS path 作为选择输入；
- node token / preflight token 为密码学随机、短期、绑定 caller SID + session + 资源；
- 响应与 event 不返回 Archive 对象 key、分卷路径、security descriptor、完整选择路径列表；
- 路径类客户数据不得进入默认 `message_arguments`。

### 5. Capability

新 capability 字符串（完成对应工作包后才声明）：

| capability | 含义 |
| --- | --- |
| `file.browse` | BrowseFileSources |
| `file.schedule` | UpsertSchedule file_set |
| `file.backup` | StartBackup on file_set schedule |
| `file.recover_browse` | ListRecoveryPointEntries |
| `file.restore` | PrepareFileRestore + StartFileRestore |

未声明时 dispatcher 在 handler 前返回 Conflict / `service.capability_unavailable`，无副作用。

## 备选方案

- **在 V3 上加可选字段：** exact_keys 协议无法安全演化，且 content_kind 与 token 语义是破坏性变更，不采用。
- **V3/V4 双栈协商：** 无已发布客户端，增加矩阵，不采用。
- **Desktop 本地枚举路径：** 违反 ADR-0016，不采用。

## 影响

- Service 与 Desktop codec 一次性切换到 V4；
- Contracts DTO 升 schema 4；Worker Job schema 4；
- ADR-0013 对 wire 的权威被本文替代；传输与安全 ADR 仍有效；
- 开发期 SQLite / 命令记录按新 schema 重建，不做迁移。

## 验证

- 人工对合法/非法 V4 frame 做协议入口验证（版本、混合 payload、绝对组件、超限）；
- 构建 `aegra_contracts`、Service、Desktop 生产 Target（后续工作包）；
- `git diff --check`；搜索确认无 V3 fallback 分支。
