# ADR-0033：恢复点 Verify / BootCheck 结果持久化与双列状态

- 状态：Accepted
- 日期：2026-09-12
- 决策者：Aegra maintainers
- 关联模块：contracts、ports、adapters/sqlite、apps/service、apps/desktop、docs/protocol

## 背景

Repository 页面此前只有一个 Status 列，且状态完全由 Desktop 内存中的任务列表推导：Verify 与 BootCheck
共用一列互相覆盖，Desktop 重启后只能看到任务日志里仍保留的任务，任务保留期清理后状态彻底消失，
从未检查过的恢复点与“链完整”无法区分。用户需要为每个恢复点分别看到最近一次 Verify 与最近一次
BootCheck 的结果，并且这个结果要跨重启持久存在；没有做过的检查显示 N/A。

Catalog 是 Repository 权威（ADR-0010），不应写入本机的检查结果；控制面 SQLite 是本机投影的合适位置。

## 决策

1. 控制面新增表 `recovery_point_checks`（schema v31）：主键 `(repository_connection_id,
   recovery_point_id, operation)`，`operation` 仅 Verify(3)/BootCheck(5)，字段 `state`
   （Succeeded/Failed/Cancelled/Interrupted）、`message_code`、`job_id`、`completed_utc_ms`。每次终态运行
   `INSERT OR REPLACE`。**不**外键到 `jobs`，因此不随任务保留期清理；产品未发布，不提供 v30 迁移。
2. Ports 新增 `RecoveryPointCheckRecord`、`IRecoveryPointCheckStore`（upsert / list by connection）与
   `IControlPlaneDatabase::list_recovery_point_checks`。
3. 写入点：
   - Verify：Worker 完成回调按 Job `source_ids` 顺序与 Supervisor 最后进度的 `recovery_point_id`
     判定停止位置——之前的恢复点记 Succeeded（`verify.completed`），当前项取 Job 终态与结果码，之后的
     恢复点未运行、不改写；停止位置未知时整批记终态。成功批全部记 Succeeded。
   - BootCheck：`PostBackupCoordinator` 在 BootCheck Job 终态（后置动作与手动均经
     `record_boot_check_job_terminal`）记录；手动锚点被启动扫描置 Interrupted 时也记录 Interrupted。
4. Wire：`RecoveryPointSummary` 增加 `verify_check` / `boot_check`（object | null，exact keys
   `state`、`message_code`、`job_id`、`completed_utc_ms`），exact keys 由 13 增至 15。仅在指定
   `repository_connection_id` 的 ListRecoveryPoints 中填充；排队/运行中状态不入表，Desktop 由
   ListJobs 合并。
5. Desktop Repository 页面把 Status 拆为 **Verify** 与 **Boot check** 两列。每列取值：活动任务
   （排队/运行）优先；否则任务创建时间晚于持久化结果完成时间者优先；否则持久化结果；否则 N/A。
   备份集行按列聚合（运行 > 排队 > 失败 > 取消 > 全部通过 > 部分已检查 > N/A）。

## 备选方案

- 查询时从 `jobs` 表联表推导：拒绝。任务保留期清理后状态丢失，且 Verify 批内逐项结果不在 JobRecord
  中，无法在事后区分批内哪些恢复点通过。
- 把结果写入 Repository Catalog：拒绝。违反 ADR-0010 的 Catalog 权威边界，且不同主机对同一
  Repository 的 BootCheck 能力不同，结果是本机事实。
- 保留单列合并显示：拒绝。两种检查语义不同，互相覆盖会让用户误判。

## 影响

- Contracts / Ports / SQLite / Service / Desktop 各增加一处对称改动；协议 V4 §4.9 更新；
  开发数据库需重建（schema v31）。
- 旧 Desktop 对新 Service 的 ListRecoveryPoints 解析会因 exact keys 变化失败；产品未发布，两端同时替换。

## 验证

- Release 生产构建、源码规模检查通过。
- 人工：对部分恢复点做 Verify、部分做 Boot Check，两列各自变化；重启 Desktop 与 Service 后状态仍在；
  从未检查的恢复点显示 N/A；备份集折叠行聚合正确；失败原因悬停可见。
