#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ntfs_core {

/// Pure Attribute List body parser (no I/O). Rejects truncated/overlapping entries.
[[nodiscard]] base::Result<std::vector<AttributeListEntry>>
parse_attribute_list(std::span<const std::byte> list_bytes);

/// Rejects lists that reference the same extension record in a way that forms an immediate
/// self-cycle for the base record, or that exceed the entry ceiling.
[[nodiscard]] base::Result<void>
validate_attribute_list_entries(std::span<const AttributeListEntry> entries,
                                std::uint64_t base_record_number);

} // namespace aegra::ntfs_core
