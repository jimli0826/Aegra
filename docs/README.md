# Aegra 文档中心

本文档树只保存新项目的现行规范。旧仓库文档的迁移与淘汰结论见[文档迁移清单](migration/LEGACY_DOCUMENT_MIGRATION.md)。

## 开发必读

1. [C++ 工程开发规范](development/CPP_ENGINEERING_STANDARD.md)
2. [模块化目标架构](architecture/MODULAR_ARCHITECTURE.md)
3. [模块开发文档索引](modules/README.md)
4. [架构决策记录规则](adr/README.md)

专题设计与执行计划：

- [文件集备份与恢复设计](architecture/FILE_SET_BACKUP_RESTORE.md)
- [文件集备份与恢复分阶段开发计划](development/FILE_SET_BACKUP_DEVELOPMENT_PLAN.md)
- [文件集增量备份与链式恢复设计](architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md)
- [文件集增量备份分阶段开发计划](development/FILE_SET_INCREMENTAL_DEVELOPMENT_PLAN.md)
- [文件集产品上限、稳定码与验证矩阵](development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md)

现行核心决策：

- [ADR-0001：个人版格式的加密与编解码依赖](adr/0001-personal-format-crypto-and-codec-dependencies.md)
- [ADR-0002：个人版 Sidecar 保密性与 ZERO Run](adr/0002-personal-sidecar-confidentiality-and-zero-runs.md)
- [ADR-0003：个人版分卷事务与多数据源边界](adr/0003-personal-split-archive-and-multi-source-boundary.md)
- [ADR-0004：个人版稀疏增量层与链式恢复视图](adr/0004-personal-incremental-layers-and-chain-reader.md)
- [ADR-0010：个人版 Repository 权威边界与可重建目录](adr/0010-personal-repository-authority-and-catalog.md)
- [ADR-0015：项目不维护自动化测试用例](adr/0015-no-project-test-suite.md)
- [ADR-0016：文件集备份、恢复与个人 Archive V7 边界](adr/0016-file-set-backup-and-restore-boundary.md)
- [ADR-0017：本地 Service 控制面协议 V4](adr/0017-service-control-protocol-v4.md)
- [ADR-0018：文件集增量备份的 USN 基线与链式恢复](adr/0018-file-set-incremental-usn-and-chain.md)

## 权威格式与需求

- [个人版 `.bkf` V7 格式](format/PERSONAL_BACKUP_FORMAT_V7.md)
- [个人版 Repository Descriptor 与 Catalog V2](format/PERSONAL_REPOSITORY_FORMAT_V2.md)
- [本地 Service 控制面协议 V4（逐条 wire 说明）](protocol/SERVICE_CONTROL_PROTOCOL_V4.md)
- [产品范围](requirements/PRODUCT_SCOPE.md)
- [品牌与命名](branding/BRAND.md)

开发史（生产不实现）：

- [个人版 `.bkf` V6 格式](format/PERSONAL_BACKUP_FORMAT_V6.md)
- [个人版 Repository Catalog V1](format/PERSONAL_REPOSITORY_FORMAT_V1.md)
- [本地 Service 控制面协议 V3](protocol/SERVICE_CONTROL_PROTOCOL_V3.md)

## 文档权威顺序

发生冲突时按以下顺序处理：

1. 已接受的 ADR；
2. `docs/format` 中的持久化格式规范；
3. `docs/architecture/MODULAR_ARCHITECTURE.md`；
4. `docs/development/CPP_ENGINEERING_STANDARD.md`；
5. `docs/modules` 中的模块开发文档；
6. 产品需求和其它说明文档。

旧仓库路径、类名、REST 路由、SQLite 表和功能完成度不构成新项目行为依据。

## 维护规则

- 行为、格式、协议或模块边界变化必须在同一变更中更新对应文档。
- 不复制同一规范到多个文件；其它文档使用链接引用权威来源。
- 不把临时实现计划写成永久架构；不可逆决策进入 `docs/adr`。
- 所有现行文档使用仓库相对链接。
