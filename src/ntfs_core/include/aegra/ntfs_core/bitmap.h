#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_core {

[[nodiscard]] base::Result<void>
validate_bitmap_covers_bits(std::uint64_t bit_count, std::uint64_t bitmap_size_bytes);

[[nodiscard]] base::Result<bool>
bitmap_bit_is_set(std::span<const std::byte> bitmap, std::uint64_t bit_index);

} // namespace aegra::ntfs_core
