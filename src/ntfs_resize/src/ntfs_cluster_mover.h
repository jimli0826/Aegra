#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>

namespace aegra::ntfs_resize::detail {

/// Copies one relocation extent with a temporary buffer (safe for overlapping LCN ranges),
/// then flush + readback verifies the target clusters.
[[nodiscard]] base::Result<std::uint64_t>
move_relocation_extent(CompositeNtfsBlockDevice& device, const ntfs_core::BootGeometry& geometry,
                       const RelocationRecord& record, base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
