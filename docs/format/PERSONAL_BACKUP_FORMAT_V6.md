# 个人版备份文件格式 V6（`.bkf`）

| 属性 | 内容 |
| --- | --- |
| 状态 | 权威格式规范 |
| 格式版本 | 6 |
| CBOR Schema | 1 |
| 来源 | 从旧项目 V6 规范迁移，与最终 V6 读写代码核对后按新架构定稿 |

> 本规范是新项目个人版 `.bkf` 的唯一格式依据。产品尚未发布，不读取整数 CBOR key、V5 结构或其它试验格式，也不实现迁移与兼容分支。

## 概述

V6 使用固定二进制启动结构、加密 CBOR 元数据和 chunk 内索引前置布局，使还原端只需要顺序读取备份文件即可恢复数据。单个备份既可以由一个 `.bkf` 文件承载，也可以按 chunk 边界拆分为 `.bkf`、`.bkf.001`、`.bkf.002` 等分卷。

- 固定二进制 `BackupHeader`：用于启动读取。
- 加密 CBOR 元数据：用于保存磁盘、分区、系统信息和原始分区表数据。
- `ChunkHeader`：描述一个 chunk 所属数据源、块索引区和 Payload AEAD 参数。
- chunk 内部紧凑二进制 `BlockEntry[]`：作为该 chunk 内的权威块索引。
- 固定二进制 `BackupFooter`：作为完成标记和全局统计信息。

该设计支持一个备份文件包含多个磁盘和多个卷。`disks[]/partitions[]` 表达物理布局和裸机恢复所需的分区表信息，`volumes[]` 表达 VSS 一致性数据源及其到物理分区/extent 的映射。一个 chunk 只能包含一个 volume 的逻辑地址空间数据；整盘备份不直接读 `PhysicalDrive`，而是展开为该磁盘上所有可备份 volume，并通过 VSS 读取这些 volume。

当前产品实现已经支持多 Volume 全量与多 Volume 增量 Archive：所有选中 Volume 位于同一个 VSS
Snapshot Set，按 `volumes[]` 顺序写入同一个逻辑 chunk 流，并在最后一个 Volume 完成后一次性提交
`.bkf` 与多 Volume Sidecar。增量创建时父 Archive / 父 Sidecar 的有序 `volume_index`、`volume_id`、
`total_size` 与块记录数必须与本次 Job 完全一致；Chain Reader 对 base-first 层列表做相同几何校验，
按 `source_index` 与 per-volume `logical_offset` 叠层。不得退化为只处理第一个 Volume。
多目标（多 volume 同时写多个独立目标）Restore 的显式映射仍属后续工作；通用 Restore Pipeline 在
未提供目标映射时对 `source_index != 0` 的 chunk 明确拒绝。

## 文件布局

```text
+----------------------------+
| BackupHeader               |
+----------------------------+
| Encrypted CBOR Metadata    |
+----------------------------+
| Chunk 0 Header             |
| Chunk 0 BlockEntry[]       |
| Chunk 0 Payloads           |
+----------------------------+
| Chunk 1 Header             |
| Chunk 1 BlockEntry[]       |
| Chunk 1 Payloads           |
+----------------------------+
| ...                        |
+----------------------------+
| BackupFooter               |
+----------------------------+
```

设计规则：

- `BackupHeader` 只保存写入块数据前就能确定的信息。
- CBOR 保存可扩展的描述性元数据，并在文件中以加密 envelope 形式存储。
- 每个 chunk 的索引位于 payload 之前。
- 还原程序顺序读到一个 chunk 后，可以先从 `ChunkHeader` 得到该 chunk 所属分区，再读取 `BlockEntry[]`，随后顺序读取 payload 并写回目标分区。
- `Block Payloads` 保存经过压缩和 Chunk 级认证加密的真实块数据，不再为每个块写入 `BlockHeader`。
- chunk 内 `BlockEntry[]` 是该 chunk 内块恢复元数据的唯一权威来源。
- 每个 chunk 只能属于一个数据源；当前数据源类型为 volume，由 `ChunkHeader.source_type + ChunkHeader.source_index` 指定。
- `BackupFooter` 保存备份完成标记和全局统计信息；正常顺序恢复不依赖 Footer 中的索引偏移。
- 非分卷文件同时是首卷和末卷；分卷文件只有首卷包含 CBOR metadata，只有末卷包含 `BackupFooter`。

## 二进制结构

所有二进制结构都使用小端整数，并使用紧凑布局。

### BackupHeader

`BackupHeader` 写在文件开头，固定 256 字节。

```cpp
#pragma pack(push, 1)
struct BackupHeader {
    char     magic[8];             // "MYBACKUP"
    uint16_t header_version;       // Header 结构版本
    uint16_t format_version;       // 文件格式版本
    uint32_t header_size;          // sizeof(BackupHeader)

    uint8_t  file_uuid[16];        // 当前备份文件 UUID
    uint8_t  backup_set_uuid[16];  // 备份集 / 备份链 UUID
    uint8_t  parent_uuid[16];      // 父备份 UUID；全量备份为全 0

    uint32_t block_size;           // 逻辑块大小，单位字节
    uint32_t flags;                // FULL / INCREMENTAL / DIFFERENTIAL / DEDUP / ENCRYPTED / SPLIT

    uint64_t cbor_offset;          // 加密 CBOR Metadata envelope 偏移
    uint64_t cbor_size;            // 加密 CBOR Metadata envelope 总大小，单位字节
    uint32_t cbor_schema_version;  // CBOR plaintext schema 版本

    uint64_t first_chunk_offset;   // 第一个 ChunkHeader 的偏移
    uint32_t default_chunk_size;   // 默认 chunk 目标大小，暂定 512MB

    uint8_t  compression_method;   // 默认压缩算法
    uint8_t  encryption_method;    // 默认加密算法

    uint32_t split_part_index;     // 分卷序号，从 0 开始；非分卷为 0
    uint32_t split_part_count;     // 总分卷数；写入期间或未知时为 0
    uint64_t split_size_bytes;     // 分卷目标上限；未启用分卷时为 0

    uint8_t  reserved[134];        // 预留，必须初始化为 0
};
static_assert(sizeof(BackupHeader) == 256);
#pragma pack(pop)
```

`flags` 字段使用以下位标志描述备份类型与处理方式：

```cpp
constexpr uint32_t BACKUP_FLAG_FULL         = 0x00000001; // 全量备份
constexpr uint32_t BACKUP_FLAG_INCREMENTAL  = 0x00000002; // 增量备份（父为上一次备份）
constexpr uint32_t BACKUP_FLAG_DIFFERENTIAL = 0x00000004; // 差异备份（父为基准全量）
constexpr uint32_t BACKUP_FLAG_DEDUP        = 0x00000008;
constexpr uint32_t BACKUP_FLAG_ENCRYPTED    = 0x00000010;
constexpr uint32_t BACKUP_FLAG_SPLIT        = 0x00000020; // 多文件分卷
```

正式 V6 文件固定使用以下 Payload 算法编号：

```cpp
constexpr uint8_t PAYLOAD_ENC_NONE                   = 0;
constexpr uint8_t PAYLOAD_ENC_XCHACHA20_POLY1305     = 2;
```

`BACKUP_FLAG_ENCRYPTED` 必须设置，`BackupHeader.encryption_method` 必须为
`PAYLOAD_ENC_XCHACHA20_POLY1305`。该标志表示 metadata、Chunk 索引和 Payload 均受到 AEAD 保护；
正式 Reader 不接受 Payload 明文文件。

备份链通过三个 UUID 字段表达：

- `file_uuid`：本备份文件的唯一标识。
- `backup_set_uuid`：同一条备份链共享同一个值，由基准全量生成并被链上所有成员继承。
- `parent_uuid`：父备份的 `file_uuid`；全量备份为全 0，差异备份指向基准全量，增量备份指向上一次备份。

`chunk_count`、总块数、最终 `file_size` 等字段不放在 Header 中，因为这些值在开始写入文件时还不知道。

### 分卷布局

启用 `BACKUP_FLAG_SPLIT` 后，路径命名固定为：

```text
part 0: <name>.bkf
part 1: <name>.bkf.001
part 2: <name>.bkf.002
...
```

所有分卷都以完整的 256 字节 `BackupHeader` 开始，并且必须具有相同的 `file_uuid`、`backup_set_uuid`、`parent_uuid`、格式版本、块大小、算法字段和 `split_size_bytes`。各卷差异如下：

| 字段/区域 | 首卷 `part 0` | 续卷 `part > 0` |
| --- | --- | --- |
| `split_part_index` | `0` | 与文件后缀对应的序号 |
| `cbor_offset` | `sizeof(BackupHeader)` | `sizeof(BackupHeader)` |
| `cbor_size` | 必须大于 0 | 必须为 0 |
| `first_chunk_offset` | `cbor_offset + cbor_size` | `sizeof(BackupHeader)` |
| CBOR metadata | 有且仅有一份 | 不存在 |

分卷只允许在完整 chunk 边界切换，不能把一个 `ChunkHeader + BlockEntry[] + Payloads` 拆到两个文件中。除末卷外，各卷在最后一个完整 chunk 后直接结束；全局 `BackupFooter` 只写入末卷。`BackupFooter.file_size` 表示末卷自身的字节数，不表示所有分卷大小之和；全局数据量由 Footer 中其它统计字段表达。

`split_size_bytes` 是写入器的目标上限，不是每卷必须达到的精确大小。单个完整 chunk 加头部所需空间可能使实际大小接近或超过该目标，因此写入器必须根据目标上限调整 `default_chunk_size`，并为末卷 Footer 保留空间。

V6 写入器固定将 `split_part_count` 写为 0：首卷 Header 同时是 metadata AEAD 的 AAD，提交时不能回写总卷数。读取器从 `.001` 开始按连续序号发现续卷，直到遇到末卷 Footer，并受产品配置的最大分卷数限制。格式解析器仍校验非零声明的一致性，以保持字段本身的自描述约束。序号不连续、UUID 不一致、Footer 后仍有续卷或末卷缺少 Footer均表示备份不完整或损坏。

### ChunkHeader

每个 chunk 以 `ChunkHeader` 开始。chunk 内索引区位于 payload 之前，使还原程序无需 seek 到文件尾部即可顺序恢复。

默认 chunk 目标大小暂定为 512MB：

```cpp
constexpr uint32_t DEFAULT_CHUNK_SIZE = 512 * 1024 * 1024;
```

`ChunkHeader` 采用固定大小结构。因为一个 chunk 只能包含一个分区的数据，所以分区定位字段直接放在 header 中，不再需要 range 表。每个 Chunk 使用独立随机 nonce 和 detached authentication tag。

```cpp
#pragma pack(push, 1)
struct ChunkHeader {
    char     magic[8];             // "MYBKCHK"
    uint32_t header_size;          // sizeof(ChunkHeader)
    uint64_t chunk_index;          // 从 0 开始递增

    uint8_t  source_type;          // 1=volume
    uint8_t  reserved_source[3];   // 对齐，必须初始化为 0
    uint32_t source_index;         // 对应 CBOR volume.volume_index

    uint32_t block_entry_count;    // 本 chunk 内 BlockEntry 数量
    uint32_t reserved0;            // 对齐，必须初始化为 0

    uint64_t payload_size;         // 本 chunk payload 总字节数

    uint32_t flags;                // 预留 chunk 级标志；未使用时为 0
    uint32_t header_crc32;         // 可选；未启用时为 0

    uint8_t  payload_nonce[24];    // 本 Chunk 独立 XChaCha20 nonce
    uint8_t  payload_authentication_tag[16]; // detached AEAD tag
    uint8_t  reserved1[4];         // 预留，必须初始化为 0
};
static_assert(sizeof(ChunkHeader) == 96);
#pragma pack(pop)
```

一个 chunk 的内部布局固定为：

```text
ChunkHeader
BlockEntry[]
Payloads
```

读取端读取固定 `ChunkHeader` 后，读取 `block_entry_count * sizeof(BlockEntry)` 字节的块索引，再顺序读取 `payload_size` 字节的 ciphertext。范围预检可以使用明文索引，但在 AEAD 认证成功前不得解压、展开或向恢复目标返回任何数据。

Chunk Payload 使用 XChaCha20-Poly1305 detached 模式整体加密。AAD 按以下顺序拼接：

1. 将 `payload_authentication_tag` 清零后的完整 96 字节 `ChunkHeader`；
2. 当前 Chunk 的全部编码后 `BlockEntry[]`。

Payload ciphertext 与 plaintext 长度相同。只有 ZERO entry 的 Chunk 允许 `payload_size == 0`，但仍须生成独立 nonce/tag，以空明文 AEAD 认证 Header 和 BlockEntry。nonce 必须由密码学安全随机源生成；同一 Payload key 下不得重复。

下一个记录的 magic 如果是 `MYBKCHK`，表示继续读取下一个 chunk；如果是 `MYBKEND`，表示 chunk 流结束并进入 Footer。

### BlockEntry

`BlockEntry` 是恢复一个逻辑块所需的权威元数据。

```cpp
#pragma pack(push, 1)
struct BlockEntry {
    uint64_t logical_block_index; // 相对于分区的逻辑块号

    union {
        uint64_t payload_offset;  // RAW/COMPRESSED: 相对于本 chunk payload 起始位置的偏移
        uint64_t ref_index;       // DEDUP: 引用的 BlockEntry 下标
    } ptr;

    uint32_t stored_size;         // 文件中实际存储大小；ZERO/DEDUP 为 0
    uint32_t logical_size;        // RAW/COMPRESSED: 有效 payload 大小；ZERO: run-length
    uint8_t  flags;               // RAW / COMPRESSED / ZERO / DEDUP
};
static_assert(sizeof(BlockEntry) == 25);
#pragma pack(pop)
```

`stored_size` 表示 Payload ciphertext 中该块压缩流的字节数。XChaCha20-Poly1305 detached 模式不改变 Payload 长度，因此它也等于解密后对应压缩流的字节数；authentication tag 只保存在 `ChunkHeader`，不计入 `stored_size`。

`logical_size` 在当前 V6 实现中的语义：

- RAW：未压缩原始块大小，最后一个块可能小于 `block_size`。
- COMPRESSED：压缩数据大小，即解密后传给解压器的有效长度。
- ZERO：连续 ZERO 块数量（run-length，最小为 1）。
- DEDUP：为 0；恢复时使用被引用 entry 的 `logical_size`。

空闲块（free/unused）使用内部态 `BLOCK_FLAG_SKIP` 表示，但不会序列化为 `BlockEntry`。写文件时直接省略对应逻辑块，恢复端通过逻辑块索引空洞将其视为跳过块，不写盘。

推荐块标志：

```cpp
constexpr uint8_t BLOCK_FLAG_RAW        = 0x01;
constexpr uint8_t BLOCK_FLAG_COMPRESSED = 0x02;
constexpr uint8_t BLOCK_FLAG_ZERO       = 0x04;
constexpr uint8_t BLOCK_FLAG_DEDUP      = 0x08;
constexpr uint8_t BLOCK_FLAG_SKIP       = 0x10; // 内部态，不落盘
```

不再为每个块保存解压后的 `original_size`。恢复时按分区大小计算：

```cpp
logical_size = min(block_size,
                   partition.size - logical_block_index * block_size);
```

全量备份中，`logical_block_index` 通常等于该块在分区内的位置。增量备份中，它表示变化块在原分区中的真实位置。

为保证顺序恢复，`BLOCK_FLAG_DEDUP` 的 `ref_index` 默认只允许引用当前 chunk 内较早出现的 `BlockEntry`。跨 chunk 去重会要求恢复端缓存历史块或随机回读备份文件，和顺序恢复目标冲突；如未来需要跨 chunk 去重，应作为独立扩展能力声明。

### BackupFooter

`BackupFooter` 最后写入，固定 512 字节，作为完成标记和全局统计信息。顺序恢复不依赖 Footer 中的索引目录，但正式恢复必须验证 Footer；Footer 缺失时只能由独立的诊断/抢救工具扫描完整 chunk，不能把该 Archive 视为已完成备份。

```cpp
#pragma pack(push, 1)
struct BackupFooter {
    char     magic[8];             // "MYBKEND"
    uint16_t footer_version;       // Footer 结构版本
    uint16_t format_version;       // 文件格式版本
    uint32_t footer_size;          // sizeof(BackupFooter)

    uint64_t chunk_count;          // 完整 chunk 数量
    uint64_t total_block_count;    // 所有数据源的逻辑块总数（含 skip 和 ZERO run 展开）
    uint64_t total_payload_size;   // 所有 chunk payload 总大小
    uint64_t file_size;            // footer_offset + sizeof(BackupFooter)

    uint8_t  reserved[464];        // 预留，必须初始化为 0
};
static_assert(sizeof(BackupFooter) == 512);
#pragma pack(pop)
```

如果 Footer 缺失或校验失败，应认为该备份文件未完成或已损坏。

`file_size` 在写入 Footer 前计算：

```cpp
footer.file_size = currentOffset + sizeof(BackupFooter);
```

## CBOR 元数据

CBOR plaintext 保存磁盘、分区、卷、系统和任务元数据。plaintext 不直接写入文件，而是先编码为 CBOR，再整体加密后写入 `Encrypted CBOR Metadata` 区域。

### 加密 Envelope

`BackupHeader.cbor_offset` 指向 `CborMetadataEnvelopeHeader`，`BackupHeader.cbor_size` 覆盖 envelope header、可选 key slot、ciphertext 和 authentication tag。`BackupHeader.first_chunk_offset` 必须等于 `cbor_offset + cbor_size`。

```text
+----------------------------+
| CborMetadataEnvelopeHeader |
+----------------------------+
| CborKeySlot[] optional     |
+----------------------------+
| CBOR Ciphertext            |
+----------------------------+
| AEAD Tag                   |
+----------------------------+
```

`CBOR Ciphertext` 是完整 CBOR 根 Map 的密文。密文解密成功前，读取器不能尝试解析 `disks[]`、`system`、`backup_job` 或 `raw_layout`。

```cpp
#pragma pack(push, 1)
struct CborMetadataEnvelopeHeader {
    char     magic[8];             // "MYBKCBR"
    uint16_t envelope_version;      // 当前为 1
    uint16_t header_size;           // sizeof(CborMetadataEnvelopeHeader)
    uint32_t flags;                 // 元数据加密标志

    uint8_t  encryption_method;     // 元数据 AEAD 算法
    uint8_t  kdf_method;            // 密钥派生算法
    uint8_t  nonce_size;            // nonce 有效字节数
    uint8_t  tag_size;              // AEAD tag 字节数

    uint32_t key_slot_count;        // 后续 CborKeySlot 数量；无 key slot 时为 0
    uint64_t plaintext_size;        // 解密后的 CBOR 字节数
    uint64_t ciphertext_size;       // CBOR ciphertext 字节数

    uint8_t  salt[32];              // KDF salt；不需要 KDF 时全 0
    uint8_t  nonce[24];             // AEAD nonce；按 nonce_size 使用前 N 字节

    uint64_t kdf_opslimit;          // KDF 计算量；不需要 KDF 时为 0
    uint64_t kdf_memlimit_bytes;    // KDF 内存量；不需要 KDF 时为 0
    uint32_t kdf_parameters_version;// Argon2id v1.3 参数语义当前为 1
    uint8_t  reserved[8];           // 预留，必须初始化为 0
};
static_assert(sizeof(CborMetadataEnvelopeHeader) == 124);
#pragma pack(pop)
```

推荐常量：

```cpp
constexpr uint32_t CBOR_META_FLAG_ENCRYPTED = 0x00000001;

constexpr uint8_t META_ENC_NONE                 = 0; // 保留值，产品文件无效
constexpr uint8_t META_ENC_AES_256_GCM          = 1;
constexpr uint8_t META_ENC_XCHACHA20_POLY1305   = 2;

constexpr uint8_t META_KDF_NONE            = 0;
constexpr uint8_t META_KDF_HKDF_SHA256     = 1;
constexpr uint8_t META_KDF_ARGON2ID        = 2;
```

正式备份文件必须设置 `CBOR_META_FLAG_ENCRYPTED`，且 `encryption_method != META_ENC_NONE`。普通读取器必须拒绝使用 `META_ENC_NONE` 或其它未加密 CBOR metadata 的文件。

个人版正式文件当前必须使用 `META_ENC_XCHACHA20_POLY1305`，`nonce_size == 24` 且
`tag_size == 16`。使用 `META_KDF_ARGON2ID` 时，salt 必须随机生成，`kdf_opslimit` 和
`kdf_memlimit_bytes` 必须记录写入时采用的实际值，`kdf_parameters_version` 必须为 1。读取器必须在
执行 KDF 前检查这两个资源参数是否位于产品支持范围内，防止恶意文件触发无界 CPU 或内存消耗。

#### 密钥层次

元数据加密密钥必须和 chunk payload 加密密钥分离，避免同一密钥/nonce 域同时保护不同数据类型。

推荐密钥派生：

```text
metadata_key = HKDF-SHA256(
    input_key_material = backup_master_key,
    salt = CborMetadataEnvelopeHeader.salt,
    info = "MYBACKUP-V6-CBOR-METADATA",
    output_length = 32
)

payload_key = HKDF-SHA256(
    input_key_material = backup_master_key,
    salt = CborMetadataEnvelopeHeader.salt,
    info = "MYBACKUP-V6-CHUNK-PAYLOAD",
    output_length = 32
)
```

如果备份文件使用口令保护，口令不得直接作为 AEAD key。写入器应先用 `META_KDF_ARGON2ID` 或等价强度 KDF 得到 `backup_master_key`，再用 HKDF 派生 `metadata_key` 和 chunk payload key。企业版可用 `CborKeySlot[]` 保存被用户口令、公钥或 KMS 包装后的 master key；key slot 自身不包含明文密钥。

`CborKeySlot` 用于描述一个可尝试解锁备份的密钥入口。个人版可以不写 key slot，而由外部口令配置直接派生 `backup_master_key`；企业版建议写入一个或多个 key slot，以支持多管理员、恢复密钥或 KMS。

```cpp
#pragma pack(push, 1)
struct CborKeySlot {
    uint16_t slot_version;          // 当前为 1
    uint16_t header_size;           // sizeof(CborKeySlot)
    uint32_t wrapping_method;       // PASSWORD / PUBLIC_KEY / KMS 等

    uint8_t  slot_uuid[16];         // key slot UUID
    uint8_t  kdf_method;            // 包装密钥所用 KDF
    uint8_t  wrap_algorithm;        // master key 包装算法
    uint16_t wrapped_key_size;      // wrapped_key 有效字节数

    uint8_t  salt[32];              // slot KDF salt
    uint8_t  nonce[24];             // slot 包装算法 nonce
    uint8_t  wrapped_key[128];      // 被包装的 backup_master_key 或 metadata_key
    uint8_t  reserved[32];          // 预留，必须初始化为 0
};
static_assert(sizeof(CborKeySlot) == 244);
#pragma pack(pop)
```

`wrapped_key` 推荐保存被包装的 `backup_master_key`，这样同一个 master key 可以继续派生 metadata key 和 chunk payload key。若某个实现只包装 `metadata_key`，必须在 `wrapping_method` 或 `wrap_algorithm` 中明确标识，且不能用于解密 chunk payload。

#### AEAD 认证范围

CBOR metadata 必须使用 AEAD 算法加密并认证。AEAD AAD 建议由以下字节串按顺序拼接：

1. `BackupHeader` 的完整 256 字节，其中 `reserved` 字节保持写入值。
2. `CborMetadataEnvelopeHeader`，不包含 ciphertext 和 tag。
3. 每个 `CborKeySlot` 的完整字节。

这样可以防止攻击者篡改 Header 中的 `file_uuid`、`backup_set_uuid`、`cbor_offset`、`cbor_size`、`first_chunk_offset` 或 envelope 加密参数后仍通过认证。读取器必须先验证 AEAD tag，成功后再解析 CBOR plaintext。

#### 明文信息最小化

加密后，文件头仍会暴露以下最小启动信息：文件格式版本、备份链 UUID、块大小、chunk 起始偏移、默认压缩/加密算法枚举、CBOR envelope 大小和加密参数。磁盘型号、序列号、分区表、卷标、文件系统、主机名、用户名、网卡和原始 `raw_layout` 都必须只存在于 CBOR ciphertext 内。

如未来需要在未解密前显示备份摘要，应新增单独的 `PublicMetadata` 区域，并明确其隐私边界；不得把摘要字段混入明文 CBOR 根 Map。

正式文件格式的所有 Map key 必须使用 UTF-8 CBOR text string。字段名采用本文定义的 snake_case 名称；整数 key、数字字符串别名和自动转换均属于无效格式，读取器必须拒绝。

### 根 Map

```text
{
  "schema_version": schema_version,
  "disks": [],
  "system": system,
  "backup_job": backup_job,
  "volumes": [],
  "extensions": extensions
}
```


### Disk Map

`disks[]` 中的每个元素描述一个物理磁盘。

```text
disk = {
  "disk_number": disk_number,
  "disk_size": disk_size,
  "bytes_per_sector": bytes_per_sector,
  "total_sectors": total_sectors,
  "partition_style": partition_style,              // 0=MBR, 1=GPT, 2=RAW

  "mbr_signature": mbr_signature,

  "gpt_disk_guid": gpt_disk_guid,               // bstr, 16 bytes
  "gpt_first_usable_lba": gpt_first_usable_lba,
  "gpt_last_usable_lba": gpt_last_usable_lba,
  "gpt_partition_entry_size": gpt_partition_entry_size,
  "gpt_partition_entry_count": gpt_partition_entry_count,

  "model": model,
  "serial": serial,
  "media_type": media_type,

  "partitions": [],
  "raw_layout": raw_layout
}
```


### Partition Map

每个 partition 属于其所在的 disk。`partition_number` 只要求在同一个 disk 内唯一。

```text
partition = {
  "partition_number": partition_number,
  "offset": offset,
  "size": size,
  "partition_style": partition_style,
  "is_active": is_active,

  "mbr_type": mbr_type,
  "mbr_boot_flag": mbr_boot_flag,

  "gpt_type_guid": gpt_type_guid,               // bstr, 16 bytes
  "gpt_id_guid": gpt_id_guid,                 // bstr, 16 bytes
  "gpt_name": gpt_name,                    // UTF-8 text
  "gpt_attributes": gpt_attributes,

  "volume_label": volume_label,
  "filesystem": filesystem,
  "volume_guid": volume_guid
}
```


partition 不再作为一致性读取源。一个 partition 是否有备份数据，由 `volumes[].extents[]` 是否引用该 `disk_number + partition_number` 且对应 volume 是否有 chunk 判断。

### Volume Map

`volumes[]` 中的每个元素描述一个 VSS 一致性数据源。basic disk 上通常一个 volume 对应一个 partition extent；动态磁盘、跨盘卷、Storage Spaces 可以通过多个 extents 表达。

```text
volume = {
  "volume_index": volume_index,                 // ChunkHeader.source_index
  "volume_id": volume_id,                    // 备份内稳定 ID；Windows 可直接使用 volume_guid
  "volume_guid": volume_guid,                  // Windows Volume GUID path
  "mount_points": [],
  "filesystem": filesystem,
  "label": label,
  "total_size": total_size,
  "cluster_size": cluster_size,

  "source_state": source_state,                // 0=none, 1=selected
  "vss_required": vss_required,
  "vss_used": vss_used,
  "vss_snapshot_set_id": vss_snapshot_set_id,
  "vss_snapshot_id": vss_snapshot_id,
  "consistency_level": consistency_level,           // 0=crash, 1=filesystem, 2=application

  "extents": []
}
```


### Volume Extent Map

```text
extent = {
  "disk_number": disk_number,
  "partition_number": partition_number,
  "physical_offset": physical_offset,
  "volume_offset": volume_offset,
  "length": length,
  "extent_role": extent_role  // basic | dynamic_member | storage_space_member
}
```


### Raw Layout Map

`raw_layout` 保存原始分区表字节，用于高保真恢复分区表。

```text
raw_layout = {
  "mbr_sector": mbr_sector,
  "gpt_primary_header": gpt_primary_header,
  "gpt_partition_entries": gpt_partition_entries,
  "gpt_backup_header": gpt_backup_header,
  "gpt_backup_entries": gpt_backup_entries
}
```


### System Map

`system` 保存备份发起机器的详细快照信息。该信息不直接参与块级恢复，但对审计、资产识别、恢复前校验和裸机恢复判断很重要。

```text
system = {
  "host": host,
  "os": os,
  "cpu": cpu,
  "memory": memory,
  "network_interfaces": [],
  "firmware": firmware,
  "locale": locale,
  "collection_time_utc": collection_time_utc
}
```


#### Host Map

```text
host = {
  "hostname": hostname,
  "domain": domain,
  "workgroup": workgroup,
  "machine_guid": machine_guid,
  "user_name": user_name,
  "is_domain_joined": is_domain_joined
}
```


#### OS Map

```text
os = {
  "name": name,
  "version": version,
  "build_number": build_number,
  "architecture": architecture,
  "install_type": install_type,
  "product_type": product_type,
  "system_root": system_root
}
```


#### CPU Map

```text
cpu = {
  "vendor": vendor,
  "brand": brand,
  "architecture": architecture,
  "physical_package_count": physical_package_count,
  "physical_core_count": physical_core_count,
  "logical_processor_count": logical_processor_count,
  "max_clock_mhz": max_clock_mhz
}
```


#### Memory Map

```text
memory = {
  "total_physical_bytes": total_physical_bytes,
  "available_physical_bytes": available_physical_bytes,
  "page_size": page_size,
  "memory_module_count": memory_module_count
}
```


#### Network Interface Map

`network_interfaces[]` 中的每个元素描述一个网络接口。多网卡、多 IP 地址时都通过数组表达。

```text
network_interface = {
  "name": name,
  "description": description,
  "mac_address": mac_address,
  "ipv4_addresses": [],
  "ipv6_addresses": [],
  "gateway_addresses": [],
  "dns_servers": [],
  "dhcp_enabled": dhcp_enabled,
  "operational_status": operational_status,
  "interface_type": interface_type
}
```


#### Firmware Map

```text
firmware = {
  "firmware_type": firmware_type,              // 1=BIOS, 2=UEFI
  "secure_boot_enabled": secure_boot_enabled,
  "manufacturer": manufacturer,
  "version": version,
  "boot_disk_number": boot_disk_number
}
```


#### Locale Map

```text
locale = {
  "timezone": timezone,
  "locale_name": locale_name,
  "ui_language": ui_language
}
```


系统信息采集建议：

- `hostname`、`machine_guid`、`os`、`cpu.logical_processor_count`、`memory.total_physical_bytes`、`firmware.firmware_type` 建议作为基础必采字段。
- `network_interfaces[]` 建议采集所有启用的物理网卡和虚拟网卡；`mac_address` 可使用文本格式，例如 `00-11-22-33-44-55`。
- IP 地址、DNS、网关可能包含敏感网络信息。企业版应允许通过策略禁用或脱敏这些字段。
- `collection_time_utc` 建议使用 ISO 8601 UTC 字符串，例如 `2026-06-27T12:34:56Z`。

### Backup Job Map

`backup_job` 保存描述性的任务元数据。它不是恢复必需信息，但对 UI 展示和审计有帮助。

```text
backup_job = {
  "backup_type": backup_type,                  // 1=full, 2=incremental, 3=differential
  "created_utc": created_utc,
  "application_version": application_version,
  "description": description
}
```


## 多磁盘和整盘备份语义

一个备份文件可以包含多个磁盘：

```text
root.disks[0]
root.disks[1]
root.disks[2]
```

`ChunkHeader` 使用以下组合定位数据源：

```text
source_type + source_index
```

`disk_number` 在 `disks[]` 内必须唯一。`partition_number` 在同一个 disk 内必须唯一。`volume_index` 在 `volumes[]` 内必须唯一，并且每个有备份数据的 chunk 必须引用一个存在的 volume。

整盘备份由两部分表达：

1. CBOR 中完整的 disk 布局元数据，包括 `raw_layout`。
2. `volumes[]` 中由该 disk 展开的所有可备份 volume。
3. 每个被备份 volume 对应一个或多个 chunk。

示例：

```text
Disk 0 partitions:
  partition_number=1 EFI
  partition_number=2 MSR
  partition_number=3 C:
  partition_number=4 Recovery

Whole disk backup:
  volume_index=0 -> EFI/ESP volume if VSS-capable
  volume_index=1 -> C:
  volume_index=2 -> Recovery volume if VSS-capable

  Chunk 0 (source_type=volume, source_index=0)
  Chunk 1 (source_type=volume, source_index=1)
  Chunk 2 (source_type=volume, source_index=1)
  Chunk 3 (source_type=volume, source_index=2)
```

恢复整盘时：

1. 使用 CBOR disk 元数据和 `raw_layout` 重建分区表。
2. 顺序读取每个 chunk。
3. 根据 `ChunkHeader.source_type + source_index` 判断该 chunk 属于哪个 volume。
4. 根据 `volumes[].extents[]` 将 volume 逻辑地址映射回目标 disk/partition。
5. 根据本 chunk `BlockEntry[]` 逐块恢复目标 extent。

## VSS 备份语义

Windows 本地 volume/disk 备份必须使用 VSS。`backup volume` 直接对用户指定的 volume 创建 VSS snapshot；`backup disk` 先枚举该 disk 上所有可备份 volume，再对这些 volume 创建 VSS snapshot。整盘备份不得直接读取在线 `\\.\PhysicalDriveN` 数据作为普通数据源。

若用户指定的 disk 上存在跨盘卷，则必须把该 volume 的所有 extents 纳入本次备份，否则拒绝执行。MVP 可以只支持 basic volume 和单 extent；动态磁盘 simple/spanned 可通过多个 extents 表达，striped/mirrored/raid5 需要记录 stripe/mirror/parity 额外布局，未实现前应拒绝。

## 增量与差异备份

V6 通过备份链（backup chain）支持增量和差异备份。同一 Schedule / 备份策略下的成员共享同一个
`backup_set_uuid`。序列允许 Full → Inc → … → Full → Inc：后继全量仍使用同一 `backup_set_uuid`，
但每个全量的 `parent_uuid` 仍为全 0（自包含、可独立恢复），因此 set 内在 `parent_uuid` 图上是森林
（每棵子树以一次 Full 为根）。

### 备份链模型

- 全量备份：`flags & BACKUP_FLAG_FULL`，`parent_uuid` 为全 0；可与先前同 set 成员并存。
- 差异备份：`flags & BACKUP_FLAG_DIFFERENTIAL`，`parent_uuid` 指向基准全量的 `file_uuid`。差异只与基准比较，链深度恒为 2。
- 增量备份：`flags & BACKUP_FLAG_INCREMENTAL`，`parent_uuid` 指向上一次备份的 `file_uuid`。自动挂接时
  「上一次」= **当前树 tip**：当前树为同 `backup_set_uuid` 中最新 Full 为根的子树；tip 为该子树上
  不被引用为 parent 的叶子（可能是该 Full 本身，或其后的 Inc）。不得默认挂到旧 Full 分支。
  Service 在写 Archive 前用 Catalog 判定树是否完整（不打开 `.bkf`）：父点须有 sidecar、结构完整、
  卷几何匹配；且 `parent_uuid` 图上从 tip 上溯到 Full 无断链（`resolve_chain`）。不完整则本次
  **改写为 Full**（同 set 或新 set，`parent_uuid` 全 0），不写悬空父引用。链可任意深度。

链上每个文件本身都是结构完整的 V6 文件：拥有完整的 CBOR 元数据（磁盘/分区/卷布局）与独立的加密 envelope（独立 salt），但其 chunk 流只包含相对父备份发生变化的逻辑块。

### Hash Sidecar 文件（.bhx）

为了在下一次备份时快速找出变化块，全量和差异/增量备份在写完 `.bkf` 后都会生成一个同名 sidecar 文件 `<backup>.bkf.bhx`。Sidecar 记录每个卷每个逻辑块的状态和内容散列，描述**源在备份时刻的完整状态**，是后续增量/差异比较的基线。Sidecar 不是恢复必需文件，缺失只影响下一次增量计算，不影响恢复。

文件布局：

```text
SidecarFileHeader（固定 96 字节）
XChaCha20-Poly1305 ciphertext，解密并 zstd 解压后为：
  重复 volume_count 次：
    SidecarVolumeHeader { volume_index, total_blocks }
    total_blocks 条记录，每条 { uint8 state; uint8 hash[hash_size] }
                按逻辑块号升序密集排列。
```

```cpp
constexpr char     MAGIC_SIDECAR[8] = {'M','Y','B','K','H','I','D','X'}; // "MYBKHIDX"
constexpr uint16_t SIDECAR_VERSION  = 1;

constexpr uint8_t SIDECAR_HASH_SHA256 = 2;
constexpr uint8_t SIDECAR_COMPRESSION_ZSTD = 1;
constexpr uint8_t SIDECAR_ENCRYPTION_XCHACHA20_POLY1305 = 2;
constexpr uint16_t SIDECAR_FLAG_ENCRYPTED = 0x0001;

// 每块状态
constexpr uint8_t SIDECAR_STATE_DATA = 0; // 真实数据，hash[] 有效
constexpr uint8_t SIDECAR_STATE_ZERO = 1; // 全零块，hash[] 为 0
constexpr uint8_t SIDECAR_STATE_SKIP = 2; // 空闲/未使用块，hash[] 为 0

#pragma pack(push, 1)
struct SidecarFileHeader {
    char     magic[8];                  // "MYBKHIDX"
    uint16_t version;                   // SIDECAR_VERSION
    uint16_t flags;                     // SIDECAR_FLAG_ENCRYPTED
    uint32_t block_size;                // 逻辑块大小（字节）
    uint8_t  file_uuid[16];             // 所描述备份的 file_uuid
    uint8_t  hash_algo;                 // SIDECAR_HASH_SHA256
    uint8_t  hash_size;                 // 每个散列字节数
    uint8_t  compression;               // SIDECAR_COMPRESSION_ZSTD
    uint8_t  encryption;                // XChaCha20-Poly1305
    uint32_t volume_count;              // payload 中卷表数量
    uint64_t payload_uncompressed_size; // 解压后 payload 字节数
    uint64_t payload_stored_size;       // 压缩后 ciphertext 字节数
    uint8_t  nonce[24];                 // Sidecar 独立随机 nonce
    uint8_t  authentication_tag[16];    // detached AEAD tag
};
static_assert(sizeof(SidecarFileHeader) == 96, "SidecarFileHeader must be 96 bytes");

struct SidecarVolumeHeader {
    uint32_t volume_index;
    uint64_t total_blocks;
};
static_assert(sizeof(SidecarVolumeHeader) == 12, "SidecarVolumeHeader must be 12 bytes");
#pragma pack(pop)
```

V1 固定使用 SHA-256（`hash_size = 32`）。DATA record 保存内容散列；ZERO/SKIP record 的 hash 必须全零。payload 使用 zstd（level 3）压缩后整体加密，头部将 `authentication_tag` 清零后的 96 字节作为 AAD。Sidecar 密钥复用对应 `.bkf` envelope 的 Argon2id 参数和 salt，但通过 `MYBACKUP-V6-SIDECAR` HKDF context 派生独立 key；Sidecar 使用独立随机 nonce。文件通过“临时文件 + rename”发布。

### 差异/增量备份写入语义

创建增量层时：

1. 调用方显式提供直接父 Archive 及其凭据；目标路径不得与父 Archive 相同。
2. 打开并认证父 Archive 与其 sidecar（`<parent>.bkf.bhx`），校验 UUID、volume、逻辑大小、
   `block_size` 与记录数量。
3. 对每个 DATA 逻辑块计算 SHA-256（全量备份也始终计算以生成 sidecar）。
4. 与基线比较，判定该块是否变化：
   - DATA 块：散列不同或基线中不存在 → 视为变化。
   - ZERO/SKIP 块：仅当基线对应块为 DATA 时才视为变化（数据被清除）。
5. 只有变化块写入 chunk 流；未变化块不写入。
6. 关键规则：从 DATA 变为 zero/free 的块**必须显式写为 `BLOCK_FLAG_ZERO` 块**，不能省略。否则恢复时该位置会残留基准的旧数据。
7. 写完 `.bkf` 后生成一份**完整状态**的新 sidecar（包含所有块的最新状态，而不仅是变化块），作为后续备份的基线。

差异/增量备份的 `BackupHeader` 设置：`backup_set_uuid` 继承自基准，`parent_uuid =` 基准/父的 `file_uuid`，`flags` 设置对应的 DIFFERENTIAL/INCREMENTAL 位，CBOR `backup_job.backup_type` 设为 3/2。

### 链恢复

恢复差异/增量备份时需要应用整条链。Application 根据用户选择、备份目录或本地目录索引解析
`parent_uuid`，并为每一层取得对应凭据，最终向 Archive Adapter 提交显式 **base-first** 层列表。
Adapter 不扫描目录猜测父文件，也不对无关文件批量执行 KDF。Chain Reader 必须验证：第一层为全量、
后续层的 `parent_uuid` 精确指向前一层、`backup_set_uuid` 一致、UUID 不重复，并且 block size 与
有序 volume 几何一致——每一层的 `volumes[]` 在相同顺序下共享相同的 `volume_index`、`volume_id` 与
`total_size`（与写入端增量父匹配规则一致，不限制为单 Volume）。

恢复不把稀疏层逐个直接写入目标。`PersonalArchiveChainReader` 以基准全量层的连续 chunk 边界为
输出范围，在读取每个范围时按 base-first 顺序、按 `source_index` 与 per-volume `logical_offset`
应用所有相交覆盖，形成完整连续 `IRecoveryPointReader` 视图。通用 Restore Pipeline 对该合并视图
只执行一次，因此不会在中间层重复准备目标、重建布局或提前上线。单个全量 Archive 是长度为 1 的合法链；
单个增量层不得作为标准恢复源。

Adapter 不扫描目录，也不自动对候选文件执行 KDF。链发现、凭据选择和用户交互由 Application 或本地
Recovery Point 目录负责，最终必须提交显式、受深度限制的 base-first 层列表。

### 链挂载与随机读取

挂载/检视（mount / inspect）通过 Chain Reader 的分层随机读取实现。每层持有独立 Reader 和解密上下文；输出 descriptor 沿用基准全量层的连续 chunk 边界，读取时按 base-first 顺序应用与该范围相交的稀疏覆盖，后层覆盖前层。因此链视图可以直接交给普通 Restore Pipeline。缺层、顺序错误或父 UUID 不匹配时必须明确报错，不能返回不完整数据。

## 写入流程

```text
1. 解析用户输入；disk 备份展开为 volume 列表。
2. 收集磁盘、分区、volume、extent、原始分区表、系统和任务元数据。
3. 为每个被备份 volume 创建 VSS snapshot，并记录 VSS 状态。
4. 将元数据编码为 CBOR plaintext。
5. 写入 BackupHeader。
6. 派生 metadata_key，生成 metadata nonce，并用 AEAD 加密 CBOR plaintext。
7. 写入 CborMetadataEnvelopeHeader、可选 CborKeySlot[]、CBOR ciphertext 和 AEAD tag。
8. 从 VSS snapshot 的 volume 逻辑地址空间读取数据，并按 default_chunk_size 组织一个 chunk 的输入块。
9. 对该 chunk 内的块进行压缩和去重处理，生成明文临时 payload 和本 chunk 的 BlockEntry[]。
10. 生成独立 Payload nonce，以 tag 清零的 ChunkHeader 和 BlockEntry[] 为 AAD，加密临时 payload。
11. 把 detached tag 写入 ChunkHeader。
12. 写入 ChunkHeader。
13. 写入本 chunk 的 BlockEntry[]。
14. 写入本 chunk 的 Payload ciphertext。
15. 重复步骤 8-14，直到所有数据写完。
16. 计算 footer.file_size = currentOffset + sizeof(BackupFooter)。
17. 写入 BackupFooter。
18. 写完 `.bkf` 后生成完整状态的 sidecar 文件 `<backup>.bkf.bhx`（见“增量与差异备份”）。
```

`currentOffset` 应在每次成功写入后递增。`BlockEntry.ptr.payload_offset` 保存的是相对于当前 chunk payload 起始位置的偏移，不是文件绝对偏移。

启用分卷时，步骤 10 之前先判断完整 chunk 是否能放入当前卷；不能则关闭当前卷，创建下一个连续编号的续卷，写入续卷 Header 后再写整个 chunk。步骤 15 的 Footer 只写入最后一个分卷。所有输出先写入 partial 路径；发布顺序固定为 Sidecar、从后向前的续卷、首卷，首卷是整个 Archive 的可见性标记。任何步骤失败时，本次新建的所有分卷、Sidecar partial 和已经发布的组内文件必须清理。

写入 `BackupHeader` 时，`cbor_size` 必须使用加密 envelope 的总大小，而不是 CBOR plaintext 大小。`first_chunk_offset` 应在写入 CBOR envelope 后计算并回填，或由预先完成的 metadata 加密结果确定。

因为索引位于 payload 之前，所以写入一个 chunk 前必须先知道该 chunk 内每个 payload 的 `stored_size` 和 `payload_offset`。实现上可以采用临时 spool：

```text
1. 读取若干逻辑块。
2. 压缩后写入临时文件或内存缓冲。
3. 同时生成 BlockEntry[]。
4. 写 ChunkHeader + BlockEntry[]。
5. 认证加密完整 payload，并顺序写入 ciphertext。
```

## 读取流程

```text
1. 读取 BackupHeader，校验 magic、版本、header_size 和 CBOR 范围。
2. 读取 CborMetadataEnvelopeHeader、可选 CborKeySlot[]、CBOR ciphertext 和 AEAD tag。
3. 根据用户口令、密钥文件、KMS 或 key slot 得到 backup_master_key，并派生 metadata_key。
4. 使用 AEAD 校验并解密 CBOR ciphertext；认证失败时立即中止。
5. 解析解密后的 CBOR plaintext。
6. 从 first_chunk_offset 开始顺序读取 ChunkHeader。
7. 读取本 chunk 的 BlockEntry[]。
8. 根据 ChunkHeader.source_type + source_index 判断本 chunk 是否包含目标 volume 数据。
9. 顺序读取本 chunk 的 Payload ciphertext。
10. 派生 Payload key，以 ChunkHeader 和 BlockEntry[] 为 AAD 验证 tag 并解密；失败时立即中止。
11. 认证成功后解压目标块，并按 logical_block_index 写回目标分区位置。
12. 跳到下一个 chunk，重复步骤 6-11。
13. 读到 BackupFooter 后校验 footer magic 和 file_size。
```

顺序恢复的关键点是：读取 payload 前已经拿到了本 chunk 的索引，因此不需要 seek 到文件尾部读取全局索引，也不需要随机读取备份文件。

启用分卷时，读取器先验证首卷 Header 和 CBOR，再按连续编号打开续卷。一个非末卷到达文件尾时，从下一卷的 `first_chunk_offset` 继续；只有末卷允许以 `BackupFooter` 结束。所有卷共同构成一个逻辑 chunk 流，`chunk_index` 必须跨卷连续递增。

由于 chunk 的分区定位依赖解密后的 CBOR metadata，恢复器必须先成功解密 metadata，才能创建目标分区、校验 chunk 引用关系或执行整盘裸机恢复。如果只需要扫描文件完整性，可以在没有 metadata key 的情况下顺序校验 chunk 结构，但不能证明 chunk 引用的 disk/partition 合法。

对差异/增量备份，恢复前需先解析并认证完整备份链（见“增量与差异备份”），再由 Chain Reader 合并为
连续视图并执行一次上述恢复流程。挂载/随机读取复用同一个分层合并视图。

## 校验规则

读取器应拒绝或警告以下情况：

- Header 或 Footer magic 无效。
- `format_version`、`header_version`、`footer_version` 或 `cbor_schema_version` 不支持。
- `header_size != sizeof(BackupHeader)`。
- `footer_size != sizeof(BackupFooter)`。
- `footer.file_size` 与实际文件大小不一致。
- CBOR envelope 区域超出文件范围。
- 首卷或非分卷文件的 `first_chunk_offset` 不等于 `cbor_offset + cbor_size`。
- 非分卷文件设置了非零 `split_part_index`、`split_part_count` 或 `split_size_bytes`。
- 分卷文件的 `split_size_bytes == 0`，或者续卷的 `cbor_size != 0`、`first_chunk_offset != sizeof(BackupHeader)`。
- 分卷编号不连续、文件后缀与 `split_part_index` 不一致，或任一续卷的身份/格式字段与首卷不一致。
- 声明了非零 `split_part_count` 但实际卷数不匹配，或者 Footer 不在末卷。
- `CborMetadataEnvelopeHeader.magic != "MYBKCBR"`。
- 正式备份文件未设置 `CBOR_META_FLAG_ENCRYPTED`，或 `encryption_method == META_ENC_NONE`。
- `nonce_size` 或 `tag_size` 与 `encryption_method` 不匹配。
- KDF 参数版本不支持、Argon2id 参数为零，或参数超出产品允许的资源上限。
- `plaintext_size`、`ciphertext_size`、`key_slot_count` 推导出的 envelope 总大小与 `BackupHeader.cbor_size` 不一致。
- metadata AEAD tag 校验失败。
- `BackupHeader.encryption_method` 不是 XChaCha20-Poly1305。
- 解密后的 CBOR plaintext 长度不等于 `plaintext_size`。
- CBOR 任意 Map 使用了非 text string key，或根 Map 缺少字符串字段 `schema_version`。
- Chunk magic、chunk version 或 chunk header size 无效。
- Chunk nonce 全零，或保留字段、flags、`header_crc32` 非零。
- Chunk 中 header/index/payload 大小之和超出文件范围。
- `chunk_index` 在同一逻辑备份流中不连续，包括跨分卷边界时。
- `disks[]` 中存在重复的 disk number。
- 同一个 disk 内存在重复的 partition number。
- `ChunkHeader.source_type + source_index` 引用不存在的 volume。
- `logical_block_index * block_size >= volume.total_size`。
- `payload_offset + stored_size > 当前 chunk 的 payload_size`。
- Chunk Payload AEAD tag 校验失败；包括 ChunkHeader、BlockEntry 或 ciphertext 被篡改。
- 加密 payload 解密后有效数据不足 `logical_size`。
- `DEDUP ref_index` 不在当前 chunk 范围内，或引用了当前 entry 之后的 entry。
- GUID byte string 长度不是 16 字节。
