# 架构决策记录（ADR）

ADR 用于记录会长期约束多个模块、持久化格式、外部协议或运维方式的决策。已经接受的 ADR 不直接修改历史；后续决策通过新 ADR 替代旧 ADR。

当前个人版 Repository 权威与 Catalog 边界见
[ADR-0010](0010-personal-repository-authority-and-catalog.md)。

本地 Service 与 Desktop 的进程通信见
[ADR-0011](0011-local-service-desktop-ipc.md)。

个人版 Repository Catalog 的分页 Service 查询见
[ADR-0012](0012-personal-repository-catalog-query.md)。

本地 Service 控制面 V3 envelope、命令幂等和 task event 见
[ADR-0013](0013-service-control-protocol-v3.md)。

Windows Service SCM 边界、显式 Named Pipe ACL 与调用方身份校验见
[ADR-0014](0014-windows-service-ipc-security.md)。

项目不维护自动化测试用例的仓库级决策见
[ADR-0015](0015-no-project-test-suite.md)。该决策取代早期 ADR 中的自动化测试要求。

## 状态

- `Proposed`：讨论中，不构成实现依据。
- `Accepted`：已接受，构成现行规范。
- `Superseded`：已被后续 ADR 替代，必须链接替代者。
- `Rejected`：明确不采用，保留原因避免重复讨论。

## 编号与文件名

使用四位连续编号和短横线文件名，例如 `0001-repository-commit-visibility.md`。编号一旦分配不得复用。

## 何时必须写 ADR

- 新增或改变跨模块依赖方向；
- 改变 `.bkf`、Repository Object、数据库 Schema 或跨进程协议；
- 引入新的第三方核心依赖、运行时进程或存储后端；
- 改变加密、密钥管理、事务边界、恢复一致性或数据删除语义；
- 重新启用迁移清单中明确淘汰的旧设计。

## 模板

```markdown
# ADR-NNNN：决策标题

- 状态：Proposed
- 日期：YYYY-MM-DD
- 决策者：
- 关联模块：

## 背景

描述问题、约束和必须决定的事项。

## 决策

写出一个可验证的决定，包括边界和不变量。

## 备选方案

列出认真考虑过的方案及未选择原因。

## 影响

说明收益、代价、迁移、验证和运维影响。

## 验证

列出证明决策被正确实现的自动化检查或验收条件。
```
