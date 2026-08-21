#pragma once

#include "ntfs_mft_scanner.h"
#include "ntfs_relocation_plan.h"

#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>

namespace aegra::ntfs_resize::detail {

[[nodiscard]] base::Result<void>
audit_relocation_plan(const MftScanResult& scan, const RelocationPlanResult& plan,
                      std::uint64_t new_total_cluster_count);

} // namespace aegra::ntfs_resize::detail
