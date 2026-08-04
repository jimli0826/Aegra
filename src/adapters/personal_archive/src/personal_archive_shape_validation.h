#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <cstdint>
#include <span>

namespace aegra::adapters::personal_archive::detail {

struct ArchiveChunkShape final {
    std::uint32_t source_index{0};
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
};

[[nodiscard]] base::Result<void>
validate_archive_layer_shape(format::BackupType backup_type, const format::Manifest& manifest,
                             std::span<const ArchiveChunkShape> chunks);

} // namespace aegra::adapters::personal_archive::detail
