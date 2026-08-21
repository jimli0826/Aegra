#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstdint>

namespace aegra::ntfs_resize {

/// Overwrites Primary Boot [0, sector) and Backup Boot (last sector) with non-NTFS bytes,
/// then flush + readback verifies both ranges. Uses IRandomAccessBlockDevice only.
[[nodiscard]] base::Result<void>
invalidate_ntfs_boot_sectors(ports::IRandomAccessBlockDevice& target, std::uint32_t bytes_per_sector,
                             base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize
