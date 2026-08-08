#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::format::file_index {

inline constexpr std::uint8_t kNameEncodingWindowsUtf16Le = 1;
inline constexpr std::uint32_t kMaximumLeafEntriesPerPage = 256;
inline constexpr std::uint32_t kMaximumInternalKeysPerPage = 256;
inline constexpr std::uint32_t kMaximumIndexDepth = 8;
inline constexpr std::uint64_t kMaximumEntries = 10'000'000;
inline constexpr char kIndexRootDigestLabel[] = "MYBACKUP-V7-INDEX-ROOT";

struct IndexKey final {
    std::uint64_t parent_entry_id{0};
    std::uint8_t name_encoding{kNameEncodingWindowsUtf16Le};
    std::vector<std::byte> name_bytes;
    std::uint64_t entry_id{0};

    [[nodiscard]] bool operator==(const IndexKey&) const = default;
};

[[nodiscard]] int compare_index_keys(const IndexKey& left, const IndexKey& right) noexcept;

[[nodiscard]] base::Result<IndexKey> make_index_key(const contracts::FileEntryDesc& entry);

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_leaf_entry_cbor(const contracts::FileEntryDesc& entry);

[[nodiscard]] base::Result<contracts::FileEntryDesc>
decode_leaf_entry_cbor(std::span<const std::byte> encoded);

struct InternalPageBody final {
    std::vector<IndexKey> keys;
    std::vector<std::uint64_t> children; // length = keys + 1
};

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_internal_page_cbor(const InternalPageBody& body);

[[nodiscard]] base::Result<InternalPageBody>
decode_internal_page_cbor(std::span<const std::byte> encoded);

struct LeafPageBody final {
    std::vector<contracts::FileEntryDesc> entries;
};

[[nodiscard]] base::Result<std::vector<std::byte>> encode_leaf_page_cbor(const LeafPageBody& body);

[[nodiscard]] base::Result<LeafPageBody> decode_leaf_page_cbor(std::span<const std::byte> encoded);

[[nodiscard]] base::Result<void> validate_leaf_page_sorted(const LeafPageBody& body);

/// Builds the SHA-256 preimage for index_root_digest (caller hashes the returned bytes).
[[nodiscard]] std::vector<std::byte>
make_index_root_digest_preimage(std::uint64_t root_page_id, std::uint64_t page_count,
                                std::uint64_t entry_count, std::uint64_t stream_count,
                                std::span<const std::byte, 32> root_page_content_digest);

// --- platform metadata envelope (PERSONAL_BACKUP_FORMAT_V7 §5.7) ---
// LE: u16 version=1, u16 reserved=0, u32 flags=0, then sections:
//   u16 tag (bit15=critical), u16 reserved=0, u32 length, bytes[length]
inline constexpr std::uint16_t kPlatformEnvelopeVersion = 1;
inline constexpr std::uint16_t kPlatformSectionSecurity = 1;
inline constexpr std::uint16_t kPlatformSectionReparse = 2;
inline constexpr std::uint16_t kPlatformTagCriticalMask = 0x8000U;

/// Builds platform_metadata with a single security-descriptor section (tag 1).
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_platform_security_envelope(std::span<const std::byte> self_relative_security_descriptor);

/// Extracts tag-1 self-relative SECURITY_DESCRIPTOR bytes; empty if section absent.
[[nodiscard]] base::Result<std::vector<std::byte>>
extract_platform_security_descriptor(std::span<const std::byte> platform_metadata);

} // namespace aegra::format::file_index
