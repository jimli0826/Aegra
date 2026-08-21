#include "aegra/ntfs_resize/boot_sector_invalidation.h"

#include <algorithm>
#include <vector>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] base::Result<void>
overwrite_and_verify(ports::IRandomAccessBlockDevice& target, const std::uint64_t offset,
                     const std::span<const std::byte> pattern,
                     const base::CancellationToken cancellation) {
    auto written = target.write_at(offset, pattern, cancellation);
    if (!written) {
        return written;
    }
    auto flushed = target.flush(cancellation);
    if (!flushed) {
        return flushed;
    }
    std::vector<std::byte> readback(pattern.size());
    auto read = target.read_at(offset, readback, cancellation);
    if (!read) {
        return base::Result<void>::failure(read.error());
    }
    if (read.value() != pattern.size() ||
        !std::equal(pattern.begin(), pattern.end(), readback.begin())) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "ntfs_resize.boot_invalidation_readback_failed"});
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> invalidate_ntfs_boot_sectors(ports::IRandomAccessBlockDevice& target,
                                                const std::uint32_t bytes_per_sector,
                                                const base::CancellationToken cancellation) {
    if (bytes_per_sector == 0 || (bytes_per_sector & (bytes_per_sector - 1U)) != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.boot_sector_size_invalid"});
    }
    const auto geometry = target.geometry();
    if (geometry.capacity_bytes < static_cast<std::uint64_t>(bytes_per_sector) * 2U) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInsufficientSpace, "ntfs_resize.boot_invalidation_target_too_small"});
    }
    if (geometry.capacity_bytes % bytes_per_sector != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.boot_sector_alignment_invalid"});
    }
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "ntfs_resize.boot_invalidation_cancelled"});
    }

    std::vector<std::byte> poison(bytes_per_sector, std::byte{0xFF});
    // Destroy NTFS OEM ID / jump so Mount Manager will not treat the volume as NTFS.
    poison[0] = std::byte{0x00};
    poison[1] = std::byte{0x00};
    poison[2] = std::byte{0x00};
    constexpr char kNotNtfs[] = {'N', 'O', 'T', 'N', 'T', 'F', 'S', '!'};
    for (std::size_t i = 0; i < sizeof(kNotNtfs); ++i) {
        poison[3 + i] = static_cast<std::byte>(kNotNtfs[i]);
    }

    const base::CancellationToken deferred_cancellation{};
    auto primary = overwrite_and_verify(target, 0, poison, deferred_cancellation);
    if (!primary) {
        return primary;
    }
    const auto backup_offset = geometry.capacity_bytes - bytes_per_sector;
    return overwrite_and_verify(target, backup_offset, poison, deferred_cancellation);
}

} // namespace aegra::ntfs_resize
