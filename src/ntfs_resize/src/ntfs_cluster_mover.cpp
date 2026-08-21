#include "ntfs_cluster_mover.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/binary.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

constexpr std::uint64_t kMaxCopyChunkBytes = 1ULL * 1024ULL * 1024ULL;

[[nodiscard]] base::Result<void>
copy_chunk_verified(CompositeNtfsBlockDevice& device, const std::uint64_t source_offset,
                    const std::uint64_t target_offset, const std::size_t size,
                    const base::CancellationToken cancellation) {
    std::vector<std::byte> buffer(size);
    auto read = device.read_at(source_offset, buffer, cancellation);
    if (!read) {
        return base::Result<void>::failure(read.error());
    }
    if (read.value() != size) {
        return shrink_fail_void(base::ErrorCode::kIoFailure, "restore.shrink_target_incomplete");
    }
    auto written = device.write_at(target_offset, buffer, cancellation);
    if (!written) {
        return written;
    }
    auto flushed = device.flush(cancellation);
    if (!flushed) {
        return flushed;
    }
    std::vector<std::byte> readback(size);
    auto verified = device.read_at(target_offset, readback, cancellation);
    if (!verified) {
        return base::Result<void>::failure(verified.error());
    }
    if (verified.value() != size ||
        !std::equal(buffer.begin(), buffer.end(), readback.begin())) {
        return shrink_fail_void(base::ErrorCode::kIoFailure, "restore.shrink_target_incomplete");
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<std::uint64_t>
move_relocation_extent(CompositeNtfsBlockDevice& device, const ntfs_core::BootGeometry& geometry,
                       const RelocationRecord& record, const base::CancellationToken cancellation) {
    if (record.cluster_count == 0 ||
        record.source.end_lcn <= record.source.begin_lcn ||
        record.target.end_lcn <= record.target.begin_lcn ||
        record.source.end_lcn - record.source.begin_lcn != record.cluster_count ||
        record.target.end_lcn - record.target.begin_lcn != record.cluster_count) {
        return shrink_fail<std::uint64_t>(base::ErrorCode::kInvalidArgument,
                                          "restore.shrink_plan_corrupt");
    }
    if (geometry.bytes_per_cluster == 0) {
        return shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                          "restore.shrink_unsupported_layout");
    }

    std::uint64_t source_offset = 0;
    std::uint64_t target_offset = 0;
    std::uint64_t total_bytes = 0;
    if (!ntfs_core::checked_mul_u64(record.source.begin_lcn, geometry.bytes_per_cluster,
                                    source_offset) ||
        !ntfs_core::checked_mul_u64(record.target.begin_lcn, geometry.bytes_per_cluster,
                                    target_offset) ||
        !ntfs_core::checked_mul_u64(record.cluster_count, geometry.bytes_per_cluster, total_bytes)) {
        return shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                          "restore.shrink_plan_corrupt");
    }

    const auto clusters_per_chunk =
        (std::max)(std::uint64_t{1}, kMaxCopyChunkBytes / geometry.bytes_per_cluster);
    const auto copy_chunk = static_cast<std::size_t>(
        (std::min)(kMaxCopyChunkBytes, clusters_per_chunk * geometry.bytes_per_cluster));

    std::uint64_t moved = 0;
    while (moved < total_bytes) {
        if (cancellation.stop_requested()) {
            return shrink_fail<std::uint64_t>(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        const auto remaining = total_bytes - moved;
        const auto size = static_cast<std::size_t>((std::min)(remaining, static_cast<std::uint64_t>(copy_chunk)));
        auto status =
            copy_chunk_verified(device, source_offset + moved, target_offset + moved, size,
                                cancellation);
        if (!status) {
            return base::Result<std::uint64_t>::failure(status.error());
        }
        moved += size;
    }
    return base::Result<std::uint64_t>::success(total_bytes);
}

} // namespace aegra::ntfs_resize::detail
