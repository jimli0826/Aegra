# `repository` 模块开发文档

## 目标

实现企业版全局去重 CAS Repository。Repository 是自描述数据面，不依赖 PostgreSQL 才能发现和恢复数据。

## 子模块

- `client`：Worker 使用的批量协议客户端。
- `gateway`：鉴权、租约、幂等、提交和限流。
- `pack`：不可变 Pack 与 Local Index。
- `index`：不可变 Segment、分片和 Root Generation。
- `catalog`：Recovery Point 可发现目录。
- `maintenance`：GC、Compaction、Scrub、Repair。

## 权威边界

Pack、Chunk 映射、Manifest、Commit、Catalog 和维护对象以 Repository 为权威。PostgreSQL 只保存控制面与可重建投影。

## 写入事务

```text
Begin -> FindMissingChunksBatch -> UploadPack -> PublishIndex
-> UploadManifest -> ValidateReferences -> PublishCommit -> UpdateCatalog
```

PostgreSQL 投影异步更新，失败不得撤销已发布 Commit。

## 不变量

- 对象默认不可变；Root 使用 generation 条件写。
- Chunk ID 在单 Repository/去重安全域内稳定，默认使用 keyed hash。
- 重试同一 transaction ID 得到幂等结果。
- 未提交 Manifest 和孤儿 Pack 在宽限期后才处理。
- GC 从 Catalog/Manifest 图计算可达性，先发布新 Root，再 Tombstone，宽限期后删除。
- 扫描 Pack Footer 可以重建 Chunk Index。

## 并发与安全

Gateway 是唯一在线写入口。Root 更新必须串行化或使用 compare-and-swap；密钥按 epoch 管理；跨租户去重必须单独安全评审。

## 验证

- 审查并人工验证并发提交、Root 冲突、重复上传和进程崩溃恢复。
- 使用隔离的非生产 Repository 验证索引/Catalog 重建、PostgreSQL 全丢失恢复、GC/Tombstone 和损坏报告。
- 构建 Repository 生产 Target 并执行架构、源码规模、格式和秘密扫描。
