# 旧项目文档迁移清单

| 属性 | 内容 |
| --- | --- |
| 来源仓库 | `D:\Work\OpenSource\backup\document` |
| 目标仓库 | `D:\Work\OpenSource\Aegra\docs` |
| 原则 | 迁移有效事实，不迁移旧实现耦合与未发布兼容负担 |

## 已迁移或提炼

| 旧文档 | 新位置 | 处理 |
| --- | --- | --- |
| `format/BACKUP_FORMAT_V6_CBOR.md`、最终 `src/archive/backup_format.h` 及分卷读写代码 | `format/PERSONAL_BACKUP_FORMAT_V6.md` | 迁移为权威格式；CBOR string key；统一 `.bkf`；补齐旧文档漏写的分卷字段与语义；删除 V5 兼容叙述 |
| `branding/BRAND.md` | `branding/BRAND.md` | 保留品牌，按 CMake/新进程命名更新工程标识 |
| `requirements/Product_Requirements_Document.md` | `requirements/PRODUCT_SCOPE.md` | 保留产品能力和非功能不变量，移除过期市场快照与旧技术方案 |
| `design/MODULAR_ARCHITECTURE_DEVELOPMENT_GUIDE.md` | `architecture/MODULAR_ARCHITECTURE.md`、`modules/*.md` | 拆成目标架构和模块执行契约 |
| `design/WINPE_SYSTEM_RESTORE_DESIGN.md` | `modules/apps.md` | 提炼离线状态机、预检、磁盘重匹配、一次性启动和最小 PE 依赖 |
| `design/dokan-virtual-file.md` | `modules/adapters.md`、`modules/apps.md` | 提炼只读 backing、COW、锁顺序、回调隔离和 Mount Host 边界 |
| `api/DISKBACKUP_JOB_JSON.md`、`api/TASK_SYSTEM.md` | `modules/contracts.md`、`modules/application.md` | 保留版本、Job、Progress、SecretRef 和任务语义；旧 JSON 字段不冻结 |
| `design/Repository_Management_Design.md` | `modules/format.md`、`modules/repository.md` | 区分个人 `.bkf` 管理与企业 CAS；不迁移旧 SQLite/REST 实现 |
| `development/VERSIONING.md` | 工程规范、Contracts/Format 文档 | 保留显式版本化原则，不迁移旧 rc 文件清单 |

## 不迁移为现行规范

| 旧文档 | 原因 |
| --- | --- |
| `format/BACKUP_FORMAT_V5.md` | 产品未发布，新项目只实现定稿 V6 |
| `development/CODE_MIGRATION_V5.md` | 不需要迁移和兼容路径 |
| `development/V6_FORMAT_REFACTOR_PLAN.md` | 实施计划已被定稿格式取代 |
| `format/ENCRYPTION.md` | AES-XTS/PBKDF2 旧设计与 V6 AEAD/KDF 层次冲突 |
| `design/Software_Design_Document.md` | 描述旧模块和进程耦合 |
| `design/Technical_Architecture_Design.md` | 规模大且包含被目标架构推翻的旧依赖 |
| `development/DATABASE.md` | 旧个人版 SQLite Schema，不适用于 PostgreSQL 控制面 |
| `development/REFACTORING_PARTITION_MANAGER.md` | 旧源码重构过程，不是新模块契约 |
| `api/REST_API.md` | 旧路由未冻结，新 Service API 需依据 Use Case 重新设计 |
| `status/*.md` | 旧仓库实现完成度与 TODO，不代表新项目状态 |
| `assets/ui-modern-prototype.html` | 旧 UI 原型；新 Desktop 设计尚未开始 |

## 使用规则

- 新实现只引用 `Aegra/docs`，不得把旧仓库路径写入源码或现行规范。
- 需要找回未迁移细节时，先判断其是否符合新架构；符合后提炼到对应模块文档。
- 任何把“不迁移”文档重新引入现行规范的决定必须提交 ADR。
