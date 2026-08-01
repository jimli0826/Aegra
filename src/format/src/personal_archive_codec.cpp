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
constexpr std::array<char, 8> kChunkMagic = {'M', 'Y', 'B', 'K', 'C', 'H', 'K', '\0'};
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

[[nodiscard]] base::Result<void> validate_parent_uuid(const BackupHeader& header,
                                                      const std::uint32_t backup_type) {
    const bool parent_is_zero = is_zero_uuid(header.parent_uuid);
    if ((backup_type == kBackupFlagFull) != parent_is_zero) {
        return base::Result<void>::failure(corrupt("backup parent UUID is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_header_common(const BackupHeader& header) {
    constexpr auto known_flags = kBackupFlagFull | kBackupFlagIncremental |
                                 kBackupFlagDifferential | kBackupFlagDedup | kBackupFlagEncrypted |
                                 kBackupFlagSplit;
    const auto backup_type =
        header.flags & (kBackupFlagFull | kBackupFlagIncremental | kBackupFlagDifferential);
    if (header.block_size == 0 || header.default_chunk_size == 0 ||
        (header.flags & kBackupFlagEncrypted) == 0 || (header.flags & ~known_flags) != 0) {
        return base::Result<void>::failure(corrupt("backup header fields are invalid"));
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
        header.first_chunk_offset != header.cbor_offset + header.cbor_size) {
        return base::Result<void>::failure(corrupt("primary header offsets are inconsistent"));
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
        header.first_chunk_offset != kBackupHeaderSize) {
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
    if ((header.flags & kCborMetadataFlagEncrypted) == 0 ||
        header.encryption_method != MetadataEncryptionMethod::kXChaCha20Poly1305 ||
        header.kdf_method != MetadataKdfMethod::kArgon2Id) {
        return base::Result<void>::failure(corrupt("metadata envelope is not formally encrypted"));
    }
    if (header.nonce_size != 24 || header.tag_size != 16 || header.key_slot_count != 0) {
        return base::Result<void>::failure(corrupt("metadata envelope sizes are unsupported"));
    }
    if (header.plaintext_size == 0 || header.plaintext_size != header.ciphertext_size ||
        header.kdf_opslimit == 0 || header.kdf_memlimit_bytes == 0 ||
        header.kdf_parameters_version != 1) {
        return base::Result<void>::failure(corrupt("metadata envelope KDF fields are invalid"));
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

} // namespace

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
    write_integer(output, 92, header.first_chunk_offset);
    write_integer(output, 100, header.default_chunk_size);
    output[104] = static_cast<std::byte>(header.compression_method);
    output[105] = static_cast<std::byte>(header.encryption_method);
    write_integer(output, 106, header.split_part_index);
    write_integer(output, 110, header.split_part_count);
    write_integer(output, 114, header.split_size_bytes);
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
    BackupHeader result;
    result.file_uuid = read_bytes<16>(bytes, 16);
    result.backup_set_uuid = read_bytes<16>(bytes, 32);
    result.parent_uuid = read_bytes<16>(bytes, 48);
    result.block_size = read_integer<std::uint32_t>(bytes, 64);
    result.flags = read_integer<std::uint32_t>(bytes, 68);
    result.cbor_offset = read_integer<std::uint64_t>(bytes, 72);
    result.cbor_size = read_integer<std::uint64_t>(bytes, 80);
    result.cbor_schema_version = read_integer<std::uint32_t>(bytes, 88);
    result.first_chunk_offset = read_integer<std::uint64_t>(bytes, 92);
    result.default_chunk_size = read_integer<std::uint32_t>(bytes, 100);
    result.compression_method =
        static_cast<CompressionMethod>(std::to_integer<std::uint8_t>(bytes[104]));
    result.encryption_method =
        static_cast<PayloadEncryptionMethod>(std::to_integer<std::uint8_t>(bytes[105]));
    result.split_part_index = read_integer<std::uint32_t>(bytes, 106);
    result.split_part_count = read_integer<std::uint32_t>(bytes, 110);
    result.split_size_bytes = read_integer<std::uint64_t>(bytes, 114);
    auto validation = validate_header(result);
    return validation ? base::Result<BackupHeader>::success(result)
                      : base::Result<BackupHeader>::failure(validation.error());
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
    if (header.block_entry_count == 0) {
        return base::Result<EncodedChunkHeader>::failure(
            corrupt("chunk contains no block entries"));
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
    if (result.block_entry_count == 0) {
        return base::Result<ChunkHeader>::failure(corrupt("chunk contains no block entries"));
    }
    return base::Result<ChunkHeader>::success(result);
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
    if (footer.file_size < kBackupFooterSize) {
        return base::Result<EncodedBackupFooter>::failure(corrupt("footer file size is invalid"));
    }
    EncodedBackupFooter output{};
    write_magic(output, kFooterMagic);
    write_integer<std::uint16_t>(output, 8, kFooterVersion);
    write_integer<std::uint16_t>(output, 10, kFormatVersion);
    write_integer<std::uint32_t>(output, 12, kBackupFooterSize);
    write_integer(output, 16, footer.chunk_count);
    write_integer(output, 24, footer.total_block_count);
    write_integer(output, 32, footer.total_payload_size);
    write_integer(output, 40, footer.file_size);
    return base::Result<EncodedBackupFooter>::success(output);
}

base::Result<BackupFooter> decode_backup_footer(std::span<const std::byte> bytes) {
    if (bytes.size() < kBackupFooterSize || !has_magic(bytes, kFooterMagic)) {
        return base::Result<BackupFooter>::failure(corrupt("backup footer is invalid"));
    }
    if (read_integer<std::uint16_t>(bytes, 8) != kFooterVersion ||
        read_integer<std::uint16_t>(bytes, 10) != kFormatVersion) {
        return base::Result<BackupFooter>::failure(unsupported("footer version is unsupported"));
    }
    if (read_integer<std::uint32_t>(bytes, 12) != kBackupFooterSize) {
        return base::Result<BackupFooter>::failure(corrupt("footer size is invalid"));
    }
    BackupFooter result;
    result.chunk_count = read_integer<std::uint64_t>(bytes, 16);
    result.total_block_count = read_integer<std::uint64_t>(bytes, 24);
    result.total_payload_size = read_integer<std::uint64_t>(bytes, 32);
    result.file_size = read_integer<std::uint64_t>(bytes, 40);
    if (result.file_size < kBackupFooterSize) {
        return base::Result<BackupFooter>::failure(corrupt("footer file size is invalid"));
    }
    return base::Result<BackupFooter>::success(result);
}

} // namespace aegra::format::personal_archive
