#include "aegra/format/personal_archive.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <string>
#include <type_traits>
#include <utility>

namespace aegra::format::personal_archive {
namespace {

constexpr std::array<char, 8> kBackupMagic = {'M', 'Y', 'B', 'A', 'C', 'K', 'U', 'P'};
constexpr std::array<char, 8> kEnvelopeMagic = {'M', 'Y', 'B', 'K', 'C', 'B', 'R', '\0'};
constexpr std::array<char, 8> kRecordMagic = {'M', 'Y', 'B', 'K', 'R', 'E', 'C', '\0'};
constexpr std::array<char, 8> kChunkMagic = {'M', 'Y', 'B', 'K', 'C', 'H', 'K', '\0'};
constexpr std::array<char, 8> kIndexPageMagic = {'M', 'Y', 'B', 'K', 'I', 'D', 'X', '\0'};
constexpr std::array<char, 8> kFooterMagic = {'M', 'Y', 'B', 'K', 'E', 'N', 'D', '\0'};

[[nodiscard]] base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

[[nodiscard]] base::Error unsupported(std::string message) {
    return {base::ErrorCode::kUnsupportedVersion, std::move(message)};
}

template <typename Integer>
void write_integer(std::span<std::byte> bytes, const std::size_t offset, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
}

template <typename Integer>
[[nodiscard]] Integer read_integer(std::span<const std::byte> bytes, const std::size_t offset) {
    static_assert(std::is_unsigned_v<Integer>);
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

template <std::size_t Size>
void write_magic(std::span<std::byte> output, const std::array<char, Size>& magic) {
    std::transform(magic.begin(), magic.end(), output.begin(),
                   [](const char value) { return static_cast<std::byte>(value); });
}

template <std::size_t Size>
[[nodiscard]] bool has_magic(std::span<const std::byte> input,
                             const std::array<char, Size>& magic) {
    if (input.size() < magic.size()) {
        return false;
    }
    return std::equal(magic.begin(), magic.end(), input.begin(),
                      [](const char expected, const std::byte actual) {
                          return static_cast<std::byte>(expected) == actual;
                      });
}

template <std::size_t Size>
void write_bytes(std::span<std::byte> output, const std::size_t offset,
                 const std::array<std::byte, Size>& value) {
    std::copy(value.begin(), value.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
}

template <std::size_t Size>
[[nodiscard]] std::array<std::byte, Size> read_bytes(std::span<const std::byte> input,
                                                     const std::size_t offset) {
    std::array<std::byte, Size> result{};
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), Size, result.begin());
    return result;
}

[[nodiscard]] bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

template <std::size_t Size>
[[nodiscard]] bool is_zero_bytes(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] bool is_zero_span(const std::span<const std::byte> value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] bool root_is_absent(const IndexRootLocator& root) noexcept {
    return root.page_id == 0 && root.offset == 0 && is_zero_bytes(root.digest);
}

[[nodiscard]] bool root_is_present(const IndexRootLocator& root) noexcept {
    return root.page_id != 0 && root.offset != 0 && !is_zero_bytes(root.digest);
}

[[nodiscard]] base::Result<void> validate_footer_index_roots(const BackupFooter& footer) {
    IndexRootLocator namespace_root;
    namespace_root.page_id = footer.index_root_page_id;
    namespace_root.offset = footer.index_root_offset;
    namespace_root.digest = footer.index_root_digest;
    if (footer.entry_count == 0) {
        if (footer.stream_count != 0 || footer.file_stream_chunk_count != 0 ||
            footer.index_page_count != 0 || !root_is_absent(namespace_root) ||
            !root_is_absent(footer.entry_id_root) || !root_is_absent(footer.stream_root) ||
            !root_is_absent(footer.chunk_root)) {
            return base::Result<void>::failure(corrupt("volume footer index fields are invalid"));
        }
        return base::Result<void>::success();
    }
    if (footer.volume_chunk_count != 0 || footer.index_page_count < 2 ||
        !root_is_present(namespace_root) || !root_is_present(footer.entry_id_root) ||
        (root_is_present(footer.stream_root) != (footer.stream_count > 0)) ||
        (root_is_present(footer.chunk_root) != (footer.file_stream_chunk_count > 0))) {
        return base::Result<void>::failure(corrupt("file footer index fields are invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_parent_uuid(const BackupHeader& header,
                                                      const std::uint32_t backup_type) {
    const bool parent_is_zero = is_zero_uuid(header.parent_uuid);
    if ((backup_type == kBackupFlagFull) != parent_is_zero) {
        return base::Result<void>::failure(corrupt("backup parent UUID is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_block_size(const BackupHeader& header) {
    if (header.block_size == 0 || header.block_size > kMaximumBlockSizeBytes) {
        return base::Result<void>::failure(corrupt("backup block size is invalid"));
    }
    if (header.content_kind == kContentKindVolumeSet) {
        if (header.block_size % kVolumeBlockSizeAlignment != 0) {
            return base::Result<void>::failure(corrupt("volume block size alignment is invalid"));
        }
        return base::Result<void>::success();
    }
    if (header.block_size < kMinimumFileBlockSizeBytes ||
        header.block_size % kFileBlockSizeAlignment != 0) {
        return base::Result<void>::failure(corrupt("file block size is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_content_kind(const BackupHeader& header) {
    if (header.content_kind != kContentKindVolumeSet &&
        header.content_kind != kContentKindFileSet) {
        return base::Result<void>::failure(corrupt("backup content kind is invalid"));
    }
    const auto known_caps =
        kCapabilityHasFileIndex | kCapabilityVolumeSidecarOk | kCapabilityFileMetadataBaseline;
    if ((header.capability_flags & ~known_caps) != 0) {
        return base::Result<void>::failure(corrupt("backup capability flags are unknown"));
    }
    if (header.content_kind == kContentKindFileSet) {
        const auto backup_type =
            header.flags & (kBackupFlagFull | kBackupFlagIncremental | kBackupFlagDifferential);
        if ((header.capability_flags & kCapabilityHasFileIndex) == 0 ||
            (header.capability_flags & kCapabilityVolumeSidecarOk) != 0 ||
            backup_type == kBackupFlagDifferential) {
            return base::Result<void>::failure(corrupt("file_set header flags are invalid"));
        }
        if (backup_type == kBackupFlagFull) {
            if (!is_zero_uuid(header.parent_uuid)) {
                return base::Result<void>::failure(corrupt("file_set full parent UUID is invalid"));
            }
        } else if (backup_type == kBackupFlagIncremental) {
            if (is_zero_uuid(header.parent_uuid) ||
                (header.capability_flags & kCapabilityFileMetadataBaseline) == 0) {
                return base::Result<void>::failure(corrupt(
                    "file_set incremental requires parent and metadata baseline capability"));
            }
        } else {
            return base::Result<void>::failure(corrupt("file_set backup type flags are invalid"));
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_header_common(const BackupHeader& header) {
    constexpr auto known_flags = kBackupFlagFull | kBackupFlagIncremental |
                                 kBackupFlagDifferential | kBackupFlagDedup | kBackupFlagEncrypted |
                                 kBackupFlagSplit;
    const auto backup_type =
        header.flags & (kBackupFlagFull | kBackupFlagIncremental | kBackupFlagDifferential);
    if (header.default_chunk_size == 0 || (header.flags & ~known_flags) != 0) {
        return base::Result<void>::failure(corrupt("backup header fields are invalid"));
    }
    auto block_size = validate_block_size(header);
    if (!block_size) {
        return block_size;
    }
    auto content = validate_content_kind(header);
    if (!content) {
        return content;
    }
    const bool encrypted = (header.flags & kBackupFlagEncrypted) != 0;
    if (encrypted) {
        if (header.encryption_method != PayloadEncryptionMethod::kXChaCha20Poly1305) {
            return base::Result<void>::failure(corrupt("backup algorithms are unsupported"));
        }
    } else if (header.encryption_method != PayloadEncryptionMethod::kNone) {
        return base::Result<void>::failure(corrupt("unencrypted backup has invalid method"));
    }
    if (header.compression_method != CompressionMethod::kZstandard &&
        header.compression_method != CompressionMethod::kNone) {
        return base::Result<void>::failure(corrupt("backup algorithms are unsupported"));
    }
    if (backup_type != kBackupFlagFull && backup_type != kBackupFlagIncremental &&
        backup_type != kBackupFlagDifferential) {
        return base::Result<void>::failure(corrupt("backup type flags are invalid"));
    }
    auto parent = validate_parent_uuid(header, backup_type);
    if (!parent) {
        return parent;
    }
    if (header.cbor_schema_version != kManifestSchemaVersion) {
        return base::Result<void>::failure(unsupported("manifest schema version is unsupported"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_primary_header(const BackupHeader& header) {
    if (header.cbor_size == 0 || header.cbor_offset != kBackupHeaderSize ||
        header.first_record_offset != header.cbor_offset + header.cbor_size) {
        return base::Result<void>::failure(corrupt("primary header offsets are inconsistent"));
    }
    if (header.cbor_size > kMaximumCborMetadataBytes + kMetadataEnvelopeHeaderSize + 16U) {
        return base::Result<void>::failure(corrupt("primary metadata size exceeds limit"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_single_file_header(const BackupHeader& header) {
    if (header.split_part_index != 0 || header.split_part_count != 0 ||
        header.split_size_bytes != 0) {
        return base::Result<void>::failure(corrupt("single-file header has split fields"));
    }
    return validate_primary_header(header);
}

[[nodiscard]] base::Result<void> validate_split_header(const BackupHeader& header) {
    if (header.split_size_bytes == 0 ||
        (header.split_part_count != 0 && header.split_part_count <= header.split_part_index)) {
        return base::Result<void>::failure(corrupt("split header fields are invalid"));
    }
    if (header.split_part_index == 0) {
        return validate_primary_header(header);
    }
    if (header.cbor_offset != kBackupHeaderSize || header.cbor_size != 0 ||
        header.first_record_offset != kBackupHeaderSize) {
        return base::Result<void>::failure(corrupt("continuation header offsets are invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_header(const BackupHeader& header) {
    auto common = validate_header_common(header);
    if (!common) {
        return common;
    }
    return (header.flags & kBackupFlagSplit) != 0 ? validate_split_header(header)
                                                  : validate_single_file_header(header);
}

[[nodiscard]] base::Result<void> validate_envelope(const MetadataEnvelopeHeader& header) {
    if (header.key_slot_count != 0 || header.plaintext_size == 0 ||
        header.plaintext_size != header.ciphertext_size) {
        return base::Result<void>::failure(corrupt("metadata envelope sizes are invalid"));
    }
    const bool encrypted = (header.flags & kCborMetadataFlagEncrypted) != 0;
    if (encrypted) {
        if (header.encryption_method != MetadataEncryptionMethod::kXChaCha20Poly1305 ||
            header.kdf_method != MetadataKdfMethod::kArgon2Id || header.nonce_size != 24 ||
            header.tag_size != 16 || header.kdf_opslimit == 0 || header.kdf_memlimit_bytes == 0 ||
            header.kdf_parameters_version != 1) {
            return base::Result<void>::failure(
                corrupt("metadata envelope is not formally encrypted"));
        }
        return base::Result<void>::success();
    }
    if (header.encryption_method != MetadataEncryptionMethod::kNone ||
        header.kdf_method != MetadataKdfMethod::kNone || header.tag_size != 0 ||
        header.kdf_opslimit != 0 || header.kdf_memlimit_bytes != 0 ||
        header.kdf_parameters_version != 0) {
        return base::Result<void>::failure(corrupt("unencrypted metadata envelope is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_block_entry(const BlockEntry& entry) {
    switch (entry.flags) {
    case kBlockFlagRaw:
    case kBlockFlagCompressed:
        if (entry.stored_size == 0 || entry.logical_size == 0) {
            return base::Result<void>::failure(corrupt("stored block entry has invalid size"));
        }
        break;
    case kBlockFlagZero:
        if (entry.stored_size != 0 || entry.logical_size == 0) {
            return base::Result<void>::failure(corrupt("zero block entry has invalid size"));
        }
        break;
    case kBlockFlagDedup:
        if (entry.stored_size != 0 || entry.logical_size != 0) {
            return base::Result<void>::failure(
                corrupt("deduplicated block entry has invalid size"));
        }
        break;
    default:
        return base::Result<void>::failure(corrupt("block entry has invalid flags"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_record_prefix(const ArchiveRecordPrefix& prefix) {
    if (prefix.prefix_version != kRecordPrefixVersion) {
        return base::Result<void>::failure(
            unsupported("archive record prefix version unsupported"));
    }
    switch (prefix.record_kind) {
    case kRecordKindVolumeChunk:
        if (prefix.header_size != kVolumeChunkRecordHeaderSize) {
            return base::Result<void>::failure(corrupt("volume chunk record header size invalid"));
        }
        break;
    case kRecordKindFileStreamChunk:
        if (prefix.header_size != kFileStreamChunkRecordHeaderSize) {
            return base::Result<void>::failure(
                corrupt("file stream chunk record header size invalid"));
        }
        break;
    case kRecordKindFileIndexPage:
        if (prefix.header_size != kFileIndexPageRecordHeaderSize) {
            return base::Result<void>::failure(corrupt("index page record header size invalid"));
        }
        break;
    case kRecordKindFooter:
        if (prefix.header_size != kArchiveRecordPrefixSize ||
            prefix.body_size != kBackupFooterBodySize) {
            return base::Result<void>::failure(corrupt("footer record sizes are invalid"));
        }
        break;
    default:
        return base::Result<void>::failure(corrupt("archive record kind is unknown"));
    }
    if (prefix.flags != 0 || prefix.header_size < kArchiveRecordPrefixSize) {
        return base::Result<void>::failure(corrupt("archive record prefix fields are invalid"));
    }
    return base::Result<void>::success();
}

} // namespace

ArchiveRecordPrefix make_volume_chunk_record_prefix(const std::uint64_t body_size) noexcept {
    return {kRecordPrefixVersion, kRecordKindVolumeChunk, kVolumeChunkRecordHeaderSize, body_size,
            0};
}

ArchiveRecordPrefix make_file_stream_chunk_record_prefix(const std::uint64_t body_size) noexcept {
    return {kRecordPrefixVersion, kRecordKindFileStreamChunk, kFileStreamChunkRecordHeaderSize,
            body_size, 0};
}

ArchiveRecordPrefix make_file_index_page_record_prefix(const std::uint64_t body_size) noexcept {
    return {kRecordPrefixVersion, kRecordKindFileIndexPage, kFileIndexPageRecordHeaderSize,
            body_size, 0};
}

ArchiveRecordPrefix make_footer_record_prefix() noexcept {
    return {kRecordPrefixVersion, kRecordKindFooter, kArchiveRecordPrefixSize,
            kBackupFooterBodySize, 0};
}

base::Result<EncodedBackupHeader> encode_backup_header(const BackupHeader& header) {
    auto validation = validate_header(header);
    if (!validation) {
        return base::Result<EncodedBackupHeader>::failure(validation.error());
    }
    EncodedBackupHeader output{};
    write_magic(output, kBackupMagic);
    write_integer<std::uint16_t>(output, 8, kHeaderVersion);
    write_integer<std::uint16_t>(output, 10, kFormatVersion);
    write_integer<std::uint32_t>(output, 12, kBackupHeaderSize);
    write_bytes(output, 16, header.file_uuid);
    write_bytes(output, 32, header.backup_set_uuid);
    write_bytes(output, 48, header.parent_uuid);
    write_integer(output, 64, header.block_size);
    write_integer(output, 68, header.flags);
    write_integer(output, 72, header.cbor_offset);
    write_integer(output, 80, header.cbor_size);
    write_integer(output, 88, header.cbor_schema_version);
    write_integer(output, 92, header.first_record_offset);
    write_integer(output, 100, header.default_chunk_size);
    output[104] = static_cast<std::byte>(header.compression_method);
    output[105] = static_cast<std::byte>(header.encryption_method);
    output[106] = static_cast<std::byte>(header.content_kind);
    output[107] = std::byte{0};
    write_integer(output, 108, header.split_part_index);
    write_integer(output, 112, header.split_part_count);
    write_integer(output, 116, header.split_size_bytes);
    write_integer(output, 124, header.capability_flags);
    // reserved[128] already zero-initialized
    return base::Result<EncodedBackupHeader>::success(output);
}

base::Result<BackupHeader> decode_backup_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kBackupHeaderSize || !has_magic(bytes, kBackupMagic)) {
        return base::Result<BackupHeader>::failure(corrupt("backup header is missing or invalid"));
    }
    if (read_integer<std::uint16_t>(bytes, 8) != kHeaderVersion ||
        read_integer<std::uint16_t>(bytes, 10) != kFormatVersion) {
        return base::Result<BackupHeader>::failure(
            unsupported("backup format version is unsupported"));
    }
    if (read_integer<std::uint32_t>(bytes, 12) != kBackupHeaderSize) {
        return base::Result<BackupHeader>::failure(corrupt("backup header size is invalid"));
    }
    if (std::to_integer<std::uint8_t>(bytes[107]) != 0 || !is_zero_span(bytes.subspan(128, 128))) {
        return base::Result<BackupHeader>::failure(
            corrupt("backup header reserved fields are set"));
    }
    BackupHeader result;
    result.file_uuid = read_bytes<16>(bytes, 16);
    result.backup_set_uuid = read_bytes<16>(bytes, 32);
    result.parent_uuid = read_bytes<16>(bytes, 48);
    result.block_size = read_integer<std::uint32_t>(bytes, 64);
    result.flags = read_integer<std::uint32_t>(bytes, 68);
    result.cbor_offset = read_integer<std::uint64_t>(bytes, 72);
    result.cbor_size = read_integer<std::uint64_t>(bytes, 80);
    result.cbor_schema_version = read_integer<std::uint32_t>(bytes, 88);
    result.first_record_offset = read_integer<std::uint64_t>(bytes, 92);
    result.default_chunk_size = read_integer<std::uint32_t>(bytes, 100);
    result.compression_method =
        static_cast<CompressionMethod>(std::to_integer<std::uint8_t>(bytes[104]));
    result.encryption_method =
        static_cast<PayloadEncryptionMethod>(std::to_integer<std::uint8_t>(bytes[105]));
    result.content_kind = std::to_integer<std::uint8_t>(bytes[106]);
    result.split_part_index = read_integer<std::uint32_t>(bytes, 108);
    result.split_part_count = read_integer<std::uint32_t>(bytes, 112);
    result.split_size_bytes = read_integer<std::uint64_t>(bytes, 116);
    result.capability_flags = read_integer<std::uint32_t>(bytes, 124);
    auto validation = validate_header(result);
    return validation ? base::Result<BackupHeader>::success(result)
                      : base::Result<BackupHeader>::failure(validation.error());
}

base::Result<EncodedArchiveRecordPrefix>
encode_archive_record_prefix(const ArchiveRecordPrefix& prefix) {
    auto validation = validate_record_prefix(prefix);
    if (!validation) {
        return base::Result<EncodedArchiveRecordPrefix>::failure(validation.error());
    }
    EncodedArchiveRecordPrefix output{};
    write_magic(output, kRecordMagic);
    write_integer<std::uint16_t>(output, 8, prefix.prefix_version);
    write_integer<std::uint16_t>(output, 10, prefix.record_kind);
    write_integer(output, 12, prefix.header_size);
    write_integer(output, 16, prefix.body_size);
    write_integer(output, 24, prefix.flags);
    write_integer(output, 28, std::uint32_t{0});
    return base::Result<EncodedArchiveRecordPrefix>::success(output);
}

base::Result<ArchiveRecordPrefix> decode_archive_record_prefix(std::span<const std::byte> bytes) {
    if (bytes.size() < kArchiveRecordPrefixSize || !has_magic(bytes, kRecordMagic)) {
        return base::Result<ArchiveRecordPrefix>::failure(
            corrupt("archive record prefix is invalid"));
    }
    if (!is_zero_span(bytes.subspan(28, 4))) {
        return base::Result<ArchiveRecordPrefix>::failure(
            corrupt("archive record prefix reserved is set"));
    }
    ArchiveRecordPrefix result;
    result.prefix_version = read_integer<std::uint16_t>(bytes, 8);
    result.record_kind = read_integer<std::uint16_t>(bytes, 10);
    result.header_size = read_integer<std::uint32_t>(bytes, 12);
    result.body_size = read_integer<std::uint64_t>(bytes, 16);
    result.flags = read_integer<std::uint32_t>(bytes, 24);
    auto validation = validate_record_prefix(result);
    return validation ? base::Result<ArchiveRecordPrefix>::success(result)
                      : base::Result<ArchiveRecordPrefix>::failure(validation.error());
}

base::Result<EncodedMetadataEnvelopeHeader>
encode_metadata_envelope_header(const MetadataEnvelopeHeader& header) {
    auto validation = validate_envelope(header);
    if (!validation) {
        return base::Result<EncodedMetadataEnvelopeHeader>::failure(validation.error());
    }
    EncodedMetadataEnvelopeHeader output{};
    write_magic(output, kEnvelopeMagic);
    write_integer<std::uint16_t>(output, 8, kEnvelopeVersion);
    write_integer<std::uint16_t>(output, 10, kMetadataEnvelopeHeaderSize);
    write_integer(output, 12, header.flags);
    output[16] = static_cast<std::byte>(header.encryption_method);
    output[17] = static_cast<std::byte>(header.kdf_method);
    output[18] = static_cast<std::byte>(header.nonce_size);
    output[19] = static_cast<std::byte>(header.tag_size);
    write_integer(output, 20, header.key_slot_count);
    write_integer(output, 24, header.plaintext_size);
    write_integer(output, 32, header.ciphertext_size);
    write_bytes(output, 40, header.salt);
    write_bytes(output, 72, header.nonce);
    write_integer(output, 96, header.kdf_opslimit);
    write_integer(output, 104, header.kdf_memlimit_bytes);
    write_integer(output, 112, header.kdf_parameters_version);
    return base::Result<EncodedMetadataEnvelopeHeader>::success(output);
}

base::Result<MetadataEnvelopeHeader>
decode_metadata_envelope_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kMetadataEnvelopeHeaderSize || !has_magic(bytes, kEnvelopeMagic)) {
        return base::Result<MetadataEnvelopeHeader>::failure(
            corrupt("metadata envelope is invalid"));
    }
    if (read_integer<std::uint16_t>(bytes, 8) != kEnvelopeVersion) {
        return base::Result<MetadataEnvelopeHeader>::failure(
            unsupported("envelope version unsupported"));
    }
    if (read_integer<std::uint16_t>(bytes, 10) != kMetadataEnvelopeHeaderSize) {
        return base::Result<MetadataEnvelopeHeader>::failure(corrupt("envelope size is invalid"));
    }
    MetadataEnvelopeHeader result;
    result.flags = read_integer<std::uint32_t>(bytes, 12);
    result.encryption_method =
        static_cast<MetadataEncryptionMethod>(std::to_integer<std::uint8_t>(bytes[16]));
    result.kdf_method = static_cast<MetadataKdfMethod>(std::to_integer<std::uint8_t>(bytes[17]));
    result.nonce_size = std::to_integer<std::uint8_t>(bytes[18]);
    result.tag_size = std::to_integer<std::uint8_t>(bytes[19]);
    result.key_slot_count = read_integer<std::uint32_t>(bytes, 20);
    result.plaintext_size = read_integer<std::uint64_t>(bytes, 24);
    result.ciphertext_size = read_integer<std::uint64_t>(bytes, 32);
    result.salt = read_bytes<32>(bytes, 40);
    result.nonce = read_bytes<24>(bytes, 72);
    result.kdf_opslimit = read_integer<std::uint64_t>(bytes, 96);
    result.kdf_memlimit_bytes = read_integer<std::uint64_t>(bytes, 104);
    result.kdf_parameters_version = read_integer<std::uint32_t>(bytes, 112);
    auto validation = validate_envelope(result);
    return validation ? base::Result<MetadataEnvelopeHeader>::success(result)
                      : base::Result<MetadataEnvelopeHeader>::failure(validation.error());
}

base::Result<EncodedChunkHeader> encode_chunk_header(const ChunkHeader& header) {
    if (header.block_entry_count == 0 || header.flags != 0 || header.header_crc32 != 0 ||
        is_zero_bytes(header.payload_nonce) || header.source_type != kSourceTypeVolume ||
        header.block_entry_count > kMaximumBlockEntriesPerChunk ||
        header.payload_size > kMaximumChunkPayloadBytes) {
        return base::Result<EncodedChunkHeader>::failure(
            corrupt("chunk encryption fields are invalid"));
    }
    EncodedChunkHeader output{};
    write_magic(output, kChunkMagic);
    write_integer<std::uint32_t>(output, 8, kChunkHeaderSize);
    write_integer(output, 12, header.chunk_index);
    output[20] = static_cast<std::byte>(header.source_type);
    write_integer(output, 24, header.source_index);
    write_integer(output, 28, header.block_entry_count);
    write_integer(output, 36, header.payload_size);
    write_integer(output, 44, header.flags);
    write_integer(output, 48, header.header_crc32);
    write_bytes(output, 52, header.payload_nonce);
    write_bytes(output, 76, header.payload_authentication_tag);
    return base::Result<EncodedChunkHeader>::success(output);
}

base::Result<ChunkHeader> decode_chunk_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kChunkHeaderSize || !has_magic(bytes, kChunkMagic) ||
        read_integer<std::uint32_t>(bytes, 8) != kChunkHeaderSize) {
        return base::Result<ChunkHeader>::failure(corrupt("chunk header is invalid"));
    }
    ChunkHeader result;
    result.chunk_index = read_integer<std::uint64_t>(bytes, 12);
    result.source_type = std::to_integer<std::uint8_t>(bytes[20]);
    result.source_index = read_integer<std::uint32_t>(bytes, 24);
    result.block_entry_count = read_integer<std::uint32_t>(bytes, 28);
    result.payload_size = read_integer<std::uint64_t>(bytes, 36);
    result.flags = read_integer<std::uint32_t>(bytes, 44);
    result.header_crc32 = read_integer<std::uint32_t>(bytes, 48);
    result.payload_nonce = read_bytes<24>(bytes, 52);
    result.payload_authentication_tag = read_bytes<16>(bytes, 76);
    const bool reserved_source_is_zero =
        std::all_of(bytes.begin() + 21, bytes.begin() + 24,
                    [](const std::byte value) { return value == std::byte{0}; });
    const bool reserved_fields_are_zero =
        std::all_of(bytes.begin() + 32, bytes.begin() + 36,
                    [](const std::byte value) { return value == std::byte{0}; }) &&
        std::all_of(bytes.begin() + 92, bytes.begin() + 96,
                    [](const std::byte value) { return value == std::byte{0}; });
    if (result.block_entry_count == 0 || result.flags != 0 || result.header_crc32 != 0 ||
        is_zero_bytes(result.payload_nonce) || !reserved_source_is_zero ||
        !reserved_fields_are_zero || result.source_type != kSourceTypeVolume ||
        result.block_entry_count > kMaximumBlockEntriesPerChunk ||
        result.payload_size > kMaximumChunkPayloadBytes) {
        return base::Result<ChunkHeader>::failure(corrupt("chunk header fields are invalid"));
    }
    return base::Result<ChunkHeader>::success(result);
}

base::Result<EncodedFileStreamChunkHeader>
encode_file_stream_chunk_header(const FileStreamChunkHeader& header) {
    if (header.block_entry_count == 0 || header.chunk_flags != 0 ||
        is_zero_bytes(header.payload_nonce) || header.source_type != kSourceTypeFileStream ||
        header.block_entry_count > kMaximumBlockEntriesPerChunk ||
        header.payload_size > kMaximumChunkPayloadBytes) {
        return base::Result<EncodedFileStreamChunkHeader>::failure(
            corrupt("file stream chunk header fields are invalid"));
    }
    EncodedFileStreamChunkHeader output{};
    write_integer(output, 0, header.chunk_index);
    output[8] = static_cast<std::byte>(header.source_type);
    write_integer(output, 12, header.source_index);
    write_integer(output, 16, header.block_entry_count);
    write_integer(output, 24, header.payload_size);
    write_integer(output, 32, header.chunk_flags);
    write_bytes(output, 40, header.payload_nonce);
    write_bytes(output, 64, header.payload_authentication_tag);
    return base::Result<EncodedFileStreamChunkHeader>::success(output);
}

base::Result<FileStreamChunkHeader>
decode_file_stream_chunk_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kFileStreamChunkHeaderSize) {
        return base::Result<FileStreamChunkHeader>::failure(
            corrupt("file stream chunk header is truncated"));
    }
    FileStreamChunkHeader result;
    result.chunk_index = read_integer<std::uint64_t>(bytes, 0);
    result.source_type = std::to_integer<std::uint8_t>(bytes[8]);
    result.source_index = read_integer<std::uint32_t>(bytes, 12);
    result.block_entry_count = read_integer<std::uint32_t>(bytes, 16);
    result.payload_size = read_integer<std::uint64_t>(bytes, 24);
    result.chunk_flags = read_integer<std::uint32_t>(bytes, 32);
    result.payload_nonce = read_bytes<24>(bytes, 40);
    result.payload_authentication_tag = read_bytes<16>(bytes, 64);
    const bool reserved_zero =
        std::all_of(bytes.begin() + 9, bytes.begin() + 12,
                    [](const std::byte value) { return value == std::byte{0}; }) &&
        std::all_of(bytes.begin() + 20, bytes.begin() + 24,
                    [](const std::byte value) { return value == std::byte{0}; }) &&
        std::all_of(bytes.begin() + 36, bytes.begin() + 40,
                    [](const std::byte value) { return value == std::byte{0}; });
    if (!reserved_zero || result.block_entry_count == 0 || result.chunk_flags != 0 ||
        is_zero_bytes(result.payload_nonce) || result.source_type != kSourceTypeFileStream ||
        result.block_entry_count > kMaximumBlockEntriesPerChunk ||
        result.payload_size > kMaximumChunkPayloadBytes) {
        return base::Result<FileStreamChunkHeader>::failure(
            corrupt("file stream chunk header fields are invalid"));
    }
    return base::Result<FileStreamChunkHeader>::success(result);
}

[[nodiscard]] bool is_valid_index_page_kind(const std::uint16_t page_kind) noexcept {
    return page_kind == kIndexPageLeaf || page_kind == kIndexPageInternal ||
           page_kind == kIndexPageEntryIdLeaf || page_kind == kIndexPageEntryIdInternal ||
           page_kind == kIndexPageStreamLeaf || page_kind == kIndexPageStreamInternal ||
           page_kind == kIndexPageChunkLeaf || page_kind == kIndexPageChunkInternal;
}

base::Result<EncodedFileIndexPageHeader>
encode_file_index_page_header(const FileIndexPageHeader& header) {
    if (header.page_format_version != kFileIndexPageFormatVersion || header.page_id == 0 ||
        header.plain_size == 0 || header.plain_size > kMaximumIndexPagePlainBytes ||
        header.encoded_size != header.plain_size || header.protection_mode != kIndexProtectAead ||
        !is_valid_index_page_kind(header.page_kind) || is_zero_bytes(header.nonce)) {
        return base::Result<EncodedFileIndexPageHeader>::failure(
            corrupt("file index page header fields are invalid"));
    }
    EncodedFileIndexPageHeader output{};
    write_magic(output, kIndexPageMagic);
    write_integer<std::uint16_t>(output, 8, header.page_format_version);
    write_integer<std::uint16_t>(output, 10, header.page_kind);
    write_integer(output, 12, header.page_id);
    write_integer(output, 20, header.plain_size);
    write_integer(output, 24, header.encoded_size);
    output[28] = static_cast<std::byte>(header.protection_mode);
    write_bytes(output, 32, header.nonce);
    write_bytes(output, 56, header.authentication_tag);
    write_bytes(output, 72, header.content_digest);
    write_integer(output, 104, header.entry_count);
    return base::Result<EncodedFileIndexPageHeader>::success(output);
}

base::Result<FileIndexPageHeader> decode_file_index_page_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kFileIndexPageHeaderSize || !has_magic(bytes, kIndexPageMagic)) {
        return base::Result<FileIndexPageHeader>::failure(
            corrupt("file index page header is invalid"));
    }
    FileIndexPageHeader result;
    result.page_format_version = read_integer<std::uint16_t>(bytes, 8);
    result.page_kind = read_integer<std::uint16_t>(bytes, 10);
    result.page_id = read_integer<std::uint64_t>(bytes, 12);
    result.plain_size = read_integer<std::uint32_t>(bytes, 20);
    result.encoded_size = read_integer<std::uint32_t>(bytes, 24);
    result.protection_mode = std::to_integer<std::uint8_t>(bytes[28]);
    result.nonce = read_bytes<24>(bytes, 32);
    result.authentication_tag = read_bytes<16>(bytes, 56);
    result.content_digest = read_bytes<32>(bytes, 72);
    result.entry_count = read_integer<std::uint32_t>(bytes, 104);
    const bool reserved_zero =
        std::all_of(bytes.begin() + 29, bytes.begin() + 32,
                    [](const std::byte value) { return value == std::byte{0}; }) &&
        std::all_of(bytes.begin() + 108, bytes.begin() + 112,
                    [](const std::byte value) { return value == std::byte{0}; });
    if (!reserved_zero || result.page_format_version != kFileIndexPageFormatVersion ||
        result.page_id == 0 || result.plain_size == 0 ||
        result.plain_size > kMaximumIndexPagePlainBytes ||
        result.encoded_size != result.plain_size || result.protection_mode != kIndexProtectAead ||
        !is_valid_index_page_kind(result.page_kind) || is_zero_bytes(result.nonce)) {
        return base::Result<FileIndexPageHeader>::failure(
            corrupt("file index page header fields are invalid"));
    }
    return base::Result<FileIndexPageHeader>::success(result);
}

base::Result<EncodedBlockEntry> encode_block_entry(const BlockEntry& entry) {
    auto validation = validate_block_entry(entry);
    if (!validation) {
        return base::Result<EncodedBlockEntry>::failure(validation.error());
    }
    EncodedBlockEntry output{};
    write_integer(output, 0, entry.logical_block_index);
    write_integer(output, 8, entry.data_offset_or_reference);
    write_integer(output, 16, entry.stored_size);
    write_integer(output, 20, entry.logical_size);
    output[24] = static_cast<std::byte>(entry.flags);
    return base::Result<EncodedBlockEntry>::success(output);
}

base::Result<BlockEntry> decode_block_entry(std::span<const std::byte> bytes) {
    if (bytes.size() < kBlockEntrySize) {
        return base::Result<BlockEntry>::failure(corrupt("block entry is truncated"));
    }
    BlockEntry result;
    result.logical_block_index = read_integer<std::uint64_t>(bytes, 0);
    result.data_offset_or_reference = read_integer<std::uint64_t>(bytes, 8);
    result.stored_size = read_integer<std::uint32_t>(bytes, 16);
    result.logical_size = read_integer<std::uint32_t>(bytes, 20);
    result.flags = std::to_integer<std::uint8_t>(bytes[24]);
    auto validation = validate_block_entry(result);
    return validation ? base::Result<BlockEntry>::success(result)
                      : base::Result<BlockEntry>::failure(validation.error());
}

base::Result<EncodedBackupFooter> encode_backup_footer(const BackupFooter& footer) {
    if (footer.part_file_size < kBackupFooterSize) {
        return base::Result<EncodedBackupFooter>::failure(corrupt("footer file size is invalid"));
    }
    auto roots = validate_footer_index_roots(footer);
    if (!roots) {
        return base::Result<EncodedBackupFooter>::failure(roots.error());
    }
    const auto prefix = make_footer_record_prefix();
    auto encoded_prefix = encode_archive_record_prefix(prefix);
    if (!encoded_prefix) {
        return base::Result<EncodedBackupFooter>::failure(encoded_prefix.error());
    }
    EncodedBackupFooter output{};
    std::copy(encoded_prefix.value().begin(), encoded_prefix.value().end(), output.begin());
    write_magic(std::span(output).subspan(32), kFooterMagic);
    write_integer<std::uint16_t>(output, 40, kFooterVersion);
    write_integer<std::uint16_t>(output, 42, kFormatVersion);
    write_integer<std::uint32_t>(output, 44, kBackupFooterSize);
    write_integer(output, 48, footer.volume_chunk_count);
    write_integer(output, 56, footer.file_stream_chunk_count);
    write_integer(output, 64, footer.index_page_count);
    write_integer(output, 72, footer.total_block_entry_count);
    write_integer(output, 80, footer.total_payload_size);
    write_integer(output, 88, footer.logical_bytes);
    write_integer(output, 96, footer.stored_bytes);
    write_integer(output, 104, footer.entry_count);
    write_integer(output, 112, footer.stream_count);
    write_integer(output, 120, footer.index_root_part_index);
    write_integer(output, 124, std::uint32_t{0});
    write_integer(output, 128, footer.index_root_offset);
    write_integer(output, 136, footer.index_root_page_id);
    write_bytes(output, 144, footer.index_root_digest);
    write_integer(output, 176, footer.part_file_size);
    write_bytes(output, 184, footer.file_uuid);
    // body offset 168 (absolute 200): secondary roots ADR-0019
    write_integer(output, 200, footer.entry_id_root.page_id);
    write_integer(output, 208, footer.entry_id_root.offset);
    write_bytes(output, 216, footer.entry_id_root.digest);
    write_integer(output, 248, footer.stream_root.page_id);
    write_integer(output, 256, footer.stream_root.offset);
    write_bytes(output, 264, footer.stream_root.digest);
    write_integer(output, 296, footer.chunk_root.page_id);
    write_integer(output, 304, footer.chunk_root.offset);
    write_bytes(output, 312, footer.chunk_root.digest);
    // reserved body tail (absolute 344..) remains zero-initialized
    return base::Result<EncodedBackupFooter>::success(output);
}

base::Result<BackupFooter> decode_backup_footer(std::span<const std::byte> bytes) {
    if (bytes.size() < kBackupFooterSize) {
        return base::Result<BackupFooter>::failure(corrupt("backup footer is invalid"));
    }
    auto prefix = decode_archive_record_prefix(bytes.subspan(0, kArchiveRecordPrefixSize));
    if (!prefix || prefix.value().record_kind != kRecordKindFooter) {
        return base::Result<BackupFooter>::failure(
            !prefix ? prefix.error() : corrupt("backup footer record kind is invalid"));
    }
    const auto body = bytes.subspan(kArchiveRecordPrefixSize);
    if (!has_magic(body, kFooterMagic)) {
        return base::Result<BackupFooter>::failure(corrupt("backup footer magic is invalid"));
    }
    if (read_integer<std::uint16_t>(body, 8) != kFooterVersion ||
        read_integer<std::uint16_t>(body, 10) != kFormatVersion) {
        return base::Result<BackupFooter>::failure(unsupported("footer version is unsupported"));
    }
    if (read_integer<std::uint32_t>(body, 12) != kBackupFooterSize) {
        return base::Result<BackupFooter>::failure(corrupt("footer size is invalid"));
    }
    // body 168..311: secondary roots; body 312..479 (168 B) must remain zero.
    if (read_integer<std::uint32_t>(body, 92) != 0 || !is_zero_span(body.subspan(312, 168))) {
        return base::Result<BackupFooter>::failure(corrupt("footer reserved fields are set"));
    }
    BackupFooter result;
    result.volume_chunk_count = read_integer<std::uint64_t>(body, 16);
    result.file_stream_chunk_count = read_integer<std::uint64_t>(body, 24);
    result.index_page_count = read_integer<std::uint64_t>(body, 32);
    result.total_block_entry_count = read_integer<std::uint64_t>(body, 40);
    result.total_payload_size = read_integer<std::uint64_t>(body, 48);
    result.logical_bytes = read_integer<std::uint64_t>(body, 56);
    result.stored_bytes = read_integer<std::uint64_t>(body, 64);
    result.entry_count = read_integer<std::uint64_t>(body, 72);
    result.stream_count = read_integer<std::uint64_t>(body, 80);
    result.index_root_part_index = read_integer<std::uint32_t>(body, 88);
    result.index_root_offset = read_integer<std::uint64_t>(body, 96);
    result.index_root_page_id = read_integer<std::uint64_t>(body, 104);
    result.index_root_digest = read_bytes<32>(body, 112);
    result.part_file_size = read_integer<std::uint64_t>(body, 144);
    result.file_uuid = read_bytes<16>(body, 152);
    result.entry_id_root.page_id = read_integer<std::uint64_t>(body, 168);
    result.entry_id_root.offset = read_integer<std::uint64_t>(body, 176);
    result.entry_id_root.digest = read_bytes<32>(body, 184);
    result.stream_root.page_id = read_integer<std::uint64_t>(body, 216);
    result.stream_root.offset = read_integer<std::uint64_t>(body, 224);
    result.stream_root.digest = read_bytes<32>(body, 232);
    result.chunk_root.page_id = read_integer<std::uint64_t>(body, 264);
    result.chunk_root.offset = read_integer<std::uint64_t>(body, 272);
    result.chunk_root.digest = read_bytes<32>(body, 280);
    if (result.part_file_size < kBackupFooterSize) {
        return base::Result<BackupFooter>::failure(corrupt("footer file size is invalid"));
    }
    auto roots = validate_footer_index_roots(result);
    if (!roots) {
        return base::Result<BackupFooter>::failure(roots.error());
    }
    return base::Result<BackupFooter>::success(result);
}

} // namespace aegra::format::personal_archive
