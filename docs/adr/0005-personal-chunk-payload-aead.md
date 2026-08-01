# ADR-0005：个人版 Chunk Payload 认证加密

- 状态：Accepted
- 日期：2026-08-01
- 决策者：Aegra 项目
- 关联模块：format、adapters/personal_archive、adapters/crypto_sodium

## 背景

个人版 V6 已使用 XChaCha20-Poly1305 保护 CBOR metadata 和 `.bhx` Sidecar，但现有 Chunk Payload
仅经过 Zstandard 压缩后直接写入文件。`BackupHeader` 同时设置了 `ENCRYPTED` 标志，导致格式声明与
实际保护范围不一致；RAW 数据会明文暴露，Payload 或 BlockEntry 的可解析篡改也没有密码学认证。

产品尚未发布，可以直接修订 V6 线格式，不保留未加密 Payload 的兼容读取路径。

## 决策

1. 正式 V6 文件的 `BackupHeader.encryption_method` 固定为 XChaCha20-Poly1305，Reader 拒绝 `NONE`
   和其它算法；`BACKUP_FLAG_ENCRYPTED` 表示 metadata、Chunk 索引和 Payload 都受到保护。
2. `ChunkHeader` 从 52 字节调整为 96 字节，新增 24 字节 `payload_nonce` 和 16 字节
   `payload_authentication_tag`，尾部 4 字节保留区必须为零。
3. 每个有 Payload 的 Chunk 使用密码学安全随机源生成独立 nonce。使用 detached AEAD 加密整个
   Chunk Payload，ciphertext 长度与 plaintext 长度相同，`payload_size` 和 BlockEntry offset/size
   均描述 ciphertext 与解密后压缩流的相同字节范围。
4. AEAD AAD 是 authentication tag 清零后的完整 96 字节 ChunkHeader，随后拼接该 Chunk 的全部
   编码后 `BlockEntry[]`。因此 Chunk 身份、数据源、逻辑块映射、压缩标志和 Payload 范围都被认证。
5. Payload key 从对应 metadata envelope 的 Argon2id master key 派生，HKDF-SHA256 context 固定为
   `MYBACKUP-V6-CHUNK-PAYLOAD`，不得复用 metadata 或 Sidecar key。
6. Reader 在任何解压、ZERO 展开或向 Pipeline 返回数据之前，必须读取完整 Chunk ciphertext 并完成
   AEAD 认证。认证失败返回稳定的未授权/认证错误，不返回部分明文。
7. 只有 ZERO entry 的 Chunk 允许 Payload 为空，但仍必须生成 nonce/tag，对 ChunkHeader 和
   BlockEntry[] 执行空明文 AEAD，从而认证 ZERO run 的逻辑映射。
8. Writer 在内存中准备并加密一个完整 Chunk 后才执行分卷判断和持久化，保持原有 chunk 边界分卷、
   partial 文件和首卷最后发布语义。

## 备选方案

- AES-256-XTS：只提供保密性，不提供认证，且需要额外完整性结构，不采用。
- 每个 Block 单独保存 nonce/tag：随机读取粒度更小，但显著扩大索引并增加格式复杂度；当前 Reader 和
  Pipeline 的最小读取单位都是 Chunk，不采用。
- 只保存 Chunk CRC：可以检测偶然损坏，不能防止恶意篡改或保护 RAW 数据，不采用。
- 对 Header、索引和 Payload 一起加密：读取 Payload 前无法获得块索引，破坏顺序恢复布局，不采用。

## 影响

- Reader 读取一个 Chunk 时需要暂存完整 ciphertext 和 plaintext，内存上限继续由
  `maximum_chunk_payload_size` 与 `maximum_chunk_logical_size` 控制。
- Chunk 索引仍为明文以支持扫描和范围预检，但只有 AEAD 认证成功后才能作为恢复输入使用。
- 分卷容量计算使用新的 96 字节 ChunkHeader。
- 旧试验 `.bkf` 文件不再可读，这是未发布产品的有意格式收敛。

## 验证

- Golden-byte 测试验证 96 字节 ChunkHeader、nonce/tag offset 和算法编号。
- libsodium 单元测试验证 Payload 往返、AAD 篡改、ciphertext 篡改和 tag 篡改。
- Archive 端到端测试验证 Payload 不含已知 RAW 明文，正确口令恢复一致。
- 损坏测试分别篡改 ChunkHeader、BlockEntry、ciphertext 和 tag，并验证 Reader 在返回数据前拒绝。
- Debug、Release、clang-format、源码规模检查和 `git diff --check` 作为合入门禁。
