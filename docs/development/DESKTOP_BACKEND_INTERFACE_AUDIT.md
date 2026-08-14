# Desktop 后台接口与 I/O 边界审计

| 属性 | 内容 |
| --- | --- |
| 审计日期 | 2026-08-14 |
| 范围 | `src/apps/desktop` 全部 C++ 与 QML |
| 协议 | Service Control V4 |

## 结论

Desktop 的业务数据只来自 Service Control IPC。Named Pipe socket、重连计时器、读写和 frame 组装运行在
`IpcFrameTransportWorker` 专用 `QThread`；GUI 线程仅提交 queued 请求并消费 queued 响应。Repository 离线、
请求超时或 Service 短暂断开时保留最后一次完整模型快照，不用空模型覆盖已有数据。

Desktop 生产 Target 不再编译网络共享访问实现，也不再调用 `QStorageInfo`、`QDir` 枚举目录、
`WNetAddConnection2W`、`WNetCancelConnection2W` 或 `GetLogicalDrives` 获取业务数据。Repository 容量、本机卷和
已占用盘符均消费异步 `ListSourceInventory` 快照；UNC 的连接、验证和访问由异步 Repository Service 命令完成。

## UI 调用的 Service 接口

| kind | 接口 | 主要 UI 消费者 |
| ---: | --- | --- |
| 1 | `GetServiceInfo` | 启动画面、连接状态、capability 门禁 |
| 2 | `ListRecoveryPoints` | Repository、Restore、Mount |
| 3 | `ListRepositoryConnections` | Repository、Backup Repository 选择 |
| 4 | `ListSourceInventory` | Home/Backup/Restore、Repository 本机卷、Mount 可用盘符 |
| 5 | `ListJobs` | Home、Backup、Restore、Event Log |
| 6 | `ListSchedules` | Backup Schedule |
| 8 | `ListMountSessions` | Mount |
| 9 | `PrepareRestore` | Disk/Volume Restore 预检 |
| 11 | `PlanDeleteRecoveryPoints` | Repository 删除确认 |
| 12 | `GetRecoveryPointLayout` | Restore、Mount 源布局 |
| 13 | `BrowseFileSources` | File Set Backup、文件恢复目标浏览 |
| 14 | `ListRecoveryPointEntries` | 文件恢复归档树 |
| 15 | `PrepareFileRestore` | 文件恢复预检 |
| 16 | `GetServiceSettings` | Settings |
| 32 | `AddRepositoryConnection` | Repository Add；包含 Service 侧网络验证 |
| 33 | `ImportRepositoryConnection` | Repository Import |
| 35 | `SetDefaultRepository` | Repository |
| 36 | `RemoveRepositoryConnection` | Repository |
| 37 | `StartBackup` | Backup |
| 38 | `CancelJob` | Backup/Restore 活动任务 |
| 40 | `StartRestore` | Disk/Volume Restore |
| 41 | `MountRecoveryPoint` | Mount |
| 42 | `UnmountSession` | Mount |
| 43 | `UpsertSchedule` | Backup Schedule 创建、编辑、启停 |
| 44 | `DeleteSchedule` | Backup Schedule |
| 47 | `ExecuteDeletePlan` | Repository |
| 48 | `StartFileRestore` | Files/Folders Restore |
| 49 | `UpdateServiceSettings` | Settings |

协议中已定义但当前 QML 未直接触发的接口为 `ListEvents`、`ResolveRecoveryPointChain`、
`TestRepositoryConnection`、`StartVerify`、`SubscribeTaskEvents`、`AcknowledgeEvents`；它们不构成本次 UI I/O 路径。

## Desktop 保留的本地运行时访问

以下访问不提供磁盘、卷、文件树、Repository、网络共享或任务业务数据：

- `main.cpp`：单实例 `QLocalServer`、Qt 临时诊断日志、窗口兼容性所需 OS 版本判断；
- `LocaleController`：UI 语言和主题偏好的 `QSettings`，以及打包 Qt resource 的存在性检查。

这些是 Desktop 自身生命周期、诊断和 UI 偏好，不得扩展为业务系统数据入口。新增业务系统访问必须先增加
Service contract，并通过 worker-thread IPC 异步返回。
