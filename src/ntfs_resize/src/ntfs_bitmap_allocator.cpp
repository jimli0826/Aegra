#include "ntfs_bitmap_allocator.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/bitmap.h"

#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] std::uint64_t reserved_cluster_budget(const std::uint64_t new_total_cluster_count) noexcept {
    const auto percent = new_total_cluster_count / 100U;
    return percent > 64U ? percent : 64U;
}

[[nodiscard]] base::Result<void>
append_free_bit(std::vector<FreeClusterRange>& ranges, const std::uint64_t lcn) {
    if (!ranges.empty() && ranges.back().end_lcn == lcn) {
        ranges.back().end_lcn = lcn + 1U;
        return base::Result<void>::success();
    }
    ranges.push_back(FreeClusterRange{lcn, lcn + 1U});
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
scan_free_ranges(const std::span<const std::byte> bitmap, const std::uint64_t new_total_cluster_count,
                 BitmapAllocator& allocator) {
    if (new_total_cluster_count <= 1) {
        return shrink_fail_void(base::ErrorCode::kInsufficientSpace, "restore.shrink_below_minimum");
    }
    for (std::uint64_t lcn = 0; lcn < new_total_cluster_count; ++lcn) {
        auto bit = ntfs_core::bitmap_bit_is_set(bitmap, lcn);
        if (!bit) {
            return base::Result<void>::failure(bit.error());
        }
        if (bit.value()) {
            continue;
        }
        ++allocator.free_inside_clusters;
        // Cluster 0 holds primary boot; last cluster may hold backup boot.
        if (lcn == 0 || lcn + 1U == new_total_cluster_count) {
            continue;
        }
        auto status = append_free_bit(allocator.allocatable_ranges, lcn);
        if (!status) {
            return status;
        }
        ++allocator.allocatable_clusters;
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<BitmapAllocator>
build_bitmap_allocator(NtfsVolumeView& view, const std::uint64_t new_total_cluster_count,
                       const base::CancellationToken cancellation) {
    auto bitmap_data = load_unnamed_data_attribute(view, kFileNumberBitmap, cancellation);
    if (!bitmap_data) {
        return base::Result<BitmapAllocator>::failure(bitmap_data.error());
    }
    auto covers = ntfs_core::validate_bitmap_covers_bits(view.geometry.total_clusters,
                                                         bitmap_data.value().data_size.value);
    if (!covers) {
        return base::Result<BitmapAllocator>::failure(covers.error());
    }
    auto bytes =
        read_attribute_payload(view, bitmap_data.value(), kMaxBitmapLoadBytes, cancellation);
    if (!bytes) {
        return base::Result<BitmapAllocator>::failure(bytes.error());
    }

    BitmapAllocator allocator;
    allocator.bitmap_bytes = std::move(bytes).value();
    allocator.new_total_cluster_count = new_total_cluster_count;
    allocator.reserved_clusters = reserved_cluster_budget(new_total_cluster_count);
    auto scanned =
        scan_free_ranges(std::span<const std::byte>(allocator.bitmap_bytes), new_total_cluster_count,
                         allocator);
    if (!scanned) {
        return base::Result<BitmapAllocator>::failure(scanned.error());
    }
    return base::Result<BitmapAllocator>::success(std::move(allocator));
}

base::Result<ClusterRange> allocate_first_fit(BitmapAllocator& allocator,
                                              const std::uint64_t cluster_count) {
    if (cluster_count == 0) {
        return shrink_fail<ClusterRange>(base::ErrorCode::kInvalidArgument,
                                         "restore.shrink_plan_corrupt");
    }
    if (allocator.allocatable_clusters < allocator.reserved_clusters ||
        cluster_count > allocator.allocatable_clusters - allocator.reserved_clusters) {
        return shrink_fail<ClusterRange>(base::ErrorCode::kInsufficientSpace,
                                         "restore.shrink_below_minimum");
    }

    for (auto& range : allocator.allocatable_ranges) {
        const auto available = range.end_lcn - range.begin_lcn;
        if (available < cluster_count) {
            continue;
        }
        ClusterRange allocated;
        allocated.begin_lcn = range.begin_lcn;
        allocated.end_lcn = range.begin_lcn + cluster_count;
        range.begin_lcn = allocated.end_lcn;
        allocator.allocatable_clusters -= cluster_count;
        return base::Result<ClusterRange>::success(allocated);
    }
    return shrink_fail<ClusterRange>(base::ErrorCode::kUnsupportedVersion,
                                     "restore.shrink_unsupported_layout");
}

} // namespace aegra::ntfs_resize::detail
