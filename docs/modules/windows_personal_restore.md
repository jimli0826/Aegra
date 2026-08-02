# Windows 个人版卷恢复

## 目标与非目标

本阶段把单个完整个人版 `.bkf` 恢复到一个已存在的非系统 Windows Volume。它不创建分区、不修改分区
表、不恢复系统卷、不自动选择目标，也不解析增量链。

## 依赖边界

`apps/worker` Composition Root 可以依赖 `PersonalArchiveReader`、`WindowsVolumeBlockSink` 和通用
`RestorePipeline`。Pipeline 只依赖 `IRecoveryPointReader` 与 `IBlockSink`，不得依赖 Windows、文件路径、
密码或 Archive Adapter。具体 Adapter 不能反向依赖 Worker。

## 执行流程

```text
Validate Restore Job
-> Resolve one SecretRef
-> Open and authenticate PersonalArchiveReader
-> Require one full Archive layer
-> Open canonical target Volume GUID
-> Reject system volume
-> Reject Archive located on target volume
-> Lock and dismount target volume
-> Preflight descriptors, capacity and memory budget
-> Read/authenticate/decompress each Chunk
-> Write by logical offset
-> Flush target
-> Unlock and close target
```

## Job 与结果

- `operation = kRestore`；
- `source_refs` 恰好包含一个 `.bkf`；
- `target_ref` 是 canonical Volume GUID Path；
- `credential_refs` 恰好包含一个 Archive 口令 SecretRef；
- 成功使用 `restore.completed`；取消、目标忙、空间不足、损坏 Archive 和凭据失败使用稳定且脱敏的
  `restore.*` message code；
- `logical_bytes` 与 `stored_bytes` 均表示成功写入的逻辑字节，`chunk_count` 表示完成写入的 Chunk 数。

## 不变量与失败语义

- 任何数据写入前，Archive 必须成功打开且目标必须完成独占锁卷、卸载和容量预检；
- 只接受全量 Archive；增量/差异 Archive 返回冲突错误，防止空洞保留目标旧数据；
- 写入开始后取消或 I/O 失败会留下部分恢复的目标，结果必须是失败/取消，调用方不得把目标重新上线；
- 成功以 `FlushFileBuffers` 完成为界；析构始终 best-effort 解锁句柄；
- Worker 不在结果中输出口令、SecretRef、Archive 路径、Volume GUID 或底层 Win32 错误文本。

## 测试与完成标准

- Windows Sink 普通文件模式覆盖 Port 行为和故障边界；
- Restore Task Fake Backend 覆盖参数映射、凭据生命周期、取消和错误脱敏；
- 现有 Restore Pipeline 覆盖容量、连续映射、写入、flush、取消和背压；
- Worker stdin/Named Pipe 共用同一 Restore 分发与 deadline；
- Debug/Release、源码规模、依赖检查和秘密扫描通过。
