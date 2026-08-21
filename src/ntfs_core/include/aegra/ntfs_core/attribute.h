#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_core {

[[nodiscard]] base::Result<void>
validate_attribute_header(std::span<const std::byte> record, std::size_t offset,
                          std::uint32_t& type, std::uint32_t& length, bool& non_resident);

[[nodiscard]] base::Result<AttributeValue>
parse_attribute(std::span<const std::byte> record, std::size_t offset);

} // namespace aegra::ntfs_core
