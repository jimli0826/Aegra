#pragma once

#include "ntfs_bitmap_allocator.h"
#include "ntfs_mft_scanner.h"

#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>
#include <vector>

namespace aegra::ntfs_resize::detail {

struct RelocationPlanResult final {
    std::vector<RelocationRecord> relocations;
    std::vector<MetadataMutation> mutations;
    std::vector<CriticalFileOperation> critical_operations;
    std::uint64_t relocation_cluster_count{0};
};

[[nodiscard]] base::Result<RelocationPlanResult>
build_relocation_plan(const MftScanResult& scan, BitmapAllocator& allocator,
                      std::uint64_t new_total_cluster_count);

} // namespace aegra::ntfs_resize::detail
