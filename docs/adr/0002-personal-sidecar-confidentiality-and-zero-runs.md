# ADR-0002：个人版 Sidecar 保密性与 ZERO Run

- 状态：Accepted
- 日期：2026-08-01
- 决策者：Aegra 项目
- 关联模块：format、adapters/personal_archive、adapters/crypto_sodium

## 背景

V6 草案使用 MD5 并明文保存 `.bhx` 块散列。块散列可用于识别已知内容，即使 `.bkf` metadata
已经加密，明文 Sidecar 仍会形成不必要的信息泄露。产品尚未发布，可以直接修正格式而无需兼容
试验文件。V6 也已经为 ZERO 定义 run-length 语义，但此前读写器尚未实现。

## 决策

1. Sidecar V1 头固定为 96 字节。偏移 `56..79` 保存 24 字节 XChaCha20 nonce，`80..95` 保存
   16 字节 detached authentication tag。
2. Sidecar payload 先按 level 3 使用 Zstandard 压缩，再使用 XChaCha20-Poly1305 加密；头部将 tag
   清零后的完整 96 字节作为 AAD。
3. Sidecar 密钥由对应 `.bkf` metadata envelope 的 Argon2id 参数和 salt 派生，但使用独立的
   `MYBACKUP-V6-SIDECAR` HKDF context，与 metadata key 分离。每个 Sidecar 使用独立随机 nonce。
4. DATA 块使用 SHA-256；ZERO 和 FREE 的 hash 字段必须全零。V1 写入器不写 MD5，也不接受其它
   hash 算法。
5. `.bhx` 不是恢复依赖。Reader 打开和恢复 `.bkf` 时不要求 Sidecar 存在；增量比较显式加载并认证
   Sidecar。
6. 写入器为每个逻辑块生成一条 Sidecar record。连续 ZERO 块在同一 chunk 内合并为一个
   `BlockEntry`，`logical_size` 保存 run-length；Footer 块计数按 run 展开。
7. 文件系统空闲簇及显式排除的 pagefile/hiberfil/swapfile extent 使用 FREE 状态，不能降级为 ZERO。
   FREE 不读取、不散列、不保存 payload；连续 FREE 块编码为独立 FREE run，恢复时跳过对应目标写入。
8. `.bkf.partial` 和 `.bkf.bhx.partial` 均完成后才发布。若第二个 rename 失败，写入器删除本次刚发布
   的 `.bkf`，避免向调用者报告一个缺少承诺产物的成功提交。

## 备选方案

- 保持 MD5 明文 Sidecar：存在碰撞弱点和已知内容指纹泄露，不采用。
- 仅改用明文 SHA-256：改善碰撞强度但不解决指纹泄露，不采用。
- 对每条 hash 使用独立 AEAD：空间和计算开销高于一次保护整个压缩 payload，不采用。
- Sidecar 缺失时禁止恢复：Sidecar 是可重建的比较数据，不应扩大恢复故障域，不采用。

## 影响

- 加载 Sidecar 需要对应 `.bkf`、口令以及其 metadata envelope 中的 KDF 参数。
- Sidecar 内容篡改、头字段篡改和错误口令在解析 record 前被 AEAD 拒绝。
- 当前实现为一个 volume 生成 Sidecar；线格式保留多个 volume 的 payload 表。
- ZERO/FREE run 只在 chunk 内合并，不跨 chunk 建立隐式状态，保持 chunk 独立可读。

## 验证

- golden/roundtrip 测试验证 96 字节头、小端字段、volume header 和 record。
- 密码测试验证 Sidecar AEAD 往返、错误口令和已知 SHA-256 向量。
- 端到端测试验证 ZERO run、Footer 展开计数、Sidecar 状态和恢复字节一致。
- Debug、Release、clang-format、clang-tidy 和 `git diff --check` 作为合入门禁。
