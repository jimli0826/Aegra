# ADR-0001：个人版格式的加密与编解码依赖

- 状态：Accepted
- 日期：2026-08-01
- 决策者：Aegra 项目
- 关联模块：format、pipeline、adapters/personal_archive、adapters/crypto_sodium

## 背景

个人版 `.bkf` V6 需要字符串键 CBOR、Zstandard 压缩、口令密钥派生和 AEAD 元数据保护。
这些能力直接影响持久化格式和恢复可靠性，不应由项目自行实现密码算法，也不能让第三方类型泄漏到
公共头文件中。

原 V6 草案只记录了 Argon2id 的 salt，没有记录计算量和内存量。不同版本或平台采用不同默认值时，
同一口令无法稳定派生同一密钥。因此，格式必须持久化解锁所需的全部 KDF 参数。

## 决策

1. 使用 libsodium 实现 Argon2id v1.3、HKDF-SHA256 和 XChaCha20-Poly1305 AEAD。
2. 元数据算法编号 `2` 明确定义为 XChaCha20-Poly1305，nonce 固定为 24 字节，tag 固定为 16 字节。
3. `CborMetadataEnvelopeHeader` 在保持 124 字节线格式不变的前提下，将原 28 字节预留区改为：
   `kdf_opslimit`、`kdf_memlimit_bytes`、`kdf_parameters_version` 和 8 字节预留区。
4. `kdf_parameters_version == 1` 表示 Argon2id v1.3 参数语义；读取器拒绝不支持的参数版本、零值参数
   和超过产品安全上限的参数，避免恶意文件导致资源耗尽。
5. 使用 nlohmann/json 的 CBOR 编解码实现，但所有正式 Map key 必须是 UTF-8 字符串；其类型只允许
   出现在 `.cpp` 文件中。
6. 使用 Zstandard 实现 chunk payload 压缩。解压前必须由受信任的 manifest/block entry 给出输出上限，
   不得根据不可信输入进行无限分配。
7. 依赖通过仓库根目录的 vcpkg manifest 统一声明；公共接口只暴露 Aegra 和 C++ 标准库类型。
8. 本项目尚未发布，不提供旧草案兼容分支，也不保留旧算法名称别名。
9. 工程规范例外：`personal_archive` 可以通过 PRIVATE target dependency 组合
   `compression_zstd` 与 `crypto_sodium`。这两个模块是无状态算法边界，不持有外部连接、设备或业务
   生命周期，也不向公共头泄漏第三方类型。当前只有一个已定稿的持久化算法实现，把它们提升为运行时
   Port 会将格式策略和无意义的可替换性推到 Composition Root。依赖必须保持单向、PRIVATE 且仅限
   Archive Adapter；出现第二种产品算法实现、动态算法选择或其它业务 Adapter 复用时，必须取消该
   例外并提取独立 Port/codec 层。

## 备选方案

- 自行实现 Argon2、AEAD 或 CBOR：安全审计和格式边界风险过高，不采用。
- OpenSSL：能够覆盖部分能力，但 XChaCha20-Poly1305 和 Argon2id 的组合不如 libsodium 直接，且会
  增加适配面。
- AES-256-GCM：需要严格管理较短 nonce 的唯一性；个人备份文件更适合使用随机 192-bit nonce 的
  XChaCha20-Poly1305。
- 固定 KDF 参数但不写入文件：升级默认值会破坏可恢复性，不采用。

## 影响

- 构建环境需要安装 vcpkg manifest 中声明的三个依赖。
- `.bkf` 的 envelope 仍为 124 字节，但原预留字节现在具有明确语义。
- 格式读取器必须先做长度和资源上限检查，再执行 KDF、解密、CBOR 解析或解压。
- 后续若替换库，只要保持线格式和算法语义不变，不影响公共 API。
- 架构检查需要允许上述两个纯算法 target 作为 Personal Archive 的私有叶子依赖，但不得放宽其它
  Adapter-to-Adapter 依赖。

## 验证

- golden-byte 测试验证所有整数的小端序和固定结构尺寸。
- 测试验证 CBOR 根 Map 和嵌套 Map 仅使用字符串键。
- 测试验证错误口令、AAD 篡改、tag 篡改和不支持的 KDF 参数均被拒绝。
- 测试验证压缩往返和超出解压上限时失败。
- Debug、Release 构建与 clang-tidy 不出现第三方类型泄漏。

## 参考

- [libsodium password hashing](https://doc.libsodium.org/password_hashing/default_phf)
- [libsodium XChaCha20-Poly1305](https://doc.libsodium.org/secret-key_cryptography/aead/chacha20-poly1305/xchacha20-poly1305_construction)
- [nlohmann/json CBOR](https://json.nlohmann.me/features/binary_formats/cbor/)
- [Zstandard API manual](https://facebook.github.io/zstd/zstd_manual.html)
