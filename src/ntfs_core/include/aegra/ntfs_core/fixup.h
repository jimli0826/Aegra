#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_core {

/// Restores the original end-of-sector words using the USA (read path).
[[nodiscard]] base::Result<void>
apply_fixup(std::span<std::byte> buffer, std::uint32_t bytes_per_sector, std::uint16_t usa_offset,
             std::uint16_t usa_count);

/// Saves end-of-sector words into the USA and stamps the USN (write path).
[[nodiscard]] base::Result<void>
seal_fixup(std::span<std::byte> buffer, std::uint32_t bytes_per_sector, std::uint16_t usa_offset,
            std::uint16_t usa_count);

} // namespace aegra::ntfs_core
