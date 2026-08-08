#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::format::personal_archive {

inline constexpr std::uint16_t kFormatVersion = 7;
inline constexpr std::uint16_t kHeaderVersion = 2;
inline constexpr std::uint16_t kEnvelopeVersion = 1;
inline constexpr std::uint16_t kFooterVersion = 2;
inline constexpr std::uint16_t kRecordPrefixVersion = 1;
inline constexpr std::uint16_t kFileIndexPageFormatVersion = 1;
inline constexpr std::size_t kBackupHeaderSize = 256;
inline constexpr std::size_t kMetadataEnvelopeHeaderSize = 124;
inline constexpr std::size_t kArchiveRecordPrefixSize = 32;
inline constexpr std::size_t kVolumeChunkHeaderSize = 96;
inline constexpr std::size_t kFileStreamChunkHeaderSize = 80;
inline constexpr std::size_t kFileIndexPageHeaderSize = 112;
inline constexpr std::size_t kBlockEntrySize = 25;
inline constexpr std::size_t kBackupFooterBodySize = 480;
inline constexpr std::size_t kBackupFooterSize = 512; // entire footer record

// Volume chunk kind-specific header size (legacy name used by volume adapter paths).
inline constexpr std::size_t kChunkHeaderSize = kVolumeChunkHeaderSize;
inline constexpr std::size_t kVolumeChunkRecordHeaderSize =
    kArchiveRecordPrefixSize + kVolumeChunkHeaderSize; // 128
inline constexpr std::size_t kFileStreamChunkRecordHeaderSize =
    kArchiveRecordPrefixSize + kFileStreamChunkHeaderSize; // 112
inline constexpr std::size_t kFileIndexPageRecordHeaderSize =
    kArchiveRecordPrefixSize + kFileIndexPageHeaderSize; // 144

inline constexpr std::uint32_t kBackupFlagFull = 0x00000001;
inline constexpr std::uint32_t kBackupFlagIncremental = 0x00000002;
inline constexpr std::uint32_t kBackupFlagDifferential = 0x00000004;
inline constexpr std::uint32_t kBackupFlagDedup = 0x00000008;
inline constexpr std::uint32_t kBackupFlagEncrypted = 0x00000010;
inline constexpr std::uint32_t kBackupFlagSplit = 0x00000020;
inline constexpr std::uint32_t kCborMetadataFlagEncrypted = 0x00000001;

inline constexpr std::uint32_t kCapabilityHasFileIndex = 0x00000001;
inline constexpr std::uint32_t kCapabilityVolumeSidecarOk = 0x00000002;

inline constexpr std::uint8_t kContentKindVolumeSet = 1;
inline constexpr std::uint8_t kContentKindFileSet = 2;

inline constexpr std::uint16_t kRecordKindVolumeChunk = 1;
inline constexpr std::uint16_t kRecordKindFileStreamChunk = 2;
inline constexpr std::uint16_t kRecordKindFileIndexPage = 3;
inline constexpr std::uint16_t kRecordKindFooter = 4;

inline constexpr std::uint8_t kSourceTypeVolume = 1;
inline constexpr std::uint8_t kSourceTypeFileStream = 2;

inline constexpr std::uint16_t kIndexPageLeaf = 1;
inline constexpr std::uint16_t kIndexPageInternal = 2;
inline constexpr std::uint8_t kIndexProtectAead = 1;

inline constexpr std::uint8_t kBlockFlagRaw = 0x01;
inline constexpr std::uint8_t kBlockFlagCompressed = 0x02;
inline constexpr std::uint8_t kBlockFlagZero = 0x04;
inline constexpr std::uint8_t kBlockFlagDedup = 0x08;

inline constexpr std::uint64_t kMaximumCborMetadataBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumBlockSizeBytes = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kMinimumFileBlockSizeBytes = 4096U;
inline constexpr std::uint32_t kVolumeBlockSizeAlignment = 512U;
inline constexpr std::uint32_t kFileBlockSizeAlignment = 4096U;
inline constexpr std::uint64_t kMaximumChunkPayloadBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumBlockEntriesPerChunk = 1'048'576U;
inline constexpr std::uint32_t kMaximumIndexPagePlainBytes = 1U * 1024U * 1024U;

enum class CompressionMethod : std::uint8_t {
    kNone = 0,
    kZstandard = 1,
};

enum class PayloadEncryptionMethod : std::uint8_t {
    kNone = 0,
    kXChaCha20Poly1305 = 2,
};

enum class MetadataEncryptionMethod : std::uint8_t {
    kNone = 0,
    kAes256Gcm = 1,
    kXChaCha20Poly1305 = 2,
};

enum class MetadataKdfMethod : std::uint8_t {
    kNone = 0,
    kHkdfSha256 = 1,
    kArgon2Id = 2,
};

struct BackupHeader final {
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::array<std::byte, 16> parent_uuid{};
    std::uint32_t block_size{0};
    std::uint32_t flags{kBackupFlagFull};
    std::uint64_t cbor_offset{kBackupHeaderSize};
    std::uint64_t cbor_size{0};
    std::uint32_t cbor_schema_version{kManifestSchemaVersion};
    std::uint64_t first_record_offset{0};
    std::uint32_t default_chunk_size{0};
    CompressionMethod compression_method{CompressionMethod::kNone};
    PayloadEncryptionMethod encryption_method{PayloadEncryptionMethod::kNone};
    std::uint8_t content_kind{kContentKindVolumeSet};
    std::uint32_t split_part_index{0};
    std::uint32_t split_part_count{0};
    std::uint64_t split_size_bytes{0};
    std::uint32_t capability_flags{0};
};

struct ArchiveRecordPrefix final {
    std::uint16_t prefix_version{kRecordPrefixVersion};
    std::uint16_t record_kind{0};
    std::uint32_t header_size{0};
    std::uint64_t body_size{0};
    std::uint32_t flags{0};
};

struct MetadataEnvelopeHeader final {
    std::uint32_t flags{kCborMetadataFlagEncrypted};
    MetadataEncryptionMethod encryption_method{MetadataEncryptionMethod::kXChaCha20Poly1305};
    MetadataKdfMethod kdf_method{MetadataKdfMethod::kArgon2Id};
    std::uint8_t nonce_size{24};
    std::uint8_t tag_size{16};
    std::uint32_t key_slot_count{0};
    std::uint64_t plaintext_size{0};
    std::uint64_t ciphertext_size{0};
    std::array<std::byte, 32> salt{};
    std::array<std::byte, 24> nonce{};
    std::uint64_t kdf_opslimit{0};
    std::uint64_t kdf_memlimit_bytes{0};
    std::uint32_t kdf_parameters_version{1};
};

// Volume chunk kind-specific header (96 B). Also used as ChunkHeader by volume adapters.
struct ChunkHeader final {
    std::uint64_t chunk_index{0};
    std::uint8_t source_type{kSourceTypeVolume};
    std::uint32_t source_index{0};
    std::uint32_t block_entry_count{0};
    std::uint64_t payload_size{0};
    std::uint32_t flags{0};
    std::uint32_t header_crc32{0};
    std::array<std::byte, 24> payload_nonce{};
    std::array<std::byte, 16> payload_authentication_tag{};
};

using VolumeChunkHeader = ChunkHeader;

struct FileStreamChunkHeader final {
    std::uint64_t chunk_index{0};
    std::uint8_t source_type{kSourceTypeFileStream};
    std::uint32_t source_index{0};
    std::uint32_t block_entry_count{0};
    std::uint64_t payload_size{0};
    std::uint32_t chunk_flags{0};
    std::array<std::byte, 24> payload_nonce{};
    std::array<std::byte, 16> payload_authentication_tag{};
};

struct FileIndexPageHeader final {
    std::uint16_t page_format_version{kFileIndexPageFormatVersion};
    std::uint16_t page_kind{kIndexPageLeaf};
    std::uint64_t page_id{0};
    std::uint32_t plain_size{0};
    std::uint32_t encoded_size{0};
    std::uint8_t protection_mode{kIndexProtectAead};
    std::array<std::byte, 24> nonce{};
    std::array<std::byte, 16> authentication_tag{};
    std::array<std::byte, 32> content_digest{};
    std::uint32_t entry_count{0};
};

struct BlockEntry final {
    std::uint64_t logical_block_index{0};
    std::uint64_t data_offset_or_reference{0};
    std::uint32_t stored_size{0};
    std::uint32_t logical_size{0};
    std::uint8_t flags{kBlockFlagRaw};
};

struct BackupFooter final {
    std::uint64_t volume_chunk_count{0};
    std::uint64_t file_stream_chunk_count{0};
    std::uint64_t index_page_count{0};
    std::uint64_t total_block_entry_count{0};
    std::uint64_t total_payload_size{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t entry_count{0};
    std::uint64_t stream_count{0};
    std::uint32_t index_root_part_index{0};
    std::uint64_t index_root_offset{0};
    std::uint64_t index_root_page_id{0};
    std::array<std::byte, 32> index_root_digest{};
    std::uint64_t part_file_size{0};
    std::array<std::byte, 16> file_uuid{};
};

using EncodedBackupHeader = std::array<std::byte, kBackupHeaderSize>;
using EncodedArchiveRecordPrefix = std::array<std::byte, kArchiveRecordPrefixSize>;
using EncodedMetadataEnvelopeHeader = std::array<std::byte, kMetadataEnvelopeHeaderSize>;
using EncodedChunkHeader = std::array<std::byte, kChunkHeaderSize>;
using EncodedFileStreamChunkHeader = std::array<std::byte, kFileStreamChunkHeaderSize>;
using EncodedFileIndexPageHeader = std::array<std::byte, kFileIndexPageHeaderSize>;
using EncodedBlockEntry = std::array<std::byte, kBlockEntrySize>;
using EncodedBackupFooter = std::array<std::byte, kBackupFooterSize>;

[[nodiscard]] base::Result<EncodedBackupHeader> encode_backup_header(const BackupHeader& header);
[[nodiscard]] base::Result<BackupHeader> decode_backup_header(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedArchiveRecordPrefix>
encode_archive_record_prefix(const ArchiveRecordPrefix& prefix);
[[nodiscard]] base::Result<ArchiveRecordPrefix>
decode_archive_record_prefix(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedMetadataEnvelopeHeader>
encode_metadata_envelope_header(const MetadataEnvelopeHeader& header);
[[nodiscard]] base::Result<MetadataEnvelopeHeader>
decode_metadata_envelope_header(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedChunkHeader> encode_chunk_header(const ChunkHeader& header);
[[nodiscard]] base::Result<ChunkHeader> decode_chunk_header(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedFileStreamChunkHeader>
encode_file_stream_chunk_header(const FileStreamChunkHeader& header);
[[nodiscard]] base::Result<FileStreamChunkHeader>
decode_file_stream_chunk_header(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedFileIndexPageHeader>
encode_file_index_page_header(const FileIndexPageHeader& header);
[[nodiscard]] base::Result<FileIndexPageHeader>
decode_file_index_page_header(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedBlockEntry> encode_block_entry(const BlockEntry& entry);
[[nodiscard]] base::Result<BlockEntry> decode_block_entry(std::span<const std::byte> bytes);

[[nodiscard]] base::Result<EncodedBackupFooter> encode_backup_footer(const BackupFooter& footer);
[[nodiscard]] base::Result<BackupFooter> decode_backup_footer(std::span<const std::byte> bytes);

[[nodiscard]] ArchiveRecordPrefix make_volume_chunk_record_prefix(
    const std::uint64_t body_size) noexcept;
[[nodiscard]] ArchiveRecordPrefix make_file_stream_chunk_record_prefix(
    const std::uint64_t body_size) noexcept;
[[nodiscard]] ArchiveRecordPrefix make_file_index_page_record_prefix(
    const std::uint64_t body_size) noexcept;
[[nodiscard]] ArchiveRecordPrefix make_footer_record_prefix() noexcept;

} // namespace aegra::format::personal_archive
