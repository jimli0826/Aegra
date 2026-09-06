# ADR-0030：隔离测试盘备份还原验证工具

- 状态：Superseded（安全约束并入 ADR-0031）
- 日期：2026-09-06
- 决策者：项目所有者
- 关联模块：`apps/cli`、开发工具与验证流程

## 背景

整盘备份和还原的人工验收需要重复执行数据生成、Schedule 启动、Job 等待、Recovery Point 发现、
目标盘还原以及逐文件 SHA-256 比较。手工拼接这些步骤容易选错 Recovery Point、遗漏失败状态，或把
结果散落在终端输出中。该决策最初作为 ADR-0015 的窄化例外建立；通用脚本政策现由 ADR-0031 统一规定。

## 决策

1. 仓库只允许一个此类脚本：`tools/AegraDiskBackupRestoreValidation.ps1`。
2. 该脚本是操作者显式启动的生产验收工具，不属于默认构建或项目测试套件；不得注册到 CMake、CTest、
   CI、定时任务或其它无人值守入口。
3. 环境值集中写在脚本顶部。默认 `ConfirmDestructiveRestore = $false`；只有操作者核对并改为 `$true`
   后才能继续。
4. 目标只接受 Inventory `disk.N`，必须由两个独立配置值完全匹配；脚本拒绝系统盘、不可用盘以及包含
   Schedule 源卷的物理盘。Service 仍执行权威 Restore preflight 和目标绑定复检。
5. CLI 不接收 Archive 密码；此流程仅支持未加密 `volume_set` Schedule。脚本不得记录凭据、SecretRef、
   preflight token 或文件内容。
6. 结果写为独立 JSON，包含阶段、Job、Recovery Point、比较统计和脱敏 transcript。数据正确性只比较
   本轮生成的 scenario 目录，使用文件相对路径、大小和 SHA-256。
7. ADR-0015 的其它禁止项保持不变；不得以本 ADR 为新增测试用例、fixture、测试 Target 或其它脚本的依据。

## 备选方案

- 完全手工执行：拒绝，步骤之间的身份关联和结果留存容易出错。
- 把脚本接入 CI：拒绝，整盘还原不可逆且依赖明确隔离的物理测试盘。
- 在 PowerShell 中复制 Service V4 Named Pipe 协议：拒绝，会形成重复协议实现并绕过生产 CLI 边界。
- 把所有测试重新引入仓库：拒绝，本需求只批准一个受限的破坏性验收工具。

## 影响

- ADR-0015 被本 ADR 仅针对上述单一文件部分取代；其余范围继续有效。
- 操作者可以用一次直接运行获得可审计的端到端结果。
- 脚本配置包含设备 ID 和路径，但不包含认证信息。错误配置仍可能造成数据丢失，因此双重目标确认、
  系统盘拒绝、Schedule 源盘拒绝和 Service preflight 都是不可移除的安全门禁。
- 参考生成与比较脚本仍是开发工具，不加入 CMake 或生产安装包。

## 验证

- Windows PowerShell 5.1 parser 无语法错误。
- 默认配置因 `ConfirmDestructiveRestore = $false` 失败关闭，不提交 Backup 或 Restore。
- `AegraCLI`、Service 与 Worker 的 Release 生产 Target 构建成功，源码规模检查通过。
- 在隔离的三盘环境人工执行时，结果 JSON 必须分别记录 Backup/Restore 终态和 SHA-256 比较结果。
