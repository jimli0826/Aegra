#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ntfs_core {

[[nodiscard]] base::Result<BootGeometry> parse_boot_sector(std::span<const std::byte> sector);

/// Copies `original` and patches TotalSectors (the backup-Boot sector index), MFT LCN and
/// MFTMirr LCN for a shrink commit. A device holding the patched NTFS volume must contain
/// `new_total_sectors + 1` sectors.
/// Re-validates the result with `parse_boot_sector`. Does not invent a boot sector from scratch.
[[nodiscard]] base::Result<std::vector<std::byte>>
patch_boot_geometry(std::span<const std::byte> original, std::uint64_t new_total_sectors,
                    std::uint64_t new_mft_start_lcn,
                    std::uint64_t new_mft_mirror_start_lcn);

} // namespace aegra::ntfs_core
