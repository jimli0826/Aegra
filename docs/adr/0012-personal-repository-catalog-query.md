# ADR-0012：个人版 Repository Catalog 查询

- 状态：Accepted
- 日期：2026-08-03
- 决策者：Aegra 项目
- 关联模块：contracts、personal_repository、application、apps/service、apps/desktop

## 背景

阶段 13A 已建立 Desktop 到本地 Service 的版本化 IPC。下一阶段需要让 Desktop 展示已配置个人版
Repository 和 Recovery Point，但当前尚未实现 Archive Header/Footer 深度扫描、Catalog Reconcile 和
凭据认证。查询接口不能把仅来自 Catalog 的记录描述为已经通过恢复认证，也不能返回不受限集合而超过
64 KiB Service frame。

## 决策

1. 新增 `RepositoryCatalogScanner`，只通过 `IObjectReader` 和 `IPrefixEnumerator` 读取 Descriptor、Catalog
   Entry 和 Deletion Tombstone。它不打开 `.bkf`、不执行 KDF、不修复对象，也不依赖具体 Storage Adapter。
2. Scanner 严格验证 Descriptor、对象 key、内容 UUID、Repository UUID、Catalog 图和 Tombstone。任一权威
   冲突使本次扫描整体失败，不返回混合可信度的部分结果。
3. 有效 Tombstone 中的 Recovery Point 从查询结果隐藏。Scanner 仍不执行删除，也不清理 Tombstone。
4. 输出按 `file_uuid` 稳定排序并分页。Service 每页最多返回 100 个 Recovery Point；continuation token 是
   不透明的 Catalog key，调用方不得解析。Scanner 总计最多读取 10,000 个 Catalog 对象。
5. Catalog Entry 的 `structural_state=complete` 只表示目录登记状态。响应称为 `catalog_ready`，不能据此启用
   Restore/Mount；正式恢复仍要求 Archive 结构扫描、认证和完整链校验。
6. 新增 Application `IPersonalRepositoryQuery` 和默认实现。Application 把 Repository 模型映射为 Contracts，
   Service Controller 不直接解析 Catalog，Desktop 不链接 Repository 或 Storage。
7. Service IPC 直接升级到 schema 2，不实现 schema 1 兼容。新增 `ListRecoveryPoints` 请求和
   `RecoveryPointPage` 响应；根响应拥有互斥的 `service` 与 `recovery_points` payload。
8. Repository 根只允许通过受信任启动参数 `--repository-root <absolute-local-path>` 配置，不接受 Desktop
   请求传入路径。未配置时返回合法的 `not_configured` 空页；配置存在但无法打开或验证时 Service 启动失败。
9. Service 只返回 Repository UUID 和不含客户 Metadata 的恢复点摘要，不返回本地根路径、SecretRef、
   Archive key、主机名或原始 Storage 错误。
10. Desktop 在 Service 握手成功后顺序拉取全部页面，校验每页 request ID、排序、重复 UUID 和 token，
    然后发布到 QML。查询失败不把 Service 连接误报为断开。
11. Service schema 2 中的时间和容量整数限制为非负有符号 64 位范围。Contracts 在编码前拒绝更大值，
    Desktop 使用 Qt 6 的 64 位整数接口解码，禁止经 32 位整数或浮点近似校验协议字段。

## Wire Schema 2

列表请求：

```json
{"schema_version":2,"request_id":"<uuid>","kind":2,"repository_list":{"maximum_results":100,"continuation_token":null}}
```

列表响应：

```json
{"schema_version":2,"request_id":"<uuid>","kind":3,"boundary_error_code":0,"message_code":"repository.catalog_ready","service":null,"recovery_points":{"state":2,"repository_uuid":"01234567-89ab-4cde-8f01-23456789abcd","items":[],"continuation_token":null}}
```

`state=1` 表示 `not_configured`，`state=2` 表示 `catalog_ready`。`kind=1` 仍为 ServiceInfo，`kind=2`
为 RequestFailed，`kind=3` 为 RecoveryPointPage。

## 备选方案

- Desktop 直接扫描目录：违反进程边界和最小权限，不采用。
- 一次返回全部 Recovery Point：无法保证 64 KiB frame 上限，不采用。
- 现在同时实现 Archive 深度扫描和自动修复：扩大只读 UI 纵向切片的风险，不采用。
- 从 IPC 接收任意 Repository 路径：扩大路径信任边界，不采用。
- 继续 schema 1 并加入可选字段：产品未发布且严格 union 更清晰，不采用。

## 影响

- Desktop 可以展示可携带 Catalog，但页面必须明确这是目录状态，不是恢复就绪证明。
- Service 启动参数暂时承担单 Repository 开发配置；SQLite 多 Repository 连接管理属于后续阶段。
- 后续 Archive Scanner 可以替换/扩展 Application 查询数据源，而不改变 Desktop 直接依赖方向。

## 验证

- Scanner 测试覆盖分页、短读、取消、Tombstone 隐藏、key/UUID 冲突、损坏对象和断链。
- Application 测试覆盖 NotConfigured、分页映射和错误脱敏。
- Service codec/dispatcher/真实进程测试覆盖 schema 2 和 `--repository-root`。
- Desktop 测试覆盖多页合并、重复/乱序拒绝、查询错误与 Service 连接状态分离。
- Debug/Release、源码规模、依赖边界、格式和秘密扫描通过。
