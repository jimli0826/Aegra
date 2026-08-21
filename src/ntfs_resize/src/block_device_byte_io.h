#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_resize::detail {

/// Reads a byte range through a sector-aligned block-device request.
[[nodiscard]] base::Result<std::size_t>
read_block_device_bytes(ports::IRandomAccessBlockDevice& device, std::uint64_t offset,
                        std::span<std::byte> destination,
                        base::CancellationToken cancellation);

/// Writes a byte range through sector-aligned read-modify-write when required.
[[nodiscard]] base::Result<void>
write_block_device_bytes(ports::IRandomAccessBlockDevice& device, std::uint64_t offset,
                         std::span<const std::byte> source,
                         base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
