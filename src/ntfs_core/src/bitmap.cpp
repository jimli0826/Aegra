#include "aegra/ntfs_core/bitmap.h"

#include "aegra/ntfs_core/binary.h"

namespace aegra::ntfs_core {

base::Result<void> validate_bitmap_covers_bits(const std::uint64_t bit_count,
                                               const std::uint64_t bitmap_size_bytes) {
    if (bit_count == 0) {
        return base::Result<void>::success();
    }
    const auto required = bit_count / 8U + (bit_count % 8U != 0 ? 1U : 0U);
    if (bitmap_size_bytes < required) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    return base::Result<void>::success();
}

base::Result<bool> bitmap_bit_is_set(const std::span<const std::byte> bitmap,
                                     const std::uint64_t bit_index) {
    const auto byte_index = bit_index / 8U;
    if (byte_index >= bitmap.size()) {
        return base::Result<bool>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    const auto bit = static_cast<unsigned>(bit_index % 8U);
    const auto value = std::to_integer<std::uint8_t>(bitmap[byte_index]);
    return base::Result<bool>::success((value & static_cast<std::uint8_t>(1U << bit)) != 0);
}

} // namespace aegra::ntfs_core
