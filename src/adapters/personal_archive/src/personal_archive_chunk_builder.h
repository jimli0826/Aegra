#pragma once

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
};

struct ChunkPreparationRequest final {
    const ports::ChunkWriteRequest& input;
    std::span<const format::personal_archive::SidecarRecord> baseline;
    std::uint32_t block_size{0};
    std::uint32_t source_index{0};
    std::uint64_t first_archive_chunk_index{0};
    bool incremental{false};
};

[[nodiscard]] base::Result<PreparedArchiveInput>
prepare_archive_chunks(const ChunkPreparationRequest& request);

} // namespace aegra::adapters::personal_archive::detail
