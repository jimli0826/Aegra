#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_block_worker_pool.h"
#include "personal_archive_payload.h"
#include "personal_archive_preamble.h"
#include "personal_archive_shape_validation.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/error.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;
using detail::archive_backup_type;
using detail::ParsedPreamble;

struct ChunkRecord final {
    archive::EncodedBackupHeader part_header{};
    archive::ChunkHeader header;
    ports::ChunkDescriptor descriptor;
    std::uint64_t payload_offset{0};
    std::uint64_t stored_payload_size{0};
    std::uint32_t source_index{0};
    std::vector<archive::BlockEntry> entries;
    std::filesystem::path part_path;
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
    std::uint64_t payload_size{0};
    std::uint64_t deduplicated_block_count{0};
    std::uint64_t deduplicated_logical_bytes{0};
};

struct PartReadRange final {
    std::filesystem::path path;
    std::uint64_t end_offset{0};
};

struct ArchiveScanState final {
    const ParsedPreamble& preamble;
    const ArchiveReadLimits& limits;
    ScanResult result;
    ScanStatistics statistics;
};

struct ScannedPart final {
    bool has_footer{false};
    archive::BackupFooter footer;
};

struct EntryOutputRange final {
    std::size_t offset{0};
    std::size_t size{0};
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
    if (size == 0) {
        return base::Result<std::vector<std::byte>>::success({});
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

[[nodiscard]] base::Result<std::optional<archive::BackupFooter>>
try_read_footer(std::ifstream& input, const std::uint64_t file_size) {
    if (file_size < archive::kBackupFooterSize) {
        return base::Result<std::optional<archive::BackupFooter>>::success(std::nullopt);
    }
    auto footer_bytes =
        read_exact(input, file_size - archive::kBackupFooterSize, archive::kBackupFooterSize);
    if (!footer_bytes) {
        return base::Result<std::optional<archive::BackupFooter>>::failure(footer_bytes.error());
    }
    auto footer = archive::decode_backup_footer(footer_bytes.value());
    if (!footer) {
        return base::Result<std::optional<archive::BackupFooter>>::success(std::nullopt);
    }
    if (footer.value().part_file_size != file_size) {
        return base::Result<std::optional<archive::BackupFooter>>::failure(
            error(base::ErrorCode::kCorruptData, "archive footer file size does not match"));
    }
    return base::Result<std::optional<archive::BackupFooter>>::success(footer.value());
}

[[nodiscard]] std::filesystem::path archive_part_path(const std::filesystem::path& primary_path,
                                                      const std::uint32_t part_index) {
    if (part_index == 0) {
        return primary_path;
    }
    std::ostringstream suffix;
    suffix << '.' << std::setw(3) << std::setfill('0') << part_index;
    auto result = primary_path;
    result += suffix.str();
    return result;
}

[[nodiscard]] base::Result<void> validate_part_header(const archive::BackupHeader& primary,
                                                      const archive::BackupHeader& part,
                                                      const std::uint32_t expected_index) {
    if (part.split_part_index != expected_index || part.file_uuid != primary.file_uuid ||
        part.backup_set_uuid != primary.backup_set_uuid ||
        part.parent_uuid != primary.parent_uuid) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive split part identity is invalid"));
    }
    if (part.flags != primary.flags || part.block_size != primary.block_size ||
        part.default_chunk_size != primary.default_chunk_size ||
        part.compression_method != primary.compression_method ||
        part.encryption_method != primary.encryption_method ||
        part.content_kind != primary.content_kind ||
        part.capability_flags != primary.capability_flags ||
        part.split_size_bytes != primary.split_size_bytes ||
        part.split_part_count != primary.split_part_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive split part settings do not match"));
    }
    return base::Result<void>::success();
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
    if (entry.flags == archive::kBlockFlagZero || entry.flags == archive::kBlockFlagFree ||
        entry.flags == archive::kBlockFlagDedup) {
        // ZERO/FREE/DEDUP carry no payload; DEDUP ref_index is validated separately.
        if (entry.flags != archive::kBlockFlagDedup && entry.data_offset_or_reference != 0) {
            return base::Result<void>::failure(error(
                base::ErrorCode::kCorruptData, "zero or free block contains a payload offset"));
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
    return entry.flags == archive::kBlockFlagZero || entry.flags == archive::kBlockFlagFree
               ? entry.logical_size
               : 1;
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

[[nodiscard]] base::Result<std::uint64_t> geometric_entry_bytes(const archive::BlockEntry& entry,
                                                                const std::uint32_t block_size,
                                                                const std::uint64_t volume_size) {
    auto range = validate_entry_range(entry, block_size, volume_size);
    if (!range) {
        return base::Result<std::uint64_t>::failure(range.error());
    }
    if (entry.flags == archive::kBlockFlagZero || entry.flags == archive::kBlockFlagFree) {
        const auto end_block = entry.logical_block_index + range.value().block_count;
        const auto logical_end =
            end_block == range.value().total_blocks ? volume_size : end_block * block_size;
        return base::Result<std::uint64_t>::success(logical_end - range.value().offset);
    }
    return base::Result<std::uint64_t>::success(
        (std::min)(static_cast<std::uint64_t>(block_size), volume_size - range.value().offset));
}

[[nodiscard]] base::Result<std::uint64_t> validate_entry_size(const archive::BlockEntry& entry,
                                                              const std::uint32_t block_size,
                                                              const std::uint64_t volume_size) {
    auto logical_size = geometric_entry_bytes(entry, block_size, volume_size);
    if (!logical_size) {
        return logical_size;
    }
    if (entry.flags == archive::kBlockFlagZero || entry.flags == archive::kBlockFlagFree ||
        entry.flags == archive::kBlockFlagDedup) {
        return logical_size;
    }
    return validate_stored_entry_size(entry, logical_size.value());
}

[[nodiscard]] base::Result<void>
validate_dedup_reference(const std::vector<archive::BlockEntry>& entries,
                         const std::size_t entry_index, const std::uint32_t block_size,
                         const std::uint64_t volume_size, const bool header_dedup_enabled) {
    const auto& entry = entries[entry_index];
    if (!header_dedup_enabled) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "dedup entry without header dedup flag"));
    }
    const auto ref_index = entry.data_offset_or_reference;
    if (ref_index >= entry_index) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "dedup forward or self reference"));
    }
    if (ref_index >= entries.size()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "dedup reference out of range"));
    }
    const auto& target = entries[static_cast<std::size_t>(ref_index)];
    if (!archive::is_canonical_dedup_target(target)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "dedup target is not raw or compressed"));
    }
    auto self_size = geometric_entry_bytes(entry, block_size, volume_size);
    auto target_size = geometric_entry_bytes(target, block_size, volume_size);
    if (!self_size || !target_size) {
        return base::Result<void>::failure(!self_size ? self_size.error() : target_size.error());
    }
    if (self_size.value() != target_size.value()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "dedup target length mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_chunk_sequence(const ChunkRecord& record, const ScanResult& scan, const bool sparse) {
    if (record.descriptor.chunk_index != scan.records.size()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk index is not sequential"));
    }
    if (scan.records.empty()) {
        return base::Result<void>::success();
    }
    const auto& previous_record = scan.records.back();
    if (record.source_index != previous_record.source_index) {
        // Sources must appear as contiguous runs (no interleaving). Each source has its own
        // logical address space that restarts at 0; a later volume may begin mid-volume on
        // sparse (incremental/differential) layers when leading blocks are unchanged.
        const bool source_seen = std::ranges::any_of(scan.records, [&record](const auto& prior) {
            return prior.source_index == record.source_index;
        });
        if (source_seen) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "archive chunk sources are interleaved"));
        }
        if (!sparse && record.descriptor.logical_offset != 0) {
            return base::Result<void>::failure(error(
                base::ErrorCode::kCorruptData, "archive volume data does not start at offset 0"));
        }
        return base::Result<void>::success();
    }
    const auto& previous = previous_record.descriptor;
    const auto previous_end = previous.logical_offset + previous.logical_size;
    if (record.descriptor.logical_offset < previous_end ||
        (!sparse && record.descriptor.logical_offset != previous_end)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunks overlap or have an invalid gap"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> accumulate_statistics(const ChunkRecord& record,
                                                       ScanStatistics& statistics) {
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
                                                      const ParsedPreamble& preamble) {
    if (scan.records.size() != footer.volume_chunk_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk count does not match footer"));
    }
    std::uint64_t expected_blocks = 0;
    for (const auto& volume : preamble.manifest.volumes) {
        const auto volume_blocks = 1 + (volume.total_size - 1) / preamble.header.block_size;
        if (volume_blocks > (std::numeric_limits<std::uint64_t>::max)() - expected_blocks) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "archive block count overflows"));
        }
        expected_blocks += volume_blocks;
    }
    if (expected_blocks != footer.total_block_entry_count ||
        statistics.payload_size != footer.total_payload_size ||
        footer.file_stream_chunk_count != 0 || footer.index_page_count != 0 ||
        footer.entry_count != 0 || footer.stream_count != 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive statistics do not match footer"));
    }
    const bool header_dedup = (preamble.header.flags & archive::kBackupFlagDedup) != 0;
    if (!header_dedup &&
        (footer.deduplicated_block_count != 0 || footer.deduplicated_logical_bytes != 0)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "footer dedup metrics without header flag"));
    }
    if (statistics.deduplicated_block_count != footer.deduplicated_block_count ||
        statistics.deduplicated_logical_bytes != footer.deduplicated_logical_bytes) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive dedup metrics do not match footer"));
    }
    if (footer.file_uuid != preamble.header.file_uuid) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive footer file UUID does not match"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ports::ChunkDescriptor>
validate_entries(const std::vector<archive::BlockEntry>& entries, const archive::ChunkHeader& chunk,
                 const std::uint32_t block_size, const std::uint64_t volume_size,
                 const bool header_dedup_enabled, ScanStatistics& statistics) {
    std::uint64_t logical_size = 0;
    std::uint64_t stored_size = 0;
    std::vector<ports::ChunkFreeRange> free_ranges;
    std::uint64_t expected_block = entries.front().logical_block_index;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        auto mapping =
            validate_entry_mapping(entry, expected_block, stored_size, chunk.payload_size);
        auto entry_size = validate_entry_size(entry, block_size, volume_size);
        if (!mapping || !entry_size) {
            return base::Result<ports::ChunkDescriptor>::failure(!mapping ? mapping.error()
                                                                          : entry_size.error());
        }
        if (entry.flags == archive::kBlockFlagDedup) {
            auto dedup = validate_dedup_reference(entries, index, block_size, volume_size,
                                                  header_dedup_enabled);
            if (!dedup) {
                return base::Result<ports::ChunkDescriptor>::failure(dedup.error());
            }
            if (statistics.deduplicated_block_count ==
                    (std::numeric_limits<std::uint64_t>::max)() ||
                statistics.deduplicated_logical_bytes >
                    (std::numeric_limits<std::uint64_t>::max)() - entry_size.value()) {
                return base::Result<ports::ChunkDescriptor>::failure(
                    error(base::ErrorCode::kCorruptData, "archive dedup metrics overflow"));
            }
            ++statistics.deduplicated_block_count;
            statistics.deduplicated_logical_bytes += entry_size.value();
        }
        if (entry_size.value() > (std::numeric_limits<std::uint64_t>::max)() - logical_size) {
            return base::Result<ports::ChunkDescriptor>::failure(
                error(base::ErrorCode::kCorruptData, "archive chunk logical size overflows"));
        }
        if (entry.flags == archive::kBlockFlagFree) {
            free_ranges.push_back({logical_size, entry_size.value()});
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
         logical_size, 0, std::move(free_ranges)});
}

[[nodiscard]] base::Result<ChunkRecord>
read_chunk_record(std::ifstream& input, const std::uint64_t offset,
                  const archive::EncodedBackupHeader& part_header, const ParsedPreamble& preamble,
                  const PartReadRange& part, const ArchiveReadLimits& limits,
                  ScanStatistics& statistics) {
    auto prefix_bytes = read_exact(input, offset, archive::kArchiveRecordPrefixSize);
    if (!prefix_bytes) {
        return base::Result<ChunkRecord>::failure(prefix_bytes.error());
    }
    auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
    if (!prefix) {
        return base::Result<ChunkRecord>::failure(prefix.error());
    }
    if (prefix.value().record_kind != archive::kRecordKindVolumeChunk) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive record kind is not a volume chunk"));
    }
    if (preamble.header.content_kind != archive::kContentKindVolumeSet) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "volume chunk is incompatible with content kind"));
    }
    const auto kind_offset = offset + archive::kArchiveRecordPrefixSize;
    auto header_bytes = read_exact(input, kind_offset, archive::kChunkHeaderSize);
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
    const auto expected_body =
        static_cast<std::uint64_t>(chunk.value().block_entry_count) * archive::kBlockEntrySize +
        chunk.value().payload_size;
    if (prefix.value().body_size != expected_body) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive record body size is inconsistent"));
    }
    const auto* volume = find_volume(preamble.manifest, chunk.value().source_index);
    if (volume == nullptr) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk references an unknown volume"));
    }
    const auto entries_offset = kind_offset + archive::kChunkHeaderSize;
    auto entries = read_entries(input, entries_offset, chunk.value().block_entry_count);
    if (!entries) {
        return base::Result<ChunkRecord>::failure(entries.error());
    }
    const auto payload_offset =
        entries_offset +
        static_cast<std::uint64_t>(chunk.value().block_entry_count) * archive::kBlockEntrySize;
    if (payload_offset > part.end_offset ||
        chunk.value().payload_size > part.end_offset - payload_offset) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk extends beyond footer"));
    }
    const bool header_dedup = (preamble.header.flags & archive::kBackupFlagDedup) != 0;
    auto descriptor =
        validate_entries(entries.value(), chunk.value(), preamble.header.block_size,
                         volume->total_size, header_dedup, statistics);
    if (!descriptor) {
        return base::Result<ChunkRecord>::failure(descriptor.error());
    }
    if (descriptor.value().logical_size > limits.maximum_logical_size) {
        return base::Result<ChunkRecord>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk logical size exceeds limit"));
    }
    descriptor.value().source_index = chunk.value().source_index;
    return base::Result<ChunkRecord>::success(
        {part_header, chunk.value(), descriptor.value(), payload_offset, chunk.value().payload_size,
         chunk.value().source_index, std::move(entries).value(), part.path});
}

[[nodiscard]] base::Result<std::uint64_t>
append_chunk(std::ifstream& input, const std::uint64_t offset,
             const archive::EncodedBackupHeader& part_header, const PartReadRange& part,
             ArchiveScanState& state) {
    auto record = read_chunk_record(input, offset, part_header, state.preamble, part, state.limits,
                                    state.statistics);
    if (!record) {
        return base::Result<std::uint64_t>::failure(record.error());
    }
    const bool sparse = (state.preamble.header.flags & archive::kBackupFlagFull) == 0;
    auto sequence = validate_chunk_sequence(record.value(), state.result, sparse);
    if (!sequence) {
        return base::Result<std::uint64_t>::failure(sequence.error());
    }
    auto accumulated = accumulate_statistics(record.value(), state.statistics);
    if (!accumulated) {
        return base::Result<std::uint64_t>::failure(accumulated.error());
    }
    const auto next_offset = record.value().payload_offset + record.value().stored_payload_size;
    state.result.records.push_back(std::move(record).value());
    return base::Result<std::uint64_t>::success(next_offset);
}

[[nodiscard]] base::Result<void> scan_part(std::ifstream& input,
                                           const archive::BackupHeader& header,
                                           const archive::EncodedBackupHeader& part_header,
                                           const PartReadRange& part, ArchiveScanState& state) {
    auto offset = header.first_record_offset;
    while (offset < part.end_offset) {
        auto next_offset = append_chunk(input, offset, part_header, part, state);
        if (!next_offset) {
            return base::Result<void>::failure(next_offset.error());
        }
        offset = next_offset.value();
    }
    if (offset != part.end_offset) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive split part has incomplete chunks"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<archive::BackupHeader> read_part_header(std::ifstream& input) {
    auto bytes = read_exact(input, 0, archive::kBackupHeaderSize);
    if (!bytes) {
        return base::Result<archive::BackupHeader>::failure(bytes.error());
    }
    return archive::decode_backup_header(bytes.value());
}

[[nodiscard]] base::ErrorCode missing_part_code(const std::uint32_t part_index) noexcept {
    return part_index == 0 ? base::ErrorCode::kIoFailure : base::ErrorCode::kCorruptData;
}

[[nodiscard]] std::uint64_t part_data_end(const std::uint64_t file_size,
                                          const bool has_footer) noexcept {
    return has_footer ? file_size - archive::kBackupFooterSize : file_size;
}

[[nodiscard]] base::Result<void> validate_part_has_data(const bool has_footer,
                                                        const std::size_t initial_count,
                                                        const std::size_t final_count) {
    if (!has_footer && initial_count == final_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "non-final archive part is empty"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ScannedPart> read_and_scan_part(const std::filesystem::path& path,
                                                           const std::uint32_t part_index,
                                                           ArchiveScanState& state) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return base::Result<ScannedPart>::failure(
            error(missing_part_code(part_index), "archive split part is missing"));
    }
    auto file_size = read_stream_size(input);
    if (!file_size) {
        return base::Result<ScannedPart>::failure(file_size.error());
    }
    auto header_bytes = read_exact(input, 0, archive::kBackupHeaderSize);
    if (!header_bytes) {
        return base::Result<ScannedPart>::failure(header_bytes.error());
    }
    auto header = archive::decode_backup_header(header_bytes.value());
    if (!header) {
        return base::Result<ScannedPart>::failure(header.error());
    }
    auto identity = validate_part_header(state.preamble.header, header.value(), part_index);
    if (!identity) {
        return base::Result<ScannedPart>::failure(identity.error());
    }
    archive::EncodedBackupHeader encoded_part_header{};
    std::copy_n(header_bytes.value().begin(), archive::kBackupHeaderSize,
                encoded_part_header.begin());
    auto footer = try_read_footer(input, file_size.value());
    if (!footer) {
        return base::Result<ScannedPart>::failure(footer.error());
    }
    const bool has_footer = footer.value().has_value();
    const auto end_offset = part_data_end(file_size.value(), has_footer);
    const auto initial_chunk_count = state.result.records.size();
    auto scanned =
        scan_part(input, header.value(), encoded_part_header, {path, end_offset}, state);
    if (!scanned) {
        return base::Result<ScannedPart>::failure(scanned.error());
    }
    auto has_data =
        validate_part_has_data(has_footer, initial_chunk_count, state.result.records.size());
    if (!has_data) {
        return base::Result<ScannedPart>::failure(has_data.error());
    }
    return base::Result<ScannedPart>::success(
        {has_footer, footer.value().value_or(archive::BackupFooter{})});
}

[[nodiscard]] base::Result<void> validate_non_final_part(const archive::BackupHeader& primary,
                                                         const std::uint32_t part_index) {
    if ((primary.flags & archive::kBackupFlagSplit) == 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "single-file archive has no footer"));
    }
    if (primary.split_part_count != 0 && part_index + 1 >= primary.split_part_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "declared final archive part has no footer"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_final_part(const std::filesystem::path& primary_path,
                                                     const std::uint32_t part_index,
                                                     const ScannedPart& part,
                                                     const ArchiveScanState& state) {
    const auto& primary = state.preamble.header;
    if (primary.split_part_count != 0 && part_index + 1 != primary.split_part_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive split part count does not match"));
    }
    std::error_code filesystem_error;
    const auto next_path = archive_part_path(primary_path, part_index + 1);
    const bool has_next = std::filesystem::exists(next_path, filesystem_error);
    if (filesystem_error) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to inspect the next archive split part"));
    }
    if ((primary.flags & archive::kBackupFlagSplit) != 0 && has_next) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive has data after the final part"));
    }
    return validate_scan_footer(state.result, state.statistics, part.footer, state.preamble);
}

[[nodiscard]] base::Result<ScanResult> scan_archive_parts(const std::filesystem::path& primary_path,
                                                          const ParsedPreamble& preamble,
                                                          const ArchiveOpenRequest& request) {
    const ArchiveReadLimits limits{request.maximum_chunk_payload_size,
                                   request.maximum_chunk_logical_size};
    ArchiveScanState state{preamble, limits, {}, {}};
    for (std::uint32_t part_index = 0; part_index < request.maximum_split_parts; ++part_index) {
        const auto path = archive_part_path(primary_path, part_index);
        auto part = read_and_scan_part(path, part_index, state);
        if (!part) {
            return base::Result<ScanResult>::failure(part.error());
        }
        if (!part.value().has_footer) {
            auto valid = validate_non_final_part(preamble.header, part_index);
            if (!valid) {
                return base::Result<ScanResult>::failure(valid.error());
            }
            continue;
        }
        auto final_part = validate_final_part(primary_path, part_index, part.value(), state);
        if (!final_part) {
            return base::Result<ScanResult>::failure(final_part.error());
        }
        return base::Result<ScanResult>::success(std::move(state.result));
    }
    return base::Result<ScanResult>::failure(
        error(base::ErrorCode::kCorruptData, "archive exceeds the split part limit"));
}

[[nodiscard]] base::Result<std::vector<EntryOutputRange>>
make_output_ranges(const ChunkRecord& record, const std::uint32_t block_size,
                   const std::uint64_t volume_size) {
    std::vector<EntryOutputRange> ranges;
    ranges.reserve(record.entries.size());
    std::size_t output_offset = 0;
    for (const auto& entry : record.entries) {
        auto entry_size = validate_entry_size(entry, block_size, volume_size);
        if (!entry_size) {
            return base::Result<std::vector<EntryOutputRange>>::failure(entry_size.error());
        }
        if (entry_size.value() > (std::numeric_limits<std::size_t>::max)() - output_offset) {
            return base::Result<std::vector<EntryOutputRange>>::failure(
                error(base::ErrorCode::kCorruptData, "archive chunk expanded size overflows"));
        }
        const auto size = static_cast<std::size_t>(entry_size.value());
        ranges.push_back({output_offset, size});
        output_offset += size;
    }
    if (output_offset != record.descriptor.logical_size) {
        return base::Result<std::vector<EntryOutputRange>>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk expanded size is invalid"));
    }
    return base::Result<std::vector<EntryOutputRange>>::success(std::move(ranges));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_authenticated_payload(const ChunkRecord& record,
                           const crypto_sodium::PayloadCipher* payload_cipher) {
    std::ifstream input(record.part_path, std::ios::binary);
    if (!input) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to reopen personal archive"));
    }
    auto ciphertext = read_exact(input, record.payload_offset,
                                 static_cast<std::size_t>(record.stored_payload_size));
    if (!ciphertext) {
        return base::Result<std::vector<std::byte>>::failure(ciphertext.error());
    }
    if (payload_cipher == nullptr) {
        return base::Result<std::vector<std::byte>>::success(std::move(ciphertext).value());
    }
    auto decrypted = detail::unprotect_archive_chunk(record.part_header, record.header,
                                                      record.entries, ciphertext.value(),
                                                      payload_cipher);
    if (!decrypted) {
        return base::Result<std::vector<std::byte>>::failure(decrypted.error());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(ciphertext).value());
}

[[nodiscard]] base::Result<void>
expand_canonical_entries(const ChunkRecord& record, const std::span<const std::byte> plaintext,
                         const std::span<const EntryOutputRange> ranges,
                         const std::uint32_t block_size, std::vector<std::byte>& result,
                         detail::BlockWorkerPool& workers,
                         const base::CancellationToken& cancellation) {
    return workers.parallel_for(
        record.entries.size(),
        [&](const std::size_t index, detail::BlockWorkerLocal& local) -> base::Result<void> {
            if (cancellation.stop_requested()) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kCancelled, "restore cancelled"));
            }
            const auto& entry = record.entries[index];
            if (entry.flags == archive::kBlockFlagZero || entry.flags == archive::kBlockFlagFree ||
                entry.flags == archive::kBlockFlagDedup) {
                return base::Result<void>::success();
            }
            const auto stored = plaintext.subspan(
                static_cast<std::size_t>(entry.data_offset_or_reference), entry.stored_size);
            const auto output =
                std::span<std::byte>(result).subspan(ranges[index].offset, ranges[index].size);
            if (entry.flags == archive::kBlockFlagRaw) {
                std::memcpy(output.data(), stored.data(), output.size());
                return base::Result<void>::success();
            }
            return local.decompressor.decompress_into(stored, output, block_size);
        });
}

[[nodiscard]] base::Result<void>
expand_dedup_entries(const ChunkRecord& record, const std::span<const EntryOutputRange> ranges,
                     std::vector<std::byte>& result) {
    for (std::size_t index = 0; index < record.entries.size(); ++index) {
        const auto& entry = record.entries[index];
        if (entry.flags != archive::kBlockFlagDedup) {
            continue;
        }
        const auto ref = static_cast<std::size_t>(entry.data_offset_or_reference);
        if (ref >= index || (record.entries[ref].flags != archive::kBlockFlagRaw &&
                             record.entries[ref].flags != archive::kBlockFlagCompressed) ||
            ranges[ref].size != ranges[index].size) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "dedup canonical is unavailable"));
        }
        std::memcpy(result.data() + ranges[index].offset, result.data() + ranges[ref].offset,
                    ranges[index].size);
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_record_payload(const ChunkRecord& record, const std::uint32_t block_size,
                    const std::uint64_t volume_size, const base::CancellationToken& cancellation,
                    const crypto_sodium::PayloadCipher* payload_cipher,
                    detail::BlockWorkerPool& workers) {
    if (cancellation.stop_requested()) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCancelled, "restore cancelled"));
    }
    // Authenticate AEAD before releasing any current-chunk data to the sink.
    auto plaintext = read_authenticated_payload(record, payload_cipher);
    if (!plaintext) {
        return base::Result<std::vector<std::byte>>::failure(plaintext.error());
    }
    if (record.descriptor.logical_size > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk logical size exceeds process limit"));
    }
    auto ranges = make_output_ranges(record, block_size, volume_size);
    if (!ranges) {
        return base::Result<std::vector<std::byte>>::failure(ranges.error());
    }
    std::vector<std::byte> result(static_cast<std::size_t>(record.descriptor.logical_size));
    auto canonicals = expand_canonical_entries(record, plaintext.value(), ranges.value(),
                                               block_size, result, workers, cancellation);
    if (!canonicals) {
        return base::Result<std::vector<std::byte>>::failure(canonicals.error());
    }
    auto deduplicated = expand_dedup_entries(record, ranges.value(), result);
    if (!deduplicated) {
        return base::Result<std::vector<std::byte>>::failure(deduplicated.error());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

} // namespace

struct PersonalArchiveReader::Impl final {
    explicit Impl(std::shared_ptr<detail::BlockWorkerPool> workers)
        : block_workers(std::move(workers)) {}

    format::Manifest manifest;
    ArchiveIdentity identity;
    std::uint32_t block_size{0};
    std::uint64_t logical_size{0};
    std::vector<ChunkRecord> records;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    std::shared_ptr<detail::BlockWorkerPool> block_workers;
};

PersonalArchiveReader::PersonalArchiveReader(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveReader::~PersonalArchiveReader() = default;

base::Result<std::unique_ptr<PersonalArchiveReader>>
PersonalArchiveReader::open(const ArchiveOpenRequest& request) {
    return open_with_workers(
        request,
        std::make_shared<detail::BlockWorkerPool>(detail::default_block_worker_count()));
}

base::Result<std::unique_ptr<PersonalArchiveReader>>
PersonalArchiveReader::open_with_workers(
    const ArchiveOpenRequest& request,
    std::shared_ptr<detail::BlockWorkerPool> block_workers) {
    constexpr std::uint32_t maximum_supported_split_parts = 100'000;
    if (block_workers == nullptr || request.source.empty() || request.maximum_metadata_size == 0 ||
        request.maximum_chunk_payload_size == 0 || request.maximum_chunk_logical_size == 0 ||
        request.maximum_split_parts == 0 ||
        request.maximum_split_parts > maximum_supported_split_parts) {
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
    auto preamble = detail::read_archive_preamble(input, request, file_size.value());
    if (!preamble) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(preamble.error());
    }
    auto scan = scan_archive_parts(request.source, preamble.value(), request);
    if (!scan) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(scan.error());
    }
    std::vector<detail::ArchiveChunkShape> chunk_shapes;
    chunk_shapes.reserve(scan.value().records.size());
    for (const auto& record : scan.value().records) {
        chunk_shapes.push_back({record.source_index, record.descriptor.logical_offset,
                                record.descriptor.logical_size});
    }
    auto shape = detail::validate_archive_layer_shape(
        archive_backup_type(preamble.value().header), preamble.value().manifest, chunk_shapes);
    if (!shape) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(shape.error());
    }
    const bool encrypted =
        (preamble.value().header.flags & format::personal_archive::kBackupFlagEncrypted) != 0;
    if (encrypted == request.password.empty()) {
        return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(error(
            base::ErrorCode::kUnauthorized,
            encrypted ? "encrypted archive requires a password" : "unencrypted archive has no password"));
    }
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    if (encrypted) {
        auto cipher = crypto_sodium::PayloadCipher::create(request.password, preamble.value().kdf,
                                                           preamble.value().salt);
        if (!cipher) {
            return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(cipher.error());
        }
        payload_cipher = std::move(cipher).value();
    }
    auto parsed = std::move(preamble).value();
    auto scanned = std::move(scan).value();
    auto implementation = std::make_unique<Impl>(std::move(block_workers));
    implementation->manifest = std::move(parsed.manifest);
    implementation->identity = {parsed.header.file_uuid, parsed.header.backup_set_uuid,
                                parsed.header.parent_uuid, archive_backup_type(parsed.header),
                                parsed.header.block_size};
    implementation->block_size = parsed.header.block_size;
    for (const auto& volume : implementation->manifest.volumes) {
        if (volume.total_size >
            (std::numeric_limits<std::uint64_t>::max)() - implementation->logical_size) {
            return base::Result<std::unique_ptr<PersonalArchiveReader>>::failure(
                error(base::ErrorCode::kCorruptData, "archive logical size overflows"));
        }
        implementation->logical_size += volume.total_size;
    }
    implementation->records = std::move(scanned.records);
    implementation->payload_cipher = std::move(payload_cipher);
    return base::Result<std::unique_ptr<PersonalArchiveReader>>::success(
        std::unique_ptr<PersonalArchiveReader>(
            new PersonalArchiveReader(std::move(implementation))));
}

const format::Manifest& PersonalArchiveReader::manifest() const noexcept {
    return implementation_->manifest;
}

const ArchiveIdentity& PersonalArchiveReader::identity() const noexcept {
    return implementation_->identity;
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
    const auto* volume = find_volume(implementation_->manifest, record.source_index);
    if (volume == nullptr) {
        return base::Result<ports::ChunkData>::failure(
            error(base::ErrorCode::kCorruptData, "archive chunk references an unknown volume"));
    }
    auto payload =
        read_record_payload(record, implementation_->block_size, volume->total_size, cancellation,
                            implementation_->payload_cipher.get(),
                            *implementation_->block_workers);
    if (!payload) {
        return base::Result<ports::ChunkData>::failure(payload.error());
    }
    return base::Result<ports::ChunkData>::success(
        {descriptor.value(), std::move(payload).value()});
}

} // namespace aegra::adapters::personal_archive
