#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::format::personal_archive {

inline constexpr std::uint16_t kFormatVersion = 6;
inline constexpr std::uint16_t kHeaderVersion = 1;
inline constexpr std::uint16_t kEnvelopeVersion = 1;
inline constexpr std::uint16_t kFooterVersion = 1;
inline constexpr std::size_t kBackupHeaderSize = 256;
inline constexpr std::size_t kMetadataEnvelopeHeaderSize = 124;
inline constexpr std::size_t kChunkHeaderSize = 52;
inline constexpr std::size_t kBlockEntrySize = 25;
inline constexpr std::size_t kBackupFooterSize = 512;

inline constexpr std::uint32_t kBackupFlagFull = 0x00000001;
inline constexpr std::uint32_t kBackupFlagIncremental = 0x00000002;
inline constexpr std::uint32_t kBackupFlagDifferential = 0x00000004;
inline constexpr std::uint32_t kBackupFlagDedup = 0x00000008;
inline constexpr std::uint32_t kBackupFlagEncrypted = 0x00000010;
inline constexpr std::uint32_t kBackupFlagSplit = 0x00000020;
inline constexpr std::uint32_t kCborMetadataFlagEncrypted = 0x00000001;

inline constexpr std::uint8_t kBlockFlagRaw = 0x01;
inline constexpr std::uint8_t kBlockFlagCompressed = 0x02;
inline constexpr std::uint8_t kBlockFlagZero = 0x04;
inline constexpr std::uint8_t kBlockFlagDedup = 0x08;

enum class CompressionMethod : std::uint8_t {
    kNone = 0,
    kZstandard = 1,
};

enum class PayloadEncryptionMethod : std::uint8_t {
    kNone = 0,
    kAes256Xts = 1,
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
    std::uint64_t first_chunk_offset{0};
    std::uint32_t default_chunk_size{0};
    CompressionMethod compression_method{CompressionMethod::kNone};
    PayloadEncryptionMethod encryption_method{PayloadEncryptionMethod::kNone};
    std::uint32_t split_part_index{0};
    std::uint32_t split_part_count{0};
    std::uint64_t split_size_bytes{0};
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

struct ChunkHeader final {
    std::uint64_t chunk_index{0};
    std::uint8_t source_type{1};
    std::uint32_t source_index{0};
    std::uint32_t block_entry_count{0};
    std::uint64_t payload_size{0};
    std::uint32_t flags{0};
    std::uint32_t header_crc32{0};
};

struct BlockEntry final {
    std::uint64_t logical_block_index{0};
    std::uint64_t data_offset_or_reference{0};
    std::uint32_t stored_size{0};
    std::uint32_t logical_size{0};
    std::uint8_t flags{kBlockFlagRaw};
};

struct BackupFooter final {
    std::uint64_t chunk_count{0};
    std::uint64_t total_block_count{0};
    std::uint64_t total_payload_size{0};
    std::uint64_t file_size{0};
};

using EncodedBackupHeader = std::array<std::byte, kBackupHeaderSize>;
using EncodedMetadataEnvelopeHeader = std::array<std::byte, kMetadataEnvelopeHeaderSize>;
using EncodedChunkHeader = std::array<std::byte, kChunkHeaderSize>;
using EncodedBlockEntry = std::array<std::byte, kBlockEntrySize>;
using EncodedBackupFooter = std::array<std::byte, kBackupFooterSize>;

[[nodiscard]] base::Result<EncodedBackupHeader> encode_backup_header(const BackupHeader& header);
[[nodiscard]] base::Result<BackupHeader> decode_backup_header(std::span<const std::byte> bytes);
[[nodiscard]] base::Result<EncodedMetadataEnvelopeHeader>
encode_metadata_envelope_header(const MetadataEnvelopeHeader& header);
[[nodiscard]] base::Result<MetadataEnvelopeHeader>
decode_metadata_envelope_header(std::span<const std::byte> bytes);
[[nodiscard]] base::Result<EncodedChunkHeader> encode_chunk_header(const ChunkHeader& header);
[[nodiscard]] base::Result<ChunkHeader> decode_chunk_header(std::span<const std::byte> bytes);
[[nodiscard]] base::Result<EncodedBlockEntry> encode_block_entry(const BlockEntry& entry);
[[nodiscard]] base::Result<BlockEntry> decode_block_entry(std::span<const std::byte> bytes);
[[nodiscard]] base::Result<EncodedBackupFooter> encode_backup_footer(const BackupFooter& footer);
[[nodiscard]] base::Result<BackupFooter> decode_backup_footer(std::span<const std::byte> bytes);

} // namespace aegra::format::personal_archive
