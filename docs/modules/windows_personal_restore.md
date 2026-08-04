# Windows 个人版卷恢复

## 目标与非目标

本阶段把显式、完整的个人版 `.bkf` 恢复链还原到一个已存在的非系统 Windows Volume。它不创建分区、
不修改分区表、不恢复系统卷、不自动选择目标，也不扫描目录猜测恢复链。

## 依赖边界

`apps/worker` Composition Root 可以依赖 `PersonalArchiveReader`、`WindowsVolumeBlockSink` 和通用
`RestorePipeline`。Pipeline 只依赖 `IRecoveryPointReader` 与 `IBlockSink`，不得依赖 Windows、文件路径、
密码或 Archive Adapter。具体 Adapter 不能反向依赖 Worker。

## 执行流程

```text
Validate Restore Job and trusted chain-depth limit
-> Resolve every SecretRef while retaining all Secret lifetimes
-> Open and authenticate every base-first Archive layer
-> Validate full base, UUID chain, backup set and volume geometry
-> Open canonical target Volume GUID
-> Reject system volume
-> Reject any chain Archive located on target volume
-> Lock and dismount target volume
-> Preflight descriptors, capacity and memory budget
-> Read/authenticate/decompress each Chunk
-> Write by logical offset
-> Flush target
-> Unlock and close target
```

## Job 与结果

- `operation = kRestore`；
- `source_refs` 按 base-first 顺序包含完整 `.bkf` 链；单个全量备份是长度为 1 的链；
- `target_ref` 是 canonical Volume GUID Path；
- `credential_refs` 与 `source_refs` 数量相同、位置一一对应，每层允许使用不同口令；
- 链深度不得超过 Worker 受信任配置 `maximum_restore_chain_depth`，Job 不能覆盖该上限；
- 成功使用 `restore.completed`；取消、目标忙、空间不足、损坏 Archive 和凭据失败使用稳定且脱敏的
  `restore.*` message code；
- `logical_bytes` 与 `stored_bytes` 均表示成功写入的逻辑字节，`chunk_count` 表示完成写入的 Chunk 数。

## 不变量与失败语义

- 打开目标写句柄前，所有凭据必须解析成功，完整链必须通过认证、身份和几何校验；
- 首层只接受全量 Archive，后续层必须是与直接父层连续的增量 Archive；单个增量层必须拒绝；
- 所有解析出的 Secret 必须保持到 Chain Reader 完成打开，并在 Backend 返回时统一释放；
- 任一链 Archive 与目标同卷或无法安全解析其所在卷时，必须在锁卷前拒绝；
- 写入开始后取消或 I/O 失败会留下部分恢复的目标，结果必须是失败/取消，调用方不得把目标重新上线；
- 成功以 `FlushFileBuffers` 完成为界；析构始终 best-effort 解锁句柄；
- Worker 不在结果中输出口令、SecretRef、Archive 路径、Volume GUID 或底层 Win32 错误文本。

## 验证与完成标准

- 审查 Windows Sink 的 Port 行为和故障边界；
- 审查多层顺序、凭据一一对应、全部 Secret 生命周期、中途解析失败、链深度、取消和错误脱敏；
- 使用隔离的非生产 full + incremental Archive 链人工验证最终内容；
- 审查 Restore Pipeline 的容量、连续映射、写入、flush、取消和背压路径；
- Worker stdin/Named Pipe 共用同一 Restore 分发与 deadline；
- Debug/Release、源码规模、依赖检查和秘密扫描通过。
