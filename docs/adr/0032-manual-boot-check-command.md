# ADR-0032：用户手动触发单个恢复点的 BootCheck

- 状态：Accepted
- 日期：2026-09-12
- 决策者：Aegra maintainers
- 关联模块：contracts、apps/service、apps/desktop、docs/protocol

## 背景

BootCheck 此前只能作为 Schedule 的备份后置动作（`boot_check_after_backup`）由 `PostBackupCoordinator`
驱动。用户需要在 Repository 页面对任意已存在的 volume_set 恢复点按需发起一次 BootCheck（例如备份时未
启用、想在更换 hypervisor 后复验、或对历史恢复点抽检），并像 Verify 一样在任务列表中看到进度与结果。

durable plan 表 `post_backup_plans` 以 `backup_job_id` 为主键并外键到 `jobs`；BootCheck 运行本身不是
Worker Job，而是由 `BootCheckSupervisor` 拉起的独立 Host 进程。手动触发需要一个既 restart-safe、又不
引入第二套调度器或 schema 变更的落点。

## 决策

1. 新增控制面命令 `StartBootCheck`（kind 54，幂等）：payload 为
   `repository_connection_id`、`recovery_point_id`（单个）、`hypervisor`（null = Settings 默认）。
   capability `recovery_point.boot_check` 仅在 AegraBootCheck Host 可派发时声明。
2. 手动 BootCheck 复用 `post_backup_plans`：Service 在同一事务写入一条 Queued 的 BootCheck
   JobRecord（`operation=5`）和一条以该 job 为锚点的 plan（`backup_job_id = job_id`，
   `verify_required=false`，`boot_check_required=true`，hypervisor 快照）。不新增表或 schema 版本。
3. `PostBackupCoordinator::drive_plan` 以锚点 Job 的 `operation=BootCheck` 识别手动 plan：
   Queued → 经 Supervisor 派发（`boot_check_job_id = job_id`，即 Host job id / VM 名后缀 / 私有目录名）
   并置 Running；取到 Host 结果后置 Succeeded/Failed。容量不足或 scavenge 未完成时保持 Queued 下次扫描
   重试。
4. 手动运行不做多次重派。Service 重启时启动扫描把活动 Job 置为 Interrupted，plan 随即终态；用户重新
   提交即可。这与备份后置动作“按 attempts 重派”不同，因为手动运行没有需要保证完成的上游备份语义。
5. 凭据与友好卷名来源：按恢复点 `backup_set_uuid` 反查 Schedule 并记入 plan；Schedule 已删除时无凭据
   （加密链将由 Host 以 `verify.credential_unavailable` 类失败终止），`source_ids` 退化为恢复点 id。
   锚点 JobRecord **不写 `schedule_id`**：手动运行不属于该 Schedule 的备份后置链，Backup 页的 B/V/C
   状态芯片只反映 Schedule 自己触发的动作。
6. Service 在接受前做快速失败：所选 hypervisor 已探测为不可用时返回该稳定 `bootcheck.*` 码；
   file_set 恢复点返回 `bootcheck.volume_set_required`。Desktop 直接本地化显示这些码。
7. Desktop Repository 页面在 Verify 右侧增加 Boot Check 按钮，进入选择模式（仅 volume_set 可选，
   可多选），再次点击后按顺序逐个提交 StartBootCheck；接受后乐观写入 Queued Job 并轮询。
8. ListJobs 对手动 boot check Job 投影 `progress.recovery_point_id`（来自以 job id 为键的 plan），
   Desktop 恢复点状态列据此显示 boot check 的排队/运行/通过/失败。

## 备选方案

- 新建独立的“ManualBootCheckService”与自有持久化表：拒绝。会复制 claim/lease、Job 记录与 Supervisor
  结果消费逻辑，并需要 schema 升级。
- 以恢复点对应的原备份 Job 作为 plan 锚点：拒绝。同一备份 Job 已有一条 plan（主键冲突），且会把手动
  运行的重试/跳过语义与备份后置动作混在一起。
- 手动运行也按 attempts 重派：拒绝。重启后自动重新拉起 VM 不符合“用户按需触发”的预期，且锚点 Job
  已被启动扫描置为 Interrupted，语义上不应再变回 Running。

## 影响

- Contracts：`ServiceRequestKind::kStartBootCheck = 54`、`StartBootCheckCommand`，request payload
  variant 与校验扩展；protocol V4 文档新增 §7.4b。
- Service：`ServiceRuntimeInfo` 新增 `post_backup_coordinator`；coordinator 构造增加
  `IRepositoryStorageFactory`（Catalog 校验）。
- Desktop：新增 `service_client_boot_check.cpp`、kind 54 编码、翻译条目与 Repository 页面模式。
- 不影响已发布格式（产品未发布），不新增 SQLite schema 版本。

## 验证

- Release 生产构建、源码规模与架构检查通过。
- 人工：Repository 页面 Boot Check → 选中 volume_set 恢复点 → 提交；任务列表出现 Queued → Running →
  终态的 Boot Check 任务；file_set 恢复点不可勾选；hypervisor 不可用时提交被稳定码拒绝；
  Service 重启后未完成的手动任务显示 Interrupted 且不再自动重启 VM。
