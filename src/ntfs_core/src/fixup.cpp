#include "aegra/ntfs_core/fixup.h"

#include "aegra/ntfs_core/binary.h"

#include <cstring>

namespace aegra::ntfs_core {

base::Result<void> apply_fixup(const std::span<std::byte> buffer,
                               const std::uint32_t bytes_per_sector,
                               const std::uint16_t usa_offset, const std::uint16_t usa_count) {
    if (bytes_per_sector < 2 || usa_count == 0) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    if (usa_offset + static_cast<std::uint32_t>(usa_count) * 2U > buffer.size()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    const auto sector_count = static_cast<std::uint32_t>(usa_count) - 1U;
    std::uint64_t required = 0;
    if (!checked_mul_u64(sector_count, bytes_per_sector, required) || required > buffer.size()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    const auto usn = read_u16(buffer, usa_offset);
    for (std::uint32_t i = 0; i < sector_count; ++i) {
        const auto sector_end = static_cast<std::size_t>(i + 1U) * bytes_per_sector - 2U;
        const auto expected = read_u16(buffer, sector_end);
        if (expected != usn) {
            return base::Result<void>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
        }
        const auto restored = read_u16(buffer, usa_offset + 2U + i * 2U);
        std::memcpy(buffer.data() + sector_end, &restored, sizeof(restored));
    }
    return base::Result<void>::success();
}

base::Result<void> seal_fixup(const std::span<std::byte> buffer, const std::uint32_t bytes_per_sector,
                              const std::uint16_t usa_offset, const std::uint16_t usa_count) {
    if (bytes_per_sector < 2 || usa_count == 0) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    if (usa_offset + static_cast<std::uint32_t>(usa_count) * 2U > buffer.size()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    const auto sector_count = static_cast<std::uint32_t>(usa_count) - 1U;
    std::uint64_t required = 0;
    if (!checked_mul_u64(sector_count, bytes_per_sector, required) || required > buffer.size()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.fixup_failed"));
    }
    auto usn = read_u16(buffer, usa_offset);
    ++usn;
    if (usn == 0) {
        usn = 1;
    }
    write_u16(buffer, usa_offset, usn);
    for (std::uint32_t i = 0; i < sector_count; ++i) {
        const auto sector_end = static_cast<std::size_t>(i + 1U) * bytes_per_sector - 2U;
        const auto original = read_u16(buffer, sector_end);
        write_u16(buffer, usa_offset + 2U + i * 2U, original);
        write_u16(buffer, sector_end, usn);
    }
    return base::Result<void>::success();
}

} // namespace aegra::ntfs_core
