#pragma once

#include "ntfs_volume_view.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>
#include <vector>

namespace aegra::ntfs_resize::detail {

struct FreeClusterRange final {
    std::uint64_t begin_lcn{0};
    std::uint64_t end_lcn{0};
};

struct BitmapAllocator final {
    std::vector<std::byte> bitmap_bytes;
    std::vector<FreeClusterRange> allocatable_ranges;
    std::uint64_t free_inside_clusters{0};
    std::uint64_t allocatable_clusters{0};
    std::uint64_t new_total_cluster_count{0};
    std::uint64_t reserved_clusters{0};
};

[[nodiscard]] base::Result<BitmapAllocator>
build_bitmap_allocator(NtfsVolumeView& view, std::uint64_t new_total_cluster_count,
                       base::CancellationToken cancellation);

/// First-fit contiguous allocation from remaining free ranges inside the new boundary.
[[nodiscard]] base::Result<ClusterRange> allocate_first_fit(BitmapAllocator& allocator,
                                                            std::uint64_t cluster_count);

} // namespace aegra::ntfs_resize::detail
