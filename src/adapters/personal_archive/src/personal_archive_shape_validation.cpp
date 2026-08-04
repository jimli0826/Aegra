#include "personal_archive_shape_validation.h"

#include "aegra/base/error.h"

#include <string>
#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

[[nodiscard]] base::Result<void> corrupt(std::string message) {
    return base::Result<void>::failure({base::ErrorCode::kCorruptData, std::move(message)});
}

} // namespace

base::Result<void> validate_archive_layer_shape(const format::BackupType backup_type,
                                                const format::Manifest& manifest,
                                                const std::span<const ArchiveChunkShape> chunks) {
    if (chunks.empty()) {
        return backup_type == format::BackupType::kFull
                   ? corrupt("full archive contains no chunks")
                   : base::Result<void>::success();
    }
    if (backup_type != format::BackupType::kFull) {
        return base::Result<void>::success();
    }
    std::size_t position = 0;
    for (const auto& volume : manifest.volumes) {
        const auto first_position = position;
        while (position < chunks.size() && chunks[position].source_index == volume.volume_index) {
            ++position;
        }
        if (first_position == position) {
            return corrupt("full archive omits a volume");
        }
        const auto& first = chunks[first_position];
        const auto& last = chunks[position - 1];
        if (first.logical_offset != 0 ||
            last.logical_offset + last.logical_size != volume.total_size) {
            return corrupt("full archive does not cover a volume");
        }
    }
    return position == chunks.size() ? base::Result<void>::success()
                                     : corrupt("archive contains unknown volume data");
}

} // namespace aegra::adapters::personal_archive::detail
