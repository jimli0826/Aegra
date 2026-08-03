# 个人版 Repository Catalog 查询开发文档

## 目标与范围

阶段 13B 建立从受管理 Repository 到 Desktop 的只读查询链：

```text
Local Object Storage -> RepositoryCatalogScanner -> PersonalRepositoryQuery
                     -> Service schema 2 -> Desktop Recovery Point list
```

本阶段只读取 Descriptor、Catalog Entry 和 Deletion Tombstone。不读取 Archive Header/Chunk/Footer，不认证
Metadata，不发布或修复 Catalog，不访问 SQLite，也不允许 Desktop 指定任意路径。

## 模块与依赖

- `personal_repository`：实现 Scanner，只依赖 Base、Format 和 Ports。
- `application`：实现查询用例并映射 Contracts，只依赖 PersonalRepository、Ports 和 Contracts。
- `apps/service`：组合 Local Storage 与查询用例，编码 schema 2。
- `apps/desktop`：只依赖 Qt，通过 IPC 分页读取，不链接上述核心模块。

具体 Windows 路径只存在于 `service_main.cpp` composition root。Contracts 不包含路径、Qt 或 Adapter 类型。

## Scanner 不变量

- 必须先读取并验证 `aegra.repository`。
- Catalog/Tombstone 对象 key 的 UUID 必须与内容一致，并属于 Descriptor Repository UUID。
- Tombstone 目标在所有页面隐藏。
- Catalog 图冲突、重复 UUID、跨 Set 父引用和环使扫描失败；缺父节点保留并标记 chain incomplete。
- 对象读取受单文档、对象数量和总读取字节上限约束，并正确处理短读与取消。
- 输出按 `file_uuid` 排序；token 只允许继续同一 Repository 的 Catalog key 顺序。

## Service 与 UI

Service 未配置 Repository 时返回 `not_configured`，不把它当作错误。配置失败在启动边界返回稳定退出码。
Desktop 握手后拉取页面，最多累计 10,000 项；页面排序、重复 UUID 或 token 不前进都作为协议错误处理。
查询错误只更新 Repository 错误状态，Service 连接仍保持 Ready。

UI 展示 backup type、创建时间、逻辑/存储大小和 chain complete/incomplete。Restore、Delete 和 Verify 按钮
保持禁用，直到后续 Archive 结构与认证状态接入。

## 测试与完成标准

- Memory Storage Scanner 单元测试覆盖正常、分页、短读、损坏、冲突、隐藏和取消。
- Local Storage + Service 真实进程测试读取临时 Repository fixture。
- Desktop fake Service 测试覆盖多页和错误状态。
- Qt 不进入 Contracts/Application/PersonalRepository，Windows Adapter 不反向依赖核心模块。
- VS 2026 Insiders + Qt 6.8 Debug/Release 和全部质量门禁通过。

## 当前状态

阶段 13B 查询链已经实现。Scanner、Application、Service schema 2、Desktop 分页聚合与 Recovery Point 首屏
均已接入；真实 Service 进程测试使用临时 Local Storage Repository 验证 Descriptor/Catalog 到 IPC 响应。
Restore、Verify 和 Delete 仍不在本查询切片范围内。
