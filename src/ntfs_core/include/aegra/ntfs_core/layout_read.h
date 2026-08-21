#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"
#include "aegra/ports/random_access.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_core {

/// Reads logical bytes from a parsed attribute using its resident payload or data runs.
[[nodiscard]] base::Result<std::size_t>
read_from_attribute(ports::IRandomAccessReader& reader, const BootGeometry& geometry,
                    const AttributeValue& data, ByteOffset offset, std::span<std::byte> destination,
                    base::CancellationToken cancellation);

} // namespace aegra::ntfs_core
