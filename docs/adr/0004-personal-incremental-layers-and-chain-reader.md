# ADR-0004：个人版稀疏增量层与链式恢复视图

- 状态：Accepted
- 日期：2026-08-01
- 决策者：Aegra 项目
- 关联模块：format、pipeline、adapters/personal_archive

## 背景

个人版增量备份只保存相对父备份发生变化的逻辑块，因此单个增量 `.bkf` 是稀疏覆盖层，而不是完整
连续数据源。现有 `IRecoveryPointReader` 和 Restore Pipeline 要求 descriptor 从 0 开始连续覆盖整个
逻辑源；直接把稀疏层交给 Pipeline 会把格式语义泄漏到通用数据面，或者导致未变化区域被错误清零。

`.bhx` 描述父备份时刻的完整块状态，可以作为下一层的比较基线，但不是恢复依赖。产品尚未发布，
本决策直接定义正式增量行为，不保留旧试验实现。

## 决策

1. `ArchiveCreateRequest.manifest.backup_job.backup_type` 是备份类型的唯一输入。全量不接受父路径；增量
   必须显式提供父 Archive 路径和父口令。差异备份在单独阶段实现，在此之前明确拒绝。
2. 创建增量 Session 时必须同时打开并验证父 Archive 和父 `.bhx`。父 Header、Sidecar、block size、
   volume identity、逻辑大小和完整记录数必须一致；当前文件继承父 `backup_set_uuid`，其
   `parent_uuid` 等于父 `file_uuid`。
3. Pipeline 仍读取完整源。Personal Archive Session 对每个输入块计算当前状态：DATA 使用 SHA-256，
   ZERO 使用零 hash；新 Sidecar 始终保存完整状态。只有相对父 Sidecar 发生变化的块进入 `.bkf`
   chunk 流。
4. DATA 只有在父状态也是 DATA 且 SHA-256 相同时才视为未变化。当前 ZERO 在父状态为 ZERO 或 SKIP
   时视为未变化；从 DATA 变为 ZERO 必须写入显式 ZERO entry。
5. 一个物理 Chunk 的 BlockEntry 必须覆盖连续逻辑范围。输入 chunk 内出现未变化空洞时，Writer 将
   变化区间拆成多个连续物理 chunk，并重新生成从 0 开始的持久化 `chunk_index`。完全无变化的输入
   chunk 不落盘；零变化增量允许只有 Header、Metadata、Footer 和完整 Sidecar。
6. 单文件 `PersonalArchiveReader` 可以打开稀疏增量层，验证其有序、无重叠和边界正确，但不得把它
   当成完整恢复视图。全量层仍必须连续覆盖整个 volume。
7. `PersonalArchiveChainReader` 接受调用者显式提供的 base-first `ArchiveOpenRequest` 列表，逐层验证
   UUID 链、类型、volume 和 block size，再把后层稀疏区间覆盖到基准全量 Reader 上，对 Pipeline 暴露
   连续 `IRecoveryPointReader`。
8. Adapter 不扫描目录猜测父文件，也不对任意文件批量执行 KDF。链发现、凭据选择和用户交互属于后续
   Application 用例；显式层列表允许每层使用不同口令，并避免路径与资源消耗策略进入格式 Reader。

## 备选方案

- 修改 Restore Pipeline 支持逻辑空洞：会让所有后端承担个人格式的 overlay 语义，不采用。
- 未变化块写入父引用：V6 DEDUP 只允许当前 chunk 内向前引用，跨文件引用会改变 BlockEntry 契约，
  不采用。
- 自动扫描 leaf 所在目录：会扩大路径信任边界，并可能对大量攻击者控制文件执行昂贵 KDF，不采用。
- 仅生成增量文件、不提供链 Reader：无法形成可端到端验证的恢复能力，不采用。

## 影响

- 增量层可以单独 inspect，但标准恢复必须使用 Chain Reader。
- Chain Reader 的输出 chunk 边界沿用基准全量层；读取某个基准 chunk 时按层顺序应用与其相交的覆盖。
- `.bhx` 丢失不影响已有链恢复，但不能以该层为父继续创建新增量。
- 链长度由请求上限控制；重复 UUID、断链、错误顺序、非全量基准或不匹配 volume 均拒绝。

## 验证

- 测试覆盖 DATA 修改、DATA 变 ZERO、未变化块省略、零变化增量和完整新 Sidecar。
- 测试验证 base-first Chain Reader 通过原 Restore Pipeline 得到最新完整字节。
- 损坏测试覆盖缺父层、UUID 断链、block size/volume 不匹配和错误父口令。
- Debug、Release、clang-tidy、clang-format、规模检查和 `git diff --check` 作为质量门禁。
