# 个人版 Archive Verify

## 目标与非目标

Archive Verify 对个人版 `.bkf` 执行完整、只读的数据完整性校验。它验证 Archive 可被正式恢复链读取，
但不创建恢复目标、不写入源文件、不依赖 `.bhx` Sidecar，也不负责自动发现增量链。

## 依赖与边界

通用 `VerifyPipeline` 只依赖 `IRecoveryPointReader`、取消和 Progress Port。个人版 Worker Composition
Root 可以创建 `PersonalArchiveReader`，但 Reader、Pipeline 和任务契约不得依赖 Windows、VSS、数据库或
JSON。JSON、DPAPI 凭据解析和进程退出码仍只存在于 `apps/worker` 与具体 Adapter。

## 执行流程

```text
Validate Verify Job
-> Resolve one SecretRef
-> Open PersonalArchiveReader
-> Authenticate and decode metadata
-> Validate split sequence and Footer
-> Preflight every ChunkDescriptor
-> Read every chunk
-> Authenticate payload AAD/AEAD and decompress
-> Return verified logical bytes and chunk count
```

`PersonalArchiveReader::open()` 完成 Header、metadata、分卷、Chunk 结构和 Footer 校验；
`IRecoveryPointReader::read_chunk()` 完成每个 Chunk 的密文认证、解压、ZERO/DEDUP 展开与输出范围校验。
`VerifyPipeline` 负责 descriptor 稳定性、非重叠顺序、payload 大小、取消和进度。顺序校验按
`source_index` 分段：每个 source 是一段连续运行（不得交错），其逻辑地址空间从 0 重新开始，与
Archive 层形状一致；所有 Chunk 逻辑字节总和不得超过 Reader 的逻辑容量。增量层允许合法空洞，
因此 Pipeline 不要求 Chunk 覆盖整个逻辑卷。

Schedule 的 `verify_after_backup` 是 Service 编排策略：Backup 提交时把动作写入 durable
`post_backup_plans`（与 JobRecord 同事务），`PostBackupCoordinator` 在 Backup 成功并完成 Catalog
发布后以确定性幂等键 `post-backup-verify:<backup job_id>:<file_uuid>`（稳定标识符字符集，区别于含
`|` 的 request fingerprint）提交独立 Verify Job；Service 重启后重扫 plan 重放同一幂等键。Verify
请求从 Catalog 重建：file_set 使用 base-first 完整链，volume_set 使用 tip Archive；凭据取
Schedule 的 dpapi-lm 密文引用（未加密备份为空凭据/空口令）。容量暂满保持 pending 重试；提交失败
在 Service `error.log` 记录错误码与脱敏 detail。Verify 失败单独记录，不回滚或隐藏已提交 Archive。

## 任务日志

每个已接受的 Verify Job（包括 Post Backup Verify）都通过 `WorkerTaskLog` 写入独立日志：
`<data_dir>/logs/verify/YYYYMMDD_HHMMSS[_job-id].log`。日志包含 Job/Request、逐阶段开始与结果，以及
成功、失败或取消的 `[Result]` 汇总；不得记录密码、SecretRef 或凭据材料。volume_set 与 file_set
使用同一目录和格式，file_set 额外记录层数、tip entry/stream 数及完整链校验字节数。

## Job 与结果

- `operation = kVerify`；
- `source_refs` 恰好包含一个 `.bkf` 路径；
- `target_ref` 必须为空，因为 Verify 不产生数据目标；
- `credential_refs` 恰好包含一个口令 SecretRef；空 SecretRef 表示未加密 Archive（空口令），
  与 Restore 和 file_set Verify 的约定一致；
- 成功结果使用 `verify.completed`，失败和取消使用稳定的 `verify.*` message code；
- `logical_bytes` 表示 Archive 描述的源逻辑容量，`stored_bytes` 表示本次成功解码验证的逻辑 payload
  字节，`chunk_count` 表示完成验证的 Chunk 数量。

## 安全、取消与资源

- 口令只在同步 Reader 创建和 Verify 生命周期内存活，不写入 Job、日志或结果；
- Reader 的 metadata、stored payload、logical payload 和分卷数量使用受信任上限；
- DEDUP 只允许当前 Volume Chunk 内后向引用 RAW/COMPRESSED；Verify 重算 block/bytes 并核对 Footer；
- 认证失败时不向调用方返回未认证数据；
- 取消在打开 Reader 后逐 Chunk 检查，并贯穿读取、认证和解压；
- Verify 只读打开 Archive，不创建 partial、Sidecar 或临时恢复目标。

## 验证与完成标准

- 审查成功、空 Archive、descriptor 越界/重叠、读取后变化、payload 大小不符和取消路径；
- 审查请求拒绝、Credential 生命周期、成功指标、错误脱敏和取消边界；
- 使用隔离的非生产 Archive 人工验证正确/错误口令、payload 篡改、截断和分卷缺失；
- stdin 与 Named Pipe Worker 对 Verify 使用与 Backup 相同的 Host、deadline、Progress 和退出码边界；
- 受影响 Target、静态检查、源码规模和文档检查通过。
