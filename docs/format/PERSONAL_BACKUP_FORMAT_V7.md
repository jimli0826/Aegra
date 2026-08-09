# 个人版备份文件格式 V7（`.bkf`）

> **范围变更：** [ADR-0020](../adr/0020-file-set-metadata-signature-incremental.md) 已接受 file_set Incremental
> 使用 metadata signature（`write_time + logical_size`）判断变化；[ADR-0018](../adr/0018-file-set-incremental-usn-and-chain.md)
> 的 USN baseline 已被替代。
> 并明确本期不支持 reparse、hard link、sparse 和 ADS。**FI0 已从 current V7 exact schema 删除**这些字段、
> 枚举与 platform reparse section；Reader 对含旧字段/枚举的开发 Archive 统一 corrupt/unsupported，不兼容、不迁移。
> 增量目标合同以 [增量设计](../architecture/FILE_SET_INCREMENTAL_BACKUP_RESTORE.md) §7 为准（FI1 起）。
>
> **[ADR-0019](../adr/0019-file-set-secondary-indexes-and-lazy-reader.md)：** file_set 增加 Entry ID / Stream /
> Chunk 二级索引；internal child 含物理 offset；普通 open 为 O(1) 级（不扫全树）；全量校验仅
> `verify_recoverability`。产品未发布，不兼容仅含 Namespace 树的开发期 Archive。

| 属性 | 内容 |
| --- | --- |
| 状态 | 权威格式规范 |
| 格式版本 | 7 |
| CBOR Schema | 1（volume_set 与 file_set 共用 schema 编号，根 Map 由 `content_kind` 分支） |
| 决策依据 | [ADR-0016](../adr/0016-file-set-backup-and-restore-boundary.md) |
| 取代 | 开发期 V6 规范；生产只实现 V7，拒绝 `format_version != 7` |

> 本规范是新项目个人版 `.bkf` 的唯一格式依据。产品尚未发布，不读取 V6 或其它试验格式，也不实现迁移、
> dual-read、alias 或 fallback。Parser 对版本不匹配统一返回 unsupported/corrupt，不得解析旧 Header 字段语义。

## 1. 概述

V7 在固定二进制启动结构、加密 CBOR 元数据、分卷与 AEAD 模型上延续 V6 的工程原则，并增加：

- Header 明文 `content_kind`：`volume_set` 或 `file_set`；
- 统一 **Archive Record** 前缀，使 volume chunk、file stream chunk、file index page 与 footer 可顺序扫描；
- `file_set` 的分页 File Index（四棵 B+tree：Namespace / Entry ID / Stream / Chunk）与 stream 级数据 chunk；
- Footer 中各索引 root 的跨分卷定位与根摘要；internal child 携带物理 offset。

`volume_set` 的 Volume 备份/增量/Sidecar 行为必须与既有产品功能等价，仅版本号与 record 包装升级到 V7。

### 1.1 文件布局

```text
+----------------------------+
| BackupHeader (256 B)       |
+----------------------------+
| Encrypted CBOR Metadata    |  仅首卷；续卷 cbor_size=0
+----------------------------+
| Archive Record 0           |  volume chunk | file stream chunk | index page
| Archive Record 1           |
| ...                        |
+----------------------------+
| Archive Record Footer      |  仅末卷
+----------------------------+
```

设计规则：

- 所有多字节整数为 **little-endian**。
- 分卷只在完整 record 边界切换。
- 非分卷文件同时是首卷和末卷。
- 首卷含且仅含一份 CBOR metadata；Footer 只在末卷。
- `file_set`：全部 file stream data chunk 写完后写 index page record，最后写 Footer。
- `volume_set`：不写 file index page；chunk 流结束后写 Footer。

### 1.2 content_kind

```cpp
constexpr uint8_t CONTENT_KIND_VOLUME_SET = 1;
constexpr uint8_t CONTENT_KIND_FILE_SET   = 2;
```

- Header、Catalog、Job 与 Service 摘要必须一致。
- `CONTENT_KIND_VOLUME_SET` 禁止出现 `source_type=file_stream` 或 index page record。
- `CONTENT_KIND_FILE_SET` 禁止出现 `source_type=volume`；禁止 `.bhx` Sidecar；禁止 DIFFERENTIAL。
  FULL：`parent_uuid` 全 0。INCREMENTAL：`parent_uuid` 非 0 且必须置位 `CAP_FILE_METADATA_BASELINE`。

## 2. BackupHeader

固定 256 字节，写在每个分卷开头。

```cpp
#pragma pack(push, 1)
struct BackupHeader {
    char     magic[8];              // "MYBACKUP"
    uint16_t header_version;        // 2
    uint16_t format_version;        // 7
    uint32_t header_size;           // 256

    uint8_t  file_uuid[16];
    uint8_t  backup_set_uuid[16];
    uint8_t  parent_uuid[16];       // full = all zero

    uint32_t block_size;            // volume_set: logical block size; file_set: stream chunk quantum
    uint32_t flags;                 // FULL/INCREMENTAL/DIFFERENTIAL/DEDUP/ENCRYPTED/SPLIT

    uint64_t cbor_offset;           // offset of CborMetadataEnvelopeHeader
    uint64_t cbor_size;             // envelope total bytes; continuation parts = 0
    uint32_t cbor_schema_version;   // 1

    uint64_t first_record_offset;   // first ArchiveRecord after metadata (or after header on cont. parts)
    uint32_t default_chunk_size;    // writer target; default 512 MiB

    uint8_t  compression_method;    // default compression for payloads
    uint8_t  encryption_method;     // PAYLOAD_ENC_*
    uint8_t  content_kind;          // CONTENT_KIND_*
    uint8_t  reserved_align0;       // must be 0

    uint32_t split_part_index;      // 0-based
    uint32_t split_part_count;      // writer may leave 0; reader discovers parts
    uint64_t split_size_bytes;      // target cap; 0 if not split

    uint32_t capability_flags;      // see below
    uint8_t  reserved[128];         // must be zero
};
static_assert(sizeof(BackupHeader) == 256);
#pragma pack(pop)
```

字段偏移（便于人工核对；合计 256）：

| 偏移 | 大小 | 字段 |
| ---: | ---: | --- |
| 0 | 8 | magic |
| 8 | 2 | header_version |
| 10 | 2 | format_version |
| 12 | 4 | header_size |
| 16 | 16 | file_uuid |
| 32 | 16 | backup_set_uuid |
| 48 | 16 | parent_uuid |
| 64 | 4 | block_size |
| 68 | 4 | flags |
| 72 | 8 | cbor_offset |
| 80 | 8 | cbor_size |
| 88 | 4 | cbor_schema_version |
| 92 | 8 | first_record_offset |
| 100 | 4 | default_chunk_size |
| 104 | 1 | compression_method |
| 105 | 1 | encryption_method |
| 106 | 1 | content_kind |
| 107 | 1 | reserved_align0 |
| 108 | 4 | split_part_index |
| 112 | 4 | split_part_count |
| 116 | 8 | split_size_bytes |
| 124 | 4 | capability_flags |
| 128 | 128 | reserved |
| 256 | — | end |

### 2.1 flags

```cpp
constexpr uint32_t BACKUP_FLAG_FULL         = 0x00000001;
constexpr uint32_t BACKUP_FLAG_INCREMENTAL  = 0x00000002;
constexpr uint32_t BACKUP_FLAG_DIFFERENTIAL = 0x00000004;
constexpr uint32_t BACKUP_FLAG_DEDUP        = 0x00000008;
constexpr uint32_t BACKUP_FLAG_ENCRYPTED    = 0x00000010;
constexpr uint32_t BACKUP_FLAG_SPLIT        = 0x00000020;
```

正式产品文件必须设置 `BACKUP_FLAG_ENCRYPTED`。FULL / INCREMENTAL / DIFFERENTIAL 互斥且恰有一个。
`BACKUP_FLAG_DEDUP` 表示 volume_set Writer 启用了 [ADR-0022](../adr/0022-volume-set-chunk-local-deduplication.md)
策略；即使没有产生 DEDUP entry 也保持置位。file_set 禁止置位。

### 2.2 capability_flags

```cpp
constexpr uint32_t CAP_HAS_FILE_INDEX     = 0x00000001; // file_set 必须置位
constexpr uint32_t CAP_VOLUME_SIDECAR_OK  = 0x00000002; // volume_set 增量链可用 sidecar
constexpr uint32_t CAP_FILE_METADATA_BASELINE = 0x00000004; // file_set Incremental 必须置位
```

未知 bit 必须为 0；Reader 对未知非零 bit 拒绝（critical）。

### 2.3 算法编号

```cpp
constexpr uint8_t PAYLOAD_ENC_NONE               = 0; // 产品无效
constexpr uint8_t PAYLOAD_ENC_XCHACHA20_POLY1305 = 2;

constexpr uint8_t COMPRESSION_NONE      = 0;
constexpr uint8_t COMPRESSION_ZSTD      = 1;
```

### 2.4 Header 校验（拒绝规则）

在读取任何后续 record 前必须失败的条件：

- `magic != "MYBACKUP"`；
- `header_version != 2` 或 `format_version != 7` 或 `header_size != 256`；
- `reserved_align0 != 0` 或 `reserved` 任一字节非 0；
- `content_kind` 不是 1 或 2；
- `encryption_method != PAYLOAD_ENC_XCHACHA20_POLY1305` 或未设置 ENCRYPTED；
- `block_size == 0` 或 `block_size > 64 MiB` 或非 512 的倍数（volume_set）；file_set 要求
  `4096 <= block_size <= 64 MiB` 且为 4096 的倍数；
- 首卷：`cbor_offset == 256` 且 `cbor_size > 0` 且 `cbor_size <= 64 MiB + envelope overhead`，
  `first_record_offset == cbor_offset + cbor_size`；
- 续卷：`cbor_size == 0` 且 `first_record_offset == 256`；
- `split_part_index` 与文件名后缀不一致；
- FULL 时 `parent_uuid` 必须全 0；INCREMENTAL/DIFFERENTIAL 时不得全 0；
- file_set：`CAP_HAS_FILE_INDEX` 置位、不得置 `CAP_VOLUME_SIDECAR_OK`、禁止 DIFFERENTIAL；
  FULL 时 `parent_uuid` 全 0；INCREMENTAL 时 `parent_uuid` 非 0 且 `CAP_FILE_METADATA_BASELINE` 置位；
  不得设置 `BACKUP_FLAG_DEDUP`。

Header 不保存文件名、路径、entry 数、index 位置或未加密客户 metadata。

## 3. 分卷

路径命名：

```text
part 0: <name>.bkf
part 1: <name>.bkf.001
part 2: <name>.bkf.002
...
```

| 字段/区域 | 首卷 | 续卷 |
| --- | --- | --- |
| `split_part_index` | 0 | 与后缀一致 |
| CBOR | 有且仅一份 | 无 |
| Footer | 仅当本卷为末卷 | 仅当本卷为末卷 |
| record 内容 | metadata 后任意完整 record | 任意完整 record |

- 最大分卷数：产品上限 **1000**（含首卷）。
- Writer 可将 `split_part_count` 写 0；Reader 从 `.001` 连续发现直到含 Footer 的末卷。
- 序号不连续、UUID/算法/content_kind 不一致、Footer 后仍有续卷 → corrupt。

Index page record 可位于任一 part，但必须在该 Archive 全部 file stream data chunk 之后。Footer 仅末卷，
并以 `(part_index, absolute_offset)` 定位 root page。

发布顺序：Sidecar（volume 增量）→ 续卷从后向前 → 首卷最后。

## 4. Archive Record

每个 record 以固定 32 字节前缀开始，随后为 kind 专属 body。Record 不可跨分卷。

```cpp
#pragma pack(push, 1)
struct ArchiveRecordPrefix {
    char     magic[8];          // "MYBKREC\0"  (7 chars + NUL) — see note
    uint16_t prefix_version;    // 1
    uint16_t record_kind;       // RECORD_KIND_*
    uint32_t header_size;       // sizeof(prefix) + kind-specific fixed header
    uint64_t body_size;         // bytes after fixed header through end of record
    uint32_t flags;             // record flags; 0 if unused
    uint32_t reserved;          // must be 0
};
static_assert(sizeof(ArchiveRecordPrefix) == 32);
#pragma pack(pop)
```

> 实现常量：magic 8 字节为 `{'M','Y','B','K','R','E','C',0}`。

```cpp
constexpr uint16_t RECORD_KIND_VOLUME_CHUNK      = 1;
constexpr uint16_t RECORD_KIND_FILE_STREAM_CHUNK = 2;
constexpr uint16_t RECORD_KIND_FILE_INDEX_PAGE   = 3;
constexpr uint16_t RECORD_KIND_FOOTER            = 4;
```

校验：

- `prefix_version == 1`；
- `record_kind` 已知且与 `content_kind` 兼容；
- `header_size >= 32` 且 `header_size + body_size` 不溢出且不越过本 part EOF；
- 未知 `record_kind` 一律 critical 拒绝（不得跳过）；
- 解密/解压/向调用方返回 payload 前必须完成该 record 的完整性认证。

### 4.1 Volume Chunk（kind=1）

固定头在 prefix 之后共 96 字节（与历史 ChunkHeader 字段对齐，便于 volume 逻辑迁移）：

Kind-specific 固定头 96 字节；`ArchiveRecordPrefix.header_size = 128`（32+96）。

```cpp
#pragma pack(push, 1)
struct VolumeChunkHeader {
    char     chunk_magic[8];        // "MYBKCHK\0"
    uint32_t chunk_header_size;     // 96
    uint64_t chunk_index;           // 0-based among volume chunks
    uint8_t  source_type;           // 1 = volume
    uint8_t  reserved_source[3];    // 0
    uint32_t source_index;          // volumes[].volume_index
    uint32_t block_entry_count;
    uint32_t reserved0;
    uint64_t payload_size;          // ciphertext bytes
    uint32_t chunk_flags;
    uint32_t header_crc32;          // 0 if unused
    uint8_t  payload_nonce[24];
    uint8_t  payload_authentication_tag[16];
    uint8_t  reserved1[4];
};
static_assert(sizeof(VolumeChunkHeader) == 96);
#pragma pack(pop)
```

Body：`BlockEntry[block_entry_count]` + payload ciphertext。

`BlockEntry`（25 字节，同既有语义）：

```cpp
#pragma pack(push, 1)
struct BlockEntry {
    uint64_t logical_block_index;
    union {
        uint64_t payload_offset;
        uint64_t ref_index;
    } ptr;
    uint32_t stored_size;
    uint32_t logical_size;
    uint8_t  flags; // RAW=0x01 COMPRESSED=0x02 ZERO=0x04 DEDUP=0x08 FREE=0x10
};
static_assert(sizeof(BlockEntry) == 25);
#pragma pack(pop)
```

Volume BlockEntry 规则：

- RAW/COMPRESSED 表示已分配数据块，`logical_size` 是展开字节数；真实全零内容仍编码为 ZERO；
- ZERO/FREE 都不占 payload，`ptr.payload_offset=0`，`stored_size=0`，`logical_size` 是连续逻辑块数；
- FREE 只表示文件系统空闲簇或经用户选项排除的 `pagefile.sys`、`hiberfil.sys`、`swapfile.sys`
  extent。备份端不得读取这些区间，恢复端不得向这些区间写盘；
- FREE 与 ZERO 是不同状态。恢复 ZERO 必须写零，恢复 FREE 必须跳过；Sidecar 对每个逻辑块分别记录
  DATA/ZERO/FREE，非 DATA 状态 hash 全零；
- FREE 区间必须按 block 边界编码（卷末最后一个逻辑块可短于 block size），相邻 FREE 块应合并为 run。
  同一 Archive block 内只要包含任意已用 cluster，该 block 整体按 DATA/ZERO 读取和编码，不允许部分标 FREE。

#### 4.1.1 Volume DEDUP 语义

DEDUP 仅用于 volume_set，引用域严格限制为当前物理 `VolumeChunk` record：

- `flags` 必须恰为 DEDUP；`ptr.ref_index` 是当前 `BlockEntry[]` 的零基索引；
- `ref_index < current_entry_index`，且目标必须恰为 RAW 或 COMPRESSED；禁止引用 ZERO、FREE 或 DEDUP；
- `stored_size == 0`、`logical_size == 0`，entry 不占 payload；
- DEDUP 表示一个逻辑块，不得编码 run；目标 canonical 解码长度必须等于该目标位置允许的块长度；
- 禁止跨 Chunk、part、source、Archive 或 Recovery Point 引用；
- canonical 是当前 Chunk 中按逻辑块序遇到的首个相同 DATA 块。Writer 用
  `(SHA-256(plaintext), logical_size)` 查找候选，命中后必须逐字节确认；
- FREE 分类早于读取、ZERO 检测与 DEDUP；ZERO 检测早于 DEDUP。Incremental 父层未变化块先被省略，
  不能成为本层 canonical；
- DEDUP entry 存在而 Header 未置 `BACKUP_FLAG_DEDUP`，或 file_set 出现 DEDUP，均为 corrupt。

BlockEntry 表是 AEAD AAD 而非密文，DEDUP flag/ref 会暴露当前 Chunk 内的块相等关系。不得持久化候选哈希，
不得使用 convergent encryption。完整算法与安全边界见
[Volume Set 去重设计](../architecture/VOLUME_SET_DEDUPLICATION.md)。

Volume chunk AEAD AAD（顺序拼接）：

1. 完整 256 字节 `BackupHeader`（本 part）；
2. 完整 32 字节 `ArchiveRecordPrefix`（本 record）；
3. kind-specific 96 字节头，其中 `payload_authentication_tag` 清零；
4. 编码后的全部 `BlockEntry[]`。

HKDF info：`MYBACKUP-V7-CHUNK-PAYLOAD`（见 §7）。

### 4.2 File Stream Chunk（kind=2）

```cpp
#pragma pack(push, 1)
struct FileStreamChunkHeader {
    uint64_t chunk_index;           // 0-based among file stream chunks only
    uint8_t  source_type;           // 2 = file_stream
    uint8_t  reserved_source[3];    // 0
    uint32_t source_index;          // File Index stream_index
    uint32_t block_entry_count;
    uint32_t reserved0;
    uint64_t payload_size;
    uint32_t chunk_flags;
    uint32_t reserved1;
    uint8_t  payload_nonce[24];
    uint8_t  payload_authentication_tag[16];
};
// sizeof = 80; ArchiveRecordPrefix.header_size = 32+80 = 112
static_assert(sizeof(FileStreamChunkHeader) == 80);
#pragma pack(pop)
```

```cpp
constexpr uint8_t SOURCE_TYPE_VOLUME      = 1;
constexpr uint8_t SOURCE_TYPE_FILE_STREAM = 2;
```

Body：`BlockEntry[block_entry_count]` + payload。

语义：

- `logical_block_index` 为相对该 stream 的 block 序号：`offset = logical_block_index * block_size`；
- 最后一块 `logical_size` 可小于 `block_size`；
- sparse hole **不**写入 BlockEntry；恢复时未覆盖区间保持 hole（目标需支持 sparse）；
- 空 stream（0 字节）不产生 chunk；Index 中 `extent_count=0`。

File stream chunk AEAD AAD：

1. 本 part `BackupHeader` 256 B；
2. `ArchiveRecordPrefix` 32 B；
3. `FileStreamChunkHeader` 80 B（tag 清零）；
4. `BlockEntry[]`。

上限：`block_entry_count <= 1_048_576`；`payload_size <= 512 MiB`；单 entry `stored_size/logical_size`
均 `<= block_size` 且 `<= 64 MiB`。

### 4.3 File Index Page（kind=3）

```cpp
#pragma pack(push, 1)
struct FileIndexPageHeader {
    char     page_magic[8];         // "MYBKIDX\0"
    uint16_t page_format_version;   // 1
    uint16_t page_kind;             // 1=leaf 2=internal
    uint64_t page_id;               // unique in archive, non-zero
    uint32_t plain_size;            // plaintext page body bytes
    uint32_t encoded_size;          // ciphertext or plaintext storage bytes
    uint8_t  protection_mode;       // 1=AEAD 2=digest_only (product uses 1)
    uint8_t  reserved0[3];
    uint8_t  nonce[24];             // AEAD nonce; zero if unused
    uint8_t  authentication_tag[16];
    uint8_t  content_digest[32];    // SHA-256 of plaintext body after auth
    uint32_t entry_count;           // keys in this page
    uint32_t reserved1;
};
// sizeof = 112; ArchiveRecordPrefix.header_size = 32+112 = 144
static_assert(sizeof(FileIndexPageHeader) == 112);
#pragma pack(pop)
```

```cpp
constexpr uint16_t INDEX_PAGE_LEAF     = 1;
constexpr uint16_t INDEX_PAGE_INTERNAL = 2;
constexpr uint8_t  INDEX_PROTECT_AEAD  = 1;
```

Body：`encoded_size` 字节密文（产品强制 AEAD）。解密后 plaintext 长度必须等于 `plain_size`。

校验顺序：

1. prefix + page header 长度与上限（`plain_size <= 1 MiB`，`encoded_size == plain_size` for detached XChaCha）；
2. AEAD 认证成功；
3. `SHA-256(plaintext) == content_digest`；
4. 解析 CBOR/紧凑结构，检查排序、引用与深度。

Index page AEAD AAD：

1. 本 part `BackupHeader` 256 B；
2. `ArchiveRecordPrefix` 32 B；
3. `FileIndexPageHeader` 中 `authentication_tag` 与 `content_digest` 清零后的 112 B。

HKDF info：`MYBACKUP-V7-FILE-INDEX-PAGE`。

## 5. File Index 模型

### 5.1 名称编码

```cpp
constexpr uint8_t NAME_ENCODING_WINDOWS_UTF16LE = 1;
```

| 规则 | 值 |
| --- | --- |
| encoding | 仅允许 `1` |
| name bytes | 偶数长度，2–512 |
| 禁止 | NUL code unit（U+0000）、编码层 path separator 注入（见组件规范化） |
| 比较 | 无符号字节序；不 locale |

组件在 Service/Worker 规范化后不得包含 `\` `/` `.` `..` 或空组件；Archive 内保存的是**已规范化**相对组件。

### 5.2 B+tree key

每个 leaf/internal 排序键：

```text
key = (
  parent_entry_id : u64,     // 0 = forest root sentinel parent for selection roots' parent link
  name_encoding   : u8,      // must be 1
  name_bytes      : bstr,    // UTF-16LE
  entry_id        : u64      // unique non-zero
)
```

排序：`parent_entry_id` ASC → `name_encoding` ASC → `name_bytes` 字节 ASC → `entry_id` ASC。

同一 `(parent_entry_id, name_bytes)` 不得对应多个 `entry_id`。Reader 拒绝重复 key。

### 5.3 Entry ID 与图

| 项 | 规则 |
| --- | --- |
| `entry_id` | `uint64`，从 1 起由 Writer 确定性分配；0 保留 |
| root | 逻辑根不写入 leaf；`parent_entry_id = 0` 的 entry 为 selection root 的父级子项（即 selection roots） |
| parent | 每个非根 entry 恰有一个 parent；parent 必须存在且为 directory（或 selection 允许的容器） |
| 可达 | 从 parent=0 的 selection roots 出发，全部 entry 可达；禁止环 |
| 最大深度 | 产品上限 64（从 selection root 起算） |
| 最大 entry | 产品上限 10_000_000 |

### 5.4 Leaf value（CBOR map，text keys）

每个 leaf 记录 value 为 CBOR map（确定性 canonical：key 按字节序排序，无重复）：

```text
{
  "entry_id": u64,
  "parent_entry_id": u64,
  "kind": u8,                    // 1=dir 2=file only
  "name_encoding": u8,           // 1
  "name": bstr,                  // UTF-16LE
  "selection_id": bstr,          // 16-byte UUID; only selection roots; else empty bstr length 0
  "stable_file_identity": {      // NTFS/ReFS identity; FAT32 uses null representation
    "volume_identity": bstr,     // canonical volume identity, or empty for null
    "file_id": bstr              // 16-byte FILE_ID_128, or 16 zero bytes for null
  },
  "attributes": u32,             // portable Windows file attributes subset; no reparse/sparse bits
  "flags": u32,                  // ENTRY_FLAG_* (known mask only)
  "creation_time": u64,          // Windows FILETIME UTC 100ns
  "access_time": u64,
  "write_time": u64,
  "change_time": u64,
  "logical_size": u64,           // main stream logical size; dirs 0
  "stream_count": u32,
  "streams": [ StreamDesc... ],
  "platform": bstr               // bounded opaque envelope; see §5.7
}
```

```cpp
constexpr uint8_t ENTRY_KIND_DIRECTORY = 1;
constexpr uint8_t ENTRY_KIND_FILE      = 2;

constexpr uint32_t ENTRY_FLAG_HAS_SECURITY   = 0x0001;
constexpr uint32_t ENTRY_FLAG_CASE_SENSITIVE = 0x0004;
// FI0: ENTRY_FLAG_SPARSE_MAIN and kinds reparse/other are removed from current format.
```

Reader 拒绝：`kind∉{1,2}`、存在 `hard_link_group`、未知 flag 位、attributes 含 sparse/reparse 位。

### 5.5 StreamDesc

```text
{
  "stream_index": u32,           // unique archive-wide among streams; 0 reserved = none
  "stream_kind": u8,             // 1=main only
  "name_encoding": u8,           // 1
  "name": bstr,                  // main: empty only
  "logical_size": u64,
  "content_storage": u8,         // 1=local 2=parent (Incremental only)
  "parent_stream_index": u32,    // parent storage: direct parent stream_index; local must be 0
  "extent_count": u32,
  "extents": [ ExtentDesc... ]
}
```

```text
ExtentDesc = {
  "chunk_index": u64,            // file stream chunk_index
  "block_entry_index": u32,      // index within that chunk's BlockEntry[]
  "file_offset": u64,            // logical offset in stream
  "logical_size": u64
}
```

规则：

- `stream_index` 全局唯一且非 0；目录 `stream_count=0` 且无 streams；
- 普通文件恰有一个 `stream_kind=main`，`name` 为空；禁止 ADS / `allocated_ranges` 字段；
- `content_storage=local`：extents 从 offset 0 起严格递增、密铺、不重叠，覆盖全部 `logical_size`；
  `parent_stream_index` 必须为 0；
- `content_storage=parent`：仅 Incremental；`extents` 必须为空且 `extent_count=0`；
  `parent_stream_index` 指向直接父层 Index 中同 identity 的 main stream；禁止任意祖先；
- 空文件（local）：`logical_size=0`，`extent_count=0`，无 chunk；
- 单 stream extent 数 ≤ 1_048_576；单 stream `logical_size` ≤ 16 TiB。

### 5.6 Internal page plaintext（Namespace）

```text
{
  "page_kind": 2,
  "keys": [ KeyDesc... ],        // separator keys, sorted
  "children": [ ChildLocator... ] // length = keys+1
}

ChildLocator = {
  "page_id": u64,                // non-zero
  "offset":  u64                 // absolute file offset of child's ArchiveRecordPrefix
}
```

`KeyDesc` 与 leaf key 同构。最大 page 深度 8（root depth 0）。`page_id` 不得形成环；所有 leaf 可达。
`offset` 必须指向该 Archive 内已写出的 index page record；Reader 不得依赖全局 page_id→offset 扫描表。

Writer 必须自底向上构建多层 internal：当 leaf 数超过单层 internal 的 fanout（keys≤256 → 最多 257
children）时提升 depth，直到单 root；在 depth 仍 ≤8 时不得因“仅实现两层”失败。单 leaf 时 root 即为该
leaf。每页 plaintext 仍受 1 MiB 限制，因此大 entry/大 separator key 会降低实际 fanout。

### 5.6.1 二级索引（ADR-0019）

除 Namespace 树外，Writer 必须写出最多三棵二级 B+tree（与 Namespace 共享 `page_id` 分配空间与
AEAD 页格式）。`FileIndexPageHeader.page_kind` / CBOR `page_kind`：

| page_kind | 树 | 角色 |
| --- | --- | --- |
| 1 | Namespace | leaf（完整 `FileEntryDesc`，§5.4） |
| 2 | Namespace | internal（§5.6） |
| 3 | Entry ID | leaf |
| 4 | Entry ID | internal |
| 5 | Stream | leaf |
| 6 | Stream | internal |
| 7 | Chunk | leaf |
| 8 | Chunk | internal |

**Entry ID leaf（page_kind=3）**

```text
{
  "page_kind": 3,
  "records": [ {
    "entry_id": u64,
    "page_id": u64,              // Namespace leaf page holding full entry
    "page_offset": u64,          // absolute ArchiveRecordPrefix of that Namespace leaf
    "slot": u32,                 // 0-based index within that leaf
    "parent_entry_id": u64,
    "kind": u8                   // 1=dir 2=file
  }, ... ]                       // sorted by entry_id ASC; unique; count 1..256
}
```

**Stream leaf（page_kind=5）**

```text
{
  "page_kind": 5,
  "records": [ {
    "stream_index": u32,
    "entry_id": u64,
    "stream_slot": u32           // index in entry.streams
  }, ... ]                       // sorted by stream_index ASC; unique; count 1..256
}
```

**Chunk leaf（page_kind=7）**

```text
{
  "page_kind": 7,
  "records": [ {
    "chunk_index": u64,
    "record_offset": u64,        // ArchiveRecordPrefix of the file stream chunk
    "payload_offset": u64,       // absolute offset of chunk payload bytes
    "payload_size": u64,
    "block_entry_count": u32
  }, ... ]                       // sorted by chunk_index ASC; unique; count 1..256
}
```

**二级 internal（page_kind ∈ {4,6,8}）**

```text
{
  "page_kind": 4|6|8,
  "keys": [ u64... ],            // separator keys (Stream 树将 stream_index 零扩展为 u64)
  "children": [ ChildLocator... ] // length = keys+1
}
```

二级 internal 的 keys 为无符号整数升序；fanout/深度/1 MiB 限制与 Namespace 相同。

写出顺序（file_set）：全部 local file stream chunk → Namespace pages → Entry ID pages →
Stream pages（若 stream_count>0）→ Chunk pages（若 file_stream_chunk_count>0）→ Footer。

### 5.7 platform metadata envelope

`platform` bstr 最大 **64 KiB**。V1 envelope：

```text
struct PlatformEnvelope {
  u16 envelope_version;  // 1
  u16 reserved;          // 0
  u32 flags;
  // followed by tagged sections
}
```

FI0 仅允许 security section（若 `ENTRY_FLAG_HAS_SECURITY`）：

| tag | 内容 |
| ---: | --- |
| 1 | self-relative SECURITY_DESCRIPTOR bytes（Owner/Group/DACL/SACL） |

历史 reparse section（tag 2）及任何其它 tag（含 non-critical）一律拒绝。Writer 只写 tag 1。

### 5.8 Index root digest

每棵树独立计算 root digest（Footer 各存 32 字节）：

```text
// Namespace（字段名 index_root_digest）
SHA-256(
    "MYBACKUP-V7-INDEX-ROOT" ||
    le64(root_page_id) || le64(0) ||
    le64(entry_count) || le64(stream_count) ||
    root_page.content_digest
)

// Entry ID
SHA-256(
    "MYBACKUP-V7-ENTRY-ID-INDEX-ROOT" ||
    le64(root_page_id) || le64(0) ||
    le64(entry_count) || le64(0) ||
    root_page.content_digest
)

// Stream（stream_count==0 时 root 全 0，不计算）
SHA-256(
    "MYBACKUP-V7-STREAM-INDEX-ROOT" ||
    le64(root_page_id) || le64(0) ||
    le64(stream_count) || le64(0) ||
    root_page.content_digest
)

// Chunk（file_stream_chunk_count==0 时 root 全 0，不计算）
SHA-256(
    "MYBACKUP-V7-CHUNK-INDEX-ROOT" ||
    le64(root_page_id) || le64(0) ||
    le64(file_stream_chunk_count) || le64(0) ||
    root_page.content_digest
)
```

`index_page_count`（Footer）= 四棵树 page 数之和。预映像中树级 `page_count` 固定写 0（Footer 不存分树页数，避免 Reader 无法复算）。

### 5.9 Reader 打开语义（ADR-0019）

| 操作 | 成本 |
| --- | --- |
| 普通打开 Archive | O(1)：Header + Footer + 校验非零 root page |
| 列目录 | O(log N + K) |
| 按 Entry ID 查询 | O(log N) |
| 解析 Stream | O(log S) |
| 定位 Chunk | O(log C) |
| Reader 常驻内存 | O(page cache size)（有界 LRU，单页 ≤ 1 MiB） |
| 完整 Verify | O(N + S + C) |

普通 browse/选择性恢复只认证访问路径；完整重复 ID / 父图 / 全部 payload 由
`verify_recoverability` 完成。

## 6. Footer（kind=4）

固定 body，`ArchiveRecordPrefix.header_size = 32`，`body_size = 480`，总 512 字节（与历史 Footer 对齐）。

整个 Footer record 固定 512 字节：`ArchiveRecordPrefix` 32 + body 480。
`prefix.record_kind = RECORD_KIND_FOOTER`，`prefix.header_size = 32`，`prefix.body_size = 480`。

```cpp
#pragma pack(push, 1)
struct IndexRootLocator {
    uint64_t page_id;               // 0 = absent
    uint64_t offset;                // ArchiveRecordPrefix absolute offset; 0 if absent
    uint8_t  digest[32];            // root digest (§5.8); zero if absent
};

struct BackupFooterBody {
    char     magic[8];              // "MYBKEND\0"
    uint16_t footer_version;        // 2
    uint16_t format_version;        // 7
    uint32_t footer_size;           // 512 (entire record)

    uint64_t volume_chunk_count;
    uint64_t file_stream_chunk_count;
    uint64_t index_page_count;      // all index trees combined
    uint64_t total_block_entry_count;
    uint64_t total_payload_size;
    uint64_t logical_bytes;
    uint64_t stored_bytes;          // sum of all part file sizes after commit
    uint64_t entry_count;           // file_set; 0 for volume_set
    uint64_t stream_count;          // file_set; 0 for volume_set

    uint32_t index_root_part_index; // Namespace root part; file_set; else 0
    uint32_t reserved0;
    uint64_t index_root_offset;     // Namespace root (alias of namespace_root)
    uint64_t index_root_page_id;
    uint8_t  index_root_digest[32];

    uint64_t part_file_size;        // this part size including footer record
    uint8_t  file_uuid[16];         // must match header

    // body offset 168: secondary roots (ADR-0019), then volume dedup metrics
    IndexRootLocator entry_id_root; // 48 B
    IndexRootLocator stream_root;   // 48 B
    IndexRootLocator chunk_root;    // 48 B
    uint64_t deduplicated_block_count;   // volume_set DEDUP entries; file_set 0
    uint64_t deduplicated_logical_bytes; // expanded bytes represented by DEDUP; file_set 0
    uint8_t  reserved[152];         // zero; body total 480
};
static_assert(sizeof(BackupFooterBody) == 480);
#pragma pack(pop)
```

Footer 无独立 AEAD；完整性依赖：

- 末卷存在且 `part_file_size` 匹配；
- 所有 chunk/page 各自 AEAD；
- 各非零 root digest 与重算一致；
- `file_uuid` 与 Header 一致。

volume_set：全部 index root 与 `entry_count`/`stream_count`/`index_page_count` 必须为 0。
file_set：`entry_count > 0` 时 Namespace 与 Entry ID root 必须有效；`volume_chunk_count == 0`；
`stream_count > 0` ⇔ Stream root 有效；`file_stream_chunk_count > 0` ⇔ Chunk root 有效；两个 dedup 计数必须为 0。

volume_set 的 `deduplicated_block_count` 是 DEDUP entry 总数，`deduplicated_logical_bytes` 是这些 entry
展开后的逻辑长度总和。两者必须同时为 0 或同时大于 0，且不得计入 ZERO、压缩节省或 Incremental 父层省略。
Header 未设置 DEDUP 时两者必须为 0；设置 DEDUP 但未命中重复时也为 0。
Volume Restore/完整 Verify 读取最后一个 Chunk 后必须重算两个计数并与 Footer 一致；不一致为 corrupt。

file_set Footer 计数语义（FI4）：

| 字段 | 含义 |
| --- | --- |
| `entry_count` / `stream_count` | tip File Index 中的完整当前树（含 parent-referenced stream） |
| `file_stream_chunk_count` / `total_block_entry_count` / `total_payload_size` | 仅本层 local stream payload |
| `logical_bytes` | 仅本层 local payload 逻辑字节（不含 parent 引用） |

Incremental 不得把 parent payload 复制进本层仍标 incremental；parent stream 在 Index 中只有
`content_storage=parent` 与 `parent_stream_index`。

## 7. CBOR Metadata Envelope

与 V6 相同的 envelope 布局与算法约束，HKDF info 升级为 V7：

```text
metadata_key = HKDF-SHA256(master, salt, "MYBACKUP-V7-CBOR-METADATA", 32)
payload_key  = HKDF-SHA256(master, salt, "MYBACKUP-V7-CHUNK-PAYLOAD", 32)
index_key    = HKDF-SHA256(master, salt, "MYBACKUP-V7-FILE-INDEX-PAGE", 32)
```

`CborMetadataEnvelopeHeader` 仍为 124 字节，`magic = "MYBKCBR"`，正式文件强制
`XCHACHA20-POLY1305` + Argon2id/外部 master。

AEAD AAD：

1. 本 part `BackupHeader` 256 B；
2. envelope header（不含 ciphertext/tag）；
3. 每个 key slot 完整字节。

明文上限：`plaintext_size <= 64 MiB`。

### 7.1 volume_set 根 Map

与既有 V6 语义相同（text keys）：

```text
{
  "schema_version": 1,
  "content_kind": 1,
  "disks": [],
  "volumes": [],
  "system": {...},
  "backup_job": {...},
  "extensions": {}
}
```

`disks`/`volumes`/`system`/`backup_job` 字段集与 V6 文档一致；V7 仅要求根上显式 `content_kind=1`。
增量按 Sidecar 的 DATA/ZERO/FREE 精确状态比较：仅状态相同（DATA 还要求 SHA-256 相同）才省略；
DATA→FREE、ZERO→FREE、FREE→ZERO 都必须在当前层显式写对应 BlockEntry。链 Reader 合并各层 FREE
区间并向恢复管线暴露最终跳写范围。多 Volume Snapshot Set 语义保持不变。

### 7.2 file_set 根 Map

```text
{
  "schema_version": 1,
  "content_kind": 2,
  "disks": [],
  "volumes": [],
  "system": {...},
  "backup_job": {
    "backup_type": 1|2,            // 1=full 2=incremental
    "created_utc": "...",
    "application_version": "...",
    "description": "..."
  },
  "file_set_baseline": {
    "fingerprint_algorithm": 1,    // SHA-256 over canonical preimage (algorithm id 1)
    "selection_fingerprint": bstr, // exactly 32 bytes; non-zero
    "change_detection_method": 1   // 1=mtime_size_v1
  },
  "extensions": {}
}
```

规则：

- Full/Incremental 均必须携带有效 `selection_fingerprint`；
- Full/Incremental 均必须携带 `change_detection_method=1`，且 Incremental Header 置
  `CAP_FILE_METADATA_BASELINE`；
- current V7 file_set metadata 不包含 `journal_checkpoints`；含该字段的开发期 Archive 统一按
  unsupported/corrupt 拒绝；
- 禁止在 file_set CBOR 中嵌入完整文件树、ACL 或逐文件路径列表。`relative_components` 只存在于
  控制面 durable selection 与 Worker Job，不进入 Catalog；Archive 内路径组件只存在于认证 File Index。

## 8. 压缩

- Header `compression_method`：volume 与 file_set 默认均为 `COMPRESSION_ZSTD`（1）。
- `COMPRESSION_ZSTD`：每个 BlockEntry 的 stored payload 为独立 zstd frame；
- volume_set 启用 DEDUP 时先选择 plaintext canonical，再仅对 canonical 执行机会性压缩；
- file_set 写入为**机会性**压缩：仅当 zstd 输出严格小于逻辑块长度时使用 `COMPRESSED`，否则 `RAW`；
- 解压后长度必须等于该 entry 的 `logical_size`（RAW 时 `stored_size == logical_size`）；
- 解压输出上限 `min(block_size, entry.logical_size)`（file stream 最后一块可小于 block_size）；
- 禁止共享 dictionary 跨 chunk。

## 9. Reader 顺序与拒绝

推荐打开顺序：

1. 验证首卷 Header；
2. 发现全部分卷并验证各 Header 身份字段；
3. 定位末卷 Footer 并校验计数；
4. 认证 CBOR metadata；
5. file_set：打开 root index page（跨 part offset），验证 digest，按需分页认证；
6. 解析 stream extent 后再读对应 chunk；认证失败不得返回文件内容。

统一拒绝类：

| 条件 | 结果 |
| --- | --- |
| version/magic/kind 不匹配 | unsupported 或 corrupt |
| AEAD/tag/digest 失败 | corrupt |
| 超限 count/size/depth | corrupt |
| 重复 key / 环 / 不可达 | corrupt |
| extent 指向缺失 chunk 或错误 source_index | corrupt |
| DEDUP 未声明、前向/越界引用、目标类型或长度非法 | corrupt |
| FREE run 越界、与逻辑块不对齐或携带 payload | corrupt |
| 未知 critical record kind | corrupt |

## 10. 与产品上限的关系

数值上限以 [FILE_SET_PRODUCT_LIMITS_AND_CODES.md](../development/FILE_SET_PRODUCT_LIMITS_AND_CODES.md)
为权威；本格式内写明的硬上限不得被产品配置放宽到超过格式上限。

## 11. 人工损坏样本矩阵（格式层）

隔离目录中复制合法 Archive 后实施，**不得提交仓库**：

| ID | 损坏 | 期望 |
| --- | --- | --- |
| C01 | Header `format_version=6` | 拒绝打开 |
| C02 | Header `content_kind=0` | 拒绝 |
| C03 | 翻转 CBOR tag 1 字节 | metadata 认证失败 |
| C04 | 截断末卷去掉 Footer | 未完成/corrupt |
| C05 | Footer `index_root_offset` 指错 | index 认证/定位失败 |
| C06 | 翻转某 index page ciphertext | page AEAD 失败 |
| C07 | leaf 中制造 parent 环 | 图校验失败 |
| C08 | extent `chunk_index` 越界 | 引用失败 |
| C09 | 删除中间分卷 | 分卷发现失败 |
| C10 | 交换两个 part 的 Header uuid | 身份不一致 |
| C11 | file_set 写入 volume chunk kind | kind/content 不兼容 |
| C12 | 截断 file stream payload | chunk 认证或长度失败 |

完整任务/UI 矩阵见产品上限文档。
