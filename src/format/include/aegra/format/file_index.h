#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace aegra::format::file_index {

inline constexpr std::uint8_t kNameEncodingWindowsUtf16Le = 1;
inline constexpr std::uint32_t kMaximumLeafEntriesPerPage = 256;
inline constexpr std::uint32_t kMaximumInternalKeysPerPage = 256;
inline constexpr std::uint32_t kMaximumIndexDepth = 8;
inline constexpr std::uint64_t kMaximumEntries = 10'000'000;
inline constexpr char kIndexRootDigestLabel[] = "MYBACKUP-V7-INDEX-ROOT";
inline constexpr char kEntryIdIndexRootDigestLabel[] = "MYBACKUP-V7-ENTRY-ID-INDEX-ROOT";
inline constexpr char kStreamIndexRootDigestLabel[] = "MYBACKUP-V7-STREAM-INDEX-ROOT";
inline constexpr char kChunkIndexRootDigestLabel[] = "MYBACKUP-V7-CHUNK-INDEX-ROOT";

/// CBOR / FileIndexPageHeader page_kind (ADR-0019).
inline constexpr std::uint16_t kPageKindNamespaceLeaf = 1;
inline constexpr std::uint16_t kPageKindNamespaceInternal = 2;
inline constexpr std::uint16_t kPageKindEntryIdLeaf = 3;
inline constexpr std::uint16_t kPageKindEntryIdInternal = 4;
inline constexpr std::uint16_t kPageKindStreamLeaf = 5;
inline constexpr std::uint16_t kPageKindStreamInternal = 6;
inline constexpr std::uint16_t kPageKindChunkLeaf = 7;
inline constexpr std::uint16_t kPageKindChunkInternal = 8;

struct IndexKey final {
    std::uint64_t parent_entry_id{0};
    std::uint8_t name_encoding{kNameEncodingWindowsUtf16Le};
    std::vector<std::byte> name_bytes;
    std::uint64_t entry_id{0};

    [[nodiscard]] bool operator==(const IndexKey&) const = default;
};

/// Physical child pointer inside authenticated internal pages (ADR-0019).
struct ChildPageLocator final {
    std::uint64_t page_id{0};
    std::uint64_t offset{0}; // absolute ArchiveRecordPrefix offset of the child page

    [[nodiscard]] bool operator==(const ChildPageLocator&) const = default;
};

struct EntryIdIndexRecord final {
    std::uint64_t entry_id{0};
    std::uint64_t page_id{0};
    /// Absolute ArchiveRecordPrefix offset of the Namespace leaf that holds the full entry.
    std::uint64_t page_offset{0};
    std::uint32_t slot{0};
    std::uint64_t parent_entry_id{0};
    std::uint8_t kind{0}; // ENTRY_KIND_*
};

struct StreamIndexRecord final {
    std::uint32_t stream_index{0};
    std::uint64_t entry_id{0};
    std::uint32_t stream_slot{0};
};

struct ChunkIndexRecord final {
    std::uint64_t chunk_index{0};
    std::uint64_t record_offset{0};
    std::uint64_t payload_offset{0};
    std::uint64_t payload_size{0};
    std::uint32_t block_entry_count{0};
};

[[nodiscard]] int compare_index_keys(const IndexKey& left, const IndexKey& right) noexcept;

[[nodiscard]] base::Result<IndexKey> make_index_key(const contracts::FileEntryDesc& entry);

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_leaf_entry_cbor(const contracts::FileEntryDesc& entry);

[[nodiscard]] base::Result<contracts::FileEntryDesc>
decode_leaf_entry_cbor(std::span<const std::byte> encoded);

struct InternalPageBody final {
    std::vector<IndexKey> keys;
    std::vector<ChildPageLocator> children; // length = keys + 1
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_internal_page_cbor(const InternalPageBody& body);

[[nodiscard]] base::Result<InternalPageBody>
decode_internal_page_cbor(std::span<const std::byte> encoded);

/// Secondary-tree internal page: integer separator keys + child locators.
struct IntegerInternalPageBody final {
    std::uint16_t page_kind{kPageKindEntryIdInternal};
    std::vector<std::uint64_t> keys;
    std::vector<ChildPageLocator> children; // length = keys + 1
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_integer_internal_page_cbor(const IntegerInternalPageBody& body);

[[nodiscard]] base::Result<IntegerInternalPageBody>
decode_integer_internal_page_cbor(std::span<const std::byte> encoded);

struct LeafPageBody final {
    std::vector<contracts::FileEntryDesc> entries;
};

[[nodiscard]] base::Result<std::vector<std::byte>> encode_leaf_page_cbor(const LeafPageBody& body);

[[nodiscard]] base::Result<LeafPageBody> decode_leaf_page_cbor(std::span<const std::byte> encoded);

[[nodiscard]] base::Result<void> validate_leaf_page_sorted(const LeafPageBody& body);

struct EntryIdLeafPageBody final {
    std::vector<EntryIdIndexRecord> records;
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_entry_id_leaf_page_cbor(const EntryIdLeafPageBody& body);

[[nodiscard]] base::Result<EntryIdLeafPageBody>
decode_entry_id_leaf_page_cbor(std::span<const std::byte> encoded);

struct StreamLeafPageBody final {
    std::vector<StreamIndexRecord> records;
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_stream_leaf_page_cbor(const StreamLeafPageBody& body);

[[nodiscard]] base::Result<StreamLeafPageBody>
decode_stream_leaf_page_cbor(std::span<const std::byte> encoded);

struct ChunkLeafPageBody final {
    std::vector<ChunkIndexRecord> records;
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_chunk_leaf_page_cbor(const ChunkLeafPageBody& body);

[[nodiscard]] base::Result<ChunkLeafPageBody>
decode_chunk_leaf_page_cbor(std::span<const std::byte> encoded);

/// Builds the SHA-256 preimage for a tree root digest (caller hashes the returned bytes).
[[nodiscard]] std::vector<std::byte>
make_index_root_digest_preimage(std::string_view label, std::uint64_t root_page_id,
                                std::uint64_t page_count, std::uint64_t primary_count,
                                std::uint64_t secondary_count,
                                std::span<const std::byte, 32> root_page_content_digest);

/// Namespace root preimage (label MYBACKUP-V7-INDEX-ROOT).
[[nodiscard]] std::vector<std::byte>
make_index_root_digest_preimage(std::uint64_t root_page_id, std::uint64_t page_count,
                                std::uint64_t entry_count, std::uint64_t stream_count,
                                std::span<const std::byte, 32> root_page_content_digest);

// --- platform metadata envelope (PERSONAL_BACKUP_FORMAT_V7 §5.7) ---
// LE: u16 version=1, u16 reserved=0, u32 flags=0, then sections:
//   u16 tag (bit15=critical), u16 reserved=0, u32 length, bytes[length]
// FI0: only security-descriptor section (tag 1). Reparse section is unsupported.
inline constexpr std::uint16_t kPlatformEnvelopeVersion = 1;
inline constexpr std::uint16_t kPlatformSectionSecurity = 1;
inline constexpr std::uint16_t kPlatformTagCriticalMask = 0x8000U;

/// Builds platform_metadata with a single security-descriptor section (tag 1).
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_platform_security_envelope(std::span<const std::byte> self_relative_security_descriptor);

/// Extracts tag-1 self-relative SECURITY_DESCRIPTOR bytes; empty if section absent.
[[nodiscard]] base::Result<std::vector<std::byte>>
extract_platform_security_descriptor(std::span<const std::byte> platform_metadata);

} // namespace aegra::format::file_index
