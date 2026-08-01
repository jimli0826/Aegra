#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/error.h"
#include "aegra/format/manifest_codec.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;

struct ChunkRecord final {
    ports::ChunkDescriptor descriptor;
    std::uint64_t payload_offset{0};
    std::uint64_t stored_payload_size{0};
    std::uint32_t source_index{0};
    std::vector<archive::BlockEntry> entries;
};

struct ParsedPreamble final {
    archive::BackupHeader header;
    format::Manifest manifest;
};

struct EncodedMetadata final {
    archive::BackupHeader header;
    std::vector<std::byte> header_bytes;
    archive::MetadataEnvelopeHeader envelope;
    std::vector<std::byte> envelope_bytes;
    std::vector<std::byte> payload;
};

struct ScanResult final {
    std::vector<ChunkRecord> records;
};

struct ArchiveReadLimits final {
    std::uint64_t maximum_payload_size{0};
    std::uint64_t maximum_logical_size{0};
};

struct EntryLogicalRange final {
    std::uint64_t offset{0};
    std::uint64_t block_count{0};
    std::uint64_t total_blocks{0};
};

struct ScanStatistics final {
    std::uint64_t block_count{0};
    std::uint64_t payload_size{0};
};

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] char* as_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact(std::ifstream& input, const std::uint64_t offset, const std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "archive read size exceeds stream limit"));
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::byte> result(size);
    input.read(as_chars(result.data()), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kIoFailure, "personal archive is truncated"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] base::Result<std::uint64_t> read_stream_size(std::ifstream& input) {
    input.seekg(0, std::ios::end);
    const auto position = input.tellg();
    if (position < 0) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kIoFailure, "failed to determine personal archive size"));
    }
    input.seekg(0, std::ios::beg);
    return base::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(static_cast<std::streamoff>(position)));
}

[[nodiscard]] std::vector<std::byte> make_authenticated_data(std::span<const std::byte> header,
                                                             std::span<const std::byte> envelope) {
    std::vector<std::byte> result;
    result.reserve(header.size() + envelope.size());
    result.insert(result.end(), header.begin(), header.end());
    result.insert(result.end(), envelope.begin(), envelope.end());
    return result;
}

[[nodiscard]] crypto_sodium::ProtectedMetadata
make_protected_metadata(const archive::MetadataEnvelopeHeader& envelope,
                        std::span<const std::byte> ciphertext, std::span<const std::byte> tag) {
    crypto_sodium::ProtectedMetadata result;
    result.kdf = {envelope.kdf_opslimit, envelope.kdf_memlimit_bytes,
                  envelope.kdf_parameters_version};
    result.salt = envelope.salt;
    result.nonce = envelope.nonce;
    result.ciphertext.assign(ciphertext.begin(), ciphertext.end());
    std::copy(tag.begin(), tag.end(), result.tag.begin());
    return result;
}

[[nodiscard]] base::Result<EncodedMetadata> read_encoded_metadata(std::ifstream& input,
                                                                  const ArchiveOpenRequest& request,
                                                                  const std::uint64_t file_size) {
    auto header_bytes = read_exact(input, 0, archive::kBackupHeaderSize);
    if (!header_bytes) {
        return base::Result<EncodedMetadata>::failure(header_bytes.error());
    }
    auto header = archive::decode_backup_header(header_bytes.value());
    if (!header) {
        return base::Result<EncodedMetadata>::failure(header.error());
    }
    if (header.value().cbor_size > request.maximum_metadata_size +
                                       archive::kMetadataEnvelopeHeaderSize +
                                       crypto_sodium::kMetadataTagSize ||
        header.value().first_chunk_offset > file_size - archive::kBackupFooterSize) {
        return base::Result<EncodedMetadata>::failure(
            error(base::ErrorCode::kCorruptData, "archive metadata range exceeds limits"));
    }
    auto envelope_bytes =
        read_exact(input, header.value().cbor_offset, archive::kMetadataEnvelopeHeaderSize);
    if (!envelope_bytes) {
        return base::Result<EncodedMetadata>::failure(envelope_bytes.error());
    }
    auto envelope = archive::decode_metadata_envelope_header(envelope_bytes.value());
    if (!envelope) {
        return base::Result<EncodedMetadata>::failure(envelope.error());
    }
    if (envelope.value().ciphertext_size > request.maximum_metadata_size) {
        return base::Result<EncodedMetadata>::failure(
            error(base::ErrorCode::kCorruptData, "archive metadata exceeds configured limit"));
    }
    const auto expected_size = archive::kMetadataEnvelopeHeaderSize +
                               envelope.value().ciphertext_size + envelope.value().tag_size;
    if (expected_size != header.value().cbor_size) {
        return base::Result<EncodedMetadata>::failure(
            error(base::ErrorCode::kCorruptData, "archive metadata envelope size is invalid"));
    }
    auto payload = read_exact(
        input, header.value().cbor_offset + archive::kMetadataEnvelopeHeaderSize,
        static_cast<std::size_t>(envelope.value().ciphertext_size + envelope.value().tag_size));
    if (!payload) {
        return base::Result<EncodedMetadata>::failure(payload.error());
    }
    return base::Result<EncodedMetadata>::success(
        {header.value(), std::move(header_bytes).value(), envelope.value(),
         std::move(envelope_bytes).value(), std::move(payload).value()});
}

[[nodiscard]] base::Result<format::Manifest> decrypt_manifest(const EncodedMetadata& encoded,
                                                              const std::string_view password) {
    const auto ciphertext = std::span<const std::byte>(encoded.payload)
                                .first(static_cast<std::size_t>(encoded.envelope.ciphertext_size));
    const auto tag = std::span<const std::byte>(encoded.payload).last(encoded.envelope.tag_size);
    auto protected_metadata = make_protected_metadata(encoded.envelope, ciphertext, tag);
    const auto aad = make_authenticated_data(encoded.header_bytes, encoded.envelope_bytes);
    auto plaintext = crypto_sodium::unprotect_metadata(protected_metadata, password, aad);
    if (!plaintext) {
        return base::Result<format::Manifest>::failure(plaintext.error());
    }
    return format::decode_manifest_cbor(plaintext.value());
}

[[nodiscard]] base::Result<ParsedPreamble> read_preamble(std::ifstream& input,
                                                         const ArchiveOpenRequest& request,
                                                         const std::uint64_t file_size) {
    auto encoded = read_encoded_metadata(input, request, file_size);
    if (!encoded) {
        return base::Result<ParsedPreamble>::failure(encoded.error());
    }
    auto manifest = decrypt_manifest(encoded.value(), request.password);
    if (!manifest) {
        return base::Result<ParsedPreamble>::failure(manifest.error());
    }
    return base::Result<ParsedPreamble>::success(
        {encoded.value().header, std::move(manifest).value()});
}

[[nodiscard]] base::Result<archive::BackupFooter> read_footer(std::ifstream& input,
                                                              const std::uint64_t file_size) {
    if (file_size < archive::kBackupHeaderSize + archive::kMetadataEnvelopeHeaderSize +
                        archive::kBackupFooterSize) {
        return base::Result<archive::BackupFooter>::failure(
            error(base::ErrorCode::kCorruptData, "personal archive is too small"));
    }
    auto footer_bytes =
        read_exact(input, file_size - archive::kBackupFooterSize, archive::kBackupFooterSize);
    if (!footer_bytes) {
        return base::Result<archive::BackupFooter>::failure(footer_bytes.error());
    }
    auto footer = archive::decode_backup_footer(footer_bytes.value());
    if (!footer) {
        return footer;
    }
    if (footer.value().file_size != file_size) {
        return base::Result<archive::BackupFooter>::failure(
            error(base::ErrorCode::kCorruptData, "archive footer file size does not match"));
    }
    return footer;
}

[[nodiscard]] const format::Volume* find_volume(const format::Manifest& manifest,
                                                const std::uint32_t source_index) {
    const auto volume = std::find_if(manifest.volumes.begin(), manifest.volumes.end(),
                                     [source_index](const format::Volume& candidate) {
                                         return candidate.volume_index == source_index;
                                     });
    return volume == manifest.volumes.end() ? nullptr : &*volume;
}

[[nodiscard]] base::Result<std::vector<archive::BlockEntry>>
read_entries(std::ifstream& input, const std::uint64_t offset, const std::uint32_t count) {
    const auto byte_count = static_cast<std::uint64_t>(count) * archive::kBlockEntrySize;
    if (byte_count > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<std::vector<archive::BlockEntry>>::failure(
            error(base::ErrorCode::kCorruptData, "archive block index exceeds process limit"));
    }
    auto bytes = read_exact(input, offset, static_cast<std::size_t>(byte_count));
    if (!bytes) {
        return base::Result<std::vector<archive::BlockEntry>>::failure(bytes.error());
    }
    std::vector<archive::BlockEntry> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto entry = archive::decode_block_entry(
            std::span<const std::byte>(bytes.value())
                .subspan(index * archive::kBlockEntrySize, archive::kBlockEntrySize));
        if (!entry) {
            return base::Result<std::vector<archive::BlockEntry>>::failure(entry.error());
        }
        result.push_back(entry.value());
    }
    return base::Result<std::vector<archive::BlockEntry>>::success(std::move(result));
}

[[nodiscard]] base::Result<void> validate_entry_mapping(const archive::BlockEntry& entry,
                                                        const std::uint64_t expected_block,
                                                        const std::uint64_t stored_size,
                                                        const std::uint64_t payload_size) {
    if (entry.logical_block_index != expected_block) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive block index is not sequential"));
    }
    if (entry.flags == archive::kBlockFlagZero) {
        if (entry.data_offset_or_reference != 0) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "zero block contains a payload offset"));
        }
        return base::Result<void>::success();
    }
    if (entry.data_offset_or_reference != stored_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive payload offsets are not sequential"));
    }
    if (stored_size > payload_size || entry.stored_size > payload_size - stored_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive block payload range is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::uint64_t entry_block_count(const archive::BlockEntry& entry) noexcept {
    return entry.flags == archive::kBlockFlagZero ? entry.logical_size : 1;
}

[[nodiscard]] base::Result<EntryLogicalRange>
validate_entry_range(const archive::BlockEntry& entry, const std::uint32_t block_size,
                     const std::uint64_t volume_size) {
    if (volume_size == 0) {
        return base::Result<EntryLogicalRange>::failure(
            error(base::ErrorCode::kCorruptData, "archive volume size is invalid"));
    }
    const auto total_blocks = 1 + (volume_size - 1) / block_size;
    const auto block_count = entry_block_count(entry);
    if (entry.logical_block_index >= total_blocks ||
        block_count > total_blocks - entry.logical_block_index) {
        return base::Result<EntryLogicalRange>::failure(
            error(base::ErrorCode::kCorruptData, "archive block logical range is invalid"));
    }
    return base::Result<EntryLogicalRange>::success(
        {entry.logical_block_index * block_size, block_count, total_blocks});
}

[[nodiscard]] base::Result<std::uint64_t>
validate_stored_entry_size(const archive::BlockEntry& entry, const std::uint64_t logical_size) {
    if (entry.flags == archive::kBlockFlagRaw) {
        if (entry.logical_size != logical_size || entry.stored_size != logical_size) {
            return base::Result<std::uint64_t>::failure(
                error(base::ErrorCode::kCorruptData, "raw archive block size is invalid"));
        }
        return base::Result<std::uint64_t>::success(logical_size);
    }
    if (entry.flags == archive::kBlockFlagCompressed && entry.logical_size == entry.stored_size) {
        return base::Result<std::uint64_t>::success(logical_size);
    }
    return base::Result<std::uint64_t>::failure(
        error(base::ErrorCode::kCorruptData, "compressed archive block size is invalid"));
}

[[nodiscard]] base::Result<std::uint64_t> validate_entry_size(const archive::BlockEntry& entry,
                                                              const std::uint32_t block_size,
                                                              const std::uint64_t volume_size) {
    auto range = validate_entry_range(entry, block_size, volume_size);
    if (!range) {
        return base::Result<std::uint64_t>::failure(range.error());
    }
    if (entry.flags == archive::kBlockFlagZero) {
        const auto end_block = entry.logical_block_index + range.value().block_count;
        const auto logical_end =
            end_block == range.value().total_blocks ? volume_size : end_block * block_size;
        return base::Result<std::uint64_t>::success(logical_end - range.value().offset);
    }
    const auto logical_size =
        (std::min)(static_cast<std::uint64_t>(block_size), volume_size - range.value().offset);
    return validate_stored_entry_size(entry, logical_size);
}

[[nodiscard]] base::Result<void> validate_chunk_sequence(const ChunkRecord& record,
                                                         const ScanResult& scan) {
    if (record.descriptor.chunk_index != scan.records.size()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk index is not sequential"));
    }
    if (scan.records.empty()) {
        return base::Result<void>::success();
    }
    if (record.source_index != scan.records.front().source_index) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk source changes unexpectedly"));
    }
    const auto& previous = scan.records.back().descriptor;
    if (record.descriptor.logical_offset != previous.logical_offset + previous.logical_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunks have a logical gap"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> accumulate_statistics(const ChunkRecord& record,
                                                       ScanStatistics& statistics) {
    for (const auto& entry : record.entries) {
        const auto count = entry_block_count(entry);
        if (count > (std::numeric_limits<std::uint64_t>::max)() - statistics.block_count) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "archive block count overflows"));
        }
        statistics.block_count += count;
    }
    if (record.stored_payload_size >
        (std::numeric_limits<std::uint64_t>::max)() - statistics.payload_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive payload total overflows"));
    }
    statistics.payload_size += record.stored_payload_size;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_scan_footer(const ScanResult& scan,
                                                      const ScanStatistics& statistics,
                                                      const archive::BackupFooter& footer,
                                                      const std::uint64_t final_offset,
                                                      const std::uint64_t footer_offset) {
    if (final_offset != footer_offset || scan.records.size() != footer.chunk_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk count does not match footer"));
    }
    if (statistics.block_count != footer.total_block_count ||
        statistics.payload_size != footer.total_payload_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive statistics do not match footer"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ports::ChunkDescriptor>
validate_entries(const std::vector<archive::BlockEntry>& entries, const archive::ChunkHeader& chunk,
                 const std::uint32_t block_size, const std::uint64_t volume_size) {
    std::uint64_t logical_size = 0;
    std::uint64_t stored_size = 0;
    std::uint64_t expected_block = entries.front().logical_block_index;
    for (const auto& entry : entries) {
        auto mapping =
            validate_entry_mapping(entry, expected_block, stored_size, chunk.payload_size);
        auto entry_size = validate_entry_size(entry, block_size, volume_size);
        if (!mapping || !entry_size) {
            return base::Result<ports::ChunkDescriptor>::failure(!mapping ? mapping.error()
                                                                          : entry_size.error());
        }
        if (entry_size.value() > (std::numeric_limits<std::uint64_t>::max)() - logical_size) {
            return base::Result<ports::ChunkDescriptor>::failure(
                error(base::ErrorCode::kCorruptData, "archive chunk logical size overflows"));
        }
        logical_size += entry_size.value();
        stored_size += entry.stored_size;
        expected_block += entry_block_count(entry);
    }
    if (stored_size != chunk.payload_size) {
        return base::Result<ports::ChunkDescriptor>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk payload size is inconsistent"));
    }
    return base::Result<ports::ChunkDescriptor>::success(
        {chunk.chunk_index, entries.front().logical_block_index * block_size, logical_size,
         logical_size});
}

[[nodiscard]] base::Result<ChunkRecord>
read_chunk_record(std::ifstream& input, const std::uint64_t offset, const ParsedPreamble& preamble,
                  const std::uint64_t footer_offset, const ArchiveReadLimits& limits) {
    auto header_bytes = read_exact(input, offset, archive::kChunkHeaderSize);
    if (!header_bytes) {
        return base::Result<ChunkRecord>::failure(header_bytes.error());
    }
    auto chunk = archive::decode_chunk_header(header_bytes.value());
    if (!chunk) {
        return base::Result<ChunkRecord>::failure(chunk.error());
    }
    if (chunk.value().payload_size > limits.maximum_payload_size) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk exceeds configured limit"));
    }
    const auto* volume = find_volume(preamble.manifest, chunk.value().source_index);
    if (volume == nullptr) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk references an unknown volume"));
    }
    const auto entries_offset = offset + archive::kChunkHeaderSize;
    auto entries = read_entries(input, entries_offset, chunk.value().block_entry_count);
    if (!entries) {
        return base::Result<ChunkRecord>::failure(entries.error());
    }
    const auto payload_offset =
        entries_offset +
        static_cast<std::uint64_t>(chunk.value().block_entry_count) * archive::kBlockEntrySize;
    if (payload_offset > footer_offset ||
        chunk.value().payload_size > footer_offset - payload_offset) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk extends beyond footer"));
    }
    auto descriptor = validate_entries(entries.value(), chunk.value(), preamble.header.block_size,
                                       volume->total_size);
    if (!descriptor) {
        return base::Result<ChunkRecord>::failure(descriptor.error());
    }
    if (descriptor.value().logical_size > limits.maximum_logical_size) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk logical size exceeds limit"));
    }
    return base::Result<ChunkRecord>::success(
        {descriptor.value(), payload_offset, chunk.value().payload_size, chunk.value().source_index,
         std::move(entries).value()});
}

[[nodiscard]] base::Result<ScanResult> scan_chunks(std::ifstream& input,
                                                   const ParsedPreamble& preamble,
                                                   const archive::BackupFooter& footer,
                                                   const std::uint64_t file_size,
                                                   const ArchiveReadLimits& limits) {
    ScanResult result;
    const auto footer_offset = file_size - archive::kBackupFooterSize;
    std::uint64_t offset = preamble.header.first_chunk_offset;
    ScanStatistics statistics;
    while (offset < footer_offset) {
        auto record = read_chunk_record(input, offset, preamble, footer_offset, limits);
        if (!record) {
            return base::Result<ScanResult>::failure(record.error());
        }
        auto sequence = validate_chunk_sequence(record.value(), result);
        auto accumulated = accumulate_statistics(record.value(), statistics);
        if (!sequence || !accumulated) {
            return base::Result<ScanResult>::failure(!sequence ? sequence.error()
                                                               : accumulated.error());
        }
        offset = record.value().payload_offset + record.value().stored_payload_size;
        result.records.push_back(std::move(record).value());
    }
    auto footer_validation =
        validate_scan_footer(result, statistics, footer, offset, footer_offset);
    if (!footer_validation) {
        return base::Result<ScanResult>::failure(footer_validation.error());
    }
    return base::Result<ScanResult>::success(std::move(result));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_entry_payload(std::ifstream& input, const ChunkRecord& record,
                   const archive::BlockEntry& entry, const std::uint32_t block_size,
                   const std::uint64_t logical_size) {
    auto entry_size = validate_entry_size(entry, block_size, logical_size);
    if (!entry_size) {
        return base::Result<std::vector<std::byte>>::failure(entry_size.error());
    }
    if (entry.flags == archive::kBlockFlagZero) {
        return base::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(static_cast<std::size_t>(entry_size.value()), std::byte{0}));
    }
    auto stored = read_exact(input, record.payload_offset + entry.data_offset_or_reference,
                             entry.stored_size);
    if (!stored) {
        return base::Result<std::vector<std::byte>>::failure(stored.error());
    }
    if (entry.flags == archive::kBlockFlagRaw) {
        return stored;
    }
    return compression_zstd::decompress(stored.value(),
                                        static_cast<std::size_t>(entry_size.value()), block_size);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_record_payload(const std::filesystem::path& path, const ChunkRecord& record,
                    const std::uint32_t block_size, const std::uint64_t logical_size,
                    const base::CancellationToken& cancellation) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to reopen personal archive"));
    }
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>(record.descriptor.logical_size));
    for (const auto& entry : record.entries) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<std::byte>>::failure(
                error(base::ErrorCode::kCancelled, "restore cancelled"));
        }
        auto payload = read_entry_payload(input, record, entry, block_size, logical_size);
        if (!payload) {
            return base::Result<std::vector<std::byte>>::failure(payload.error());
        }
        result.insert(result.end(), payload.value().begin(), payload.value().end());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

} // namespace

struct PersonalArchiveReader::Impl final {
    std::filesystem::path source;
    format::Manifest manifest;
    std::uint32_t block_size{0};
    std::uint64_t logical_size{0};
    std::vector<ChunkRecord> records;
};

PersonalArchiveReader::PersonalArchiveReader(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveReader::~PersonalArchiveReader() = default;

base::Result<std::unique_ptr<PersonalArchiveReader>>
PersonalArchiveReader::open(const ArchiveOpenRequest& request) {
    if (request.source.empty() || request.password.empty() || request.maximum_metadata_size == 0 ||
        request.maximum_chunk_payload_size == 0 || request.maximum_chunk_logical_size == 0) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive open request is invalid"));
    }
    std::ifstream input(request.source, std::ios::binary);
    if (!input) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to open personal archive"));
    }
    auto file_size = read_stream_size(input);
    if (!file_size) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(file_size.error());
    }
    auto footer = read_footer(input, file_size.value());
    if (!footer) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(footer.error());
    }
    auto preamble = read_preamble(input, request, file_size.value());
    if (!preamble) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(preamble.error());
    }
    const ArchiveReadLimits limits{request.maximum_chunk_payload_size,
                                   request.maximum_chunk_logical_size};
    auto scan = scan_chunks(input, preamble.value(), footer.value(), file_size.value(), limits);
    if (!scan) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(scan.error());
    }
    if (preamble.value().manifest.volumes.size() != 1 || scan.value().records.empty() ||
        scan.value().records.front().source_index !=
            preamble.value().manifest.volumes.front().volume_index ||
        scan.value().records.back().descriptor.logical_offset +
                scan.value().records.back().descriptor.logical_size !=
            preamble.value().manifest.volumes.front().total_size) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(
            error(base::ErrorCode::kCorruptData, "reader currently requires one non-empty volume"));
    }
    auto parsed = std::move(preamble).value();
    auto scanned = std::move(scan).value();
    auto implementation = std::make_unique<Impl>();
    implementation->source = request.source;
    implementation->manifest = std::move(parsed.manifest);
    implementation->block_size = parsed.header.block_size;
    implementation->logical_size = implementation->manifest.volumes.front().total_size;
    implementation->records = std::move(scanned.records);
    return base::Result<std::unique_ptr<PersonalArchiveReader>>::success(
        std::unique_ptr<PersonalArchiveReader>(
            new PersonalArchiveReader(std::move(implementation))));
}

const format::Manifest& PersonalArchiveReader::manifest() const noexcept {
    return implementation_->manifest;
}

std::uint64_t PersonalArchiveReader::logical_size_bytes() const noexcept {
    return implementation_->logical_size;
}

std::uint64_t PersonalArchiveReader::chunk_count() const noexcept {
    return implementation_->records.size();
}

base::Result<ports::ChunkDescriptor>
PersonalArchiveReader::describe_chunk(const std::uint64_t chunk_index) const {
    if (chunk_index >= implementation_->records.size()) {
        return base::Result<ports::ChunkDescriptor>::failure(
            error(base::ErrorCode::kNotFound, "archive chunk does not exist"));
    }
    return base::Result<ports::ChunkDescriptor>::success(
        implementation_->records[static_cast<std::size_t>(chunk_index)].descriptor);
}

base::Result<ports::ChunkData>
PersonalArchiveReader::read_chunk(const std::uint64_t chunk_index,
                                  const base::CancellationToken cancellation) {
    auto descriptor = describe_chunk(chunk_index);
    if (!descriptor) {
        return base::Result<ports::ChunkData>::failure(descriptor.error());
    }
    const auto& record = implementation_->records[static_cast<std::size_t>(chunk_index)];
    auto payload = read_record_payload(implementation_->source, record, implementation_->block_size,
                                       implementation_->logical_size, cancellation);
    if (!payload) {
        return base::Result<ports::ChunkData>::failure(payload.error());
    }
    return base::Result<ports::ChunkData>::success(
        {descriptor.value(), std::move(payload).value()});
}

} // namespace aegra::adapters::personal_archive
