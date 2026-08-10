#pragma once

#include "personal_archive_block_worker_pool.h"

#include "aegra/base/result.h"
#include "aegra/format/personal_archive.h"
#include "aegra/format/personal_archive_sidecar.h"
#include "aegra/ports/backup_session.h"

#include <cstdint>
#include <span>
#include <vector>

namespace aegra::adapters::personal_archive::detail {

struct PreparedArchiveChunk final {
    format::personal_archive::ChunkHeader header;
    std::vector<format::personal_archive::BlockEntry> entries;
    std::vector<std::byte> payload;
};

struct PreparedArchiveInput final {
    std::vector<PreparedArchiveChunk> chunks;
    std::vector<format::personal_archive::SidecarRecord> sidecar_records;
    /// DEDUP entry count produced by this preparation (ADR-0022).
    std::uint64_t deduplicated_block_count{0};
    /// Expanded plaintext bytes represented by those DEDUP entries.
    std::uint64_t deduplicated_logical_bytes{0};
    /// CPU time summed across pool workers (not wall time).
    std::uint64_t hash_microseconds{0};
    std::uint64_t compress_microseconds{0};
};

struct ChunkPreparationRequest final {
    const ports::ChunkWriteRequest& input;
    std::span<const format::personal_archive::SidecarRecord> baseline;
    std::uint32_t block_size{0};
    std::uint32_t source_index{0};
    std::uint64_t first_archive_chunk_index{0};
    bool incremental{false};
    /// volume_set single-chunk DEDUP (ADR-0022); never true for file_set.
    bool deduplication_enabled{false};
    /// Session-owned pool; required for non-empty non-all-zero preparation.
    BlockWorkerPool* worker_pool{nullptr};
};

[[nodiscard]] base::Result<PreparedArchiveInput>
prepare_archive_chunks(const ChunkPreparationRequest& request);

} // namespace aegra::adapters::personal_archive::detail
