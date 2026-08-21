#include "aegra/ntfs_core/boot_sector.h"

#include "aegra/ntfs_core/binary.h"

#include <cstring>
#include <limits>
#include <utility>

namespace aegra::ntfs_core {
namespace {

[[nodiscard]] bool is_power_of_two(const std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] base::Result<std::uint32_t>
decode_record_size(const std::int8_t clusters_per_record, const std::uint32_t bytes_per_cluster,
                   const std::uint32_t maximum_bytes) {
    if (clusters_per_record > 0) {
        std::uint64_t record_bytes = 0;
        if (!checked_mul_u64(static_cast<std::uint64_t>(clusters_per_record), bytes_per_cluster,
                             record_bytes) ||
            record_bytes == 0 || record_bytes > maximum_bytes) {
            return base::Result<std::uint32_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        return base::Result<std::uint32_t>::success(static_cast<std::uint32_t>(record_bytes));
    }
    if (clusters_per_record < 0) {
        const auto shift = static_cast<std::uint32_t>(-clusters_per_record);
        if (shift >= 31) {
            return base::Result<std::uint32_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        const auto record_bytes = 1U << shift;
        if (record_bytes < 512 || record_bytes > maximum_bytes) {
            return base::Result<std::uint32_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        return base::Result<std::uint32_t>::success(record_bytes);
    }
    return base::Result<std::uint32_t>::failure(
        make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
}

} // namespace

base::Result<BootGeometry> parse_boot_sector(const std::span<const std::byte> sector) {
    if (sector.size() < 512) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }
    if (std::memcmp(sector.data() + 3, "NTFS    ", 8) != 0) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }
    if (sector[510] != std::byte{0x55} || sector[511] != std::byte{0xAA}) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }

    BootGeometry info;
    info.bytes_per_sector = read_u16(sector, 0x0B);
    info.sectors_per_cluster = std::to_integer<std::uint8_t>(sector[0x0D]);
    if (!is_power_of_two(info.bytes_per_sector) || info.bytes_per_sector < 512 ||
        info.bytes_per_sector > 4096 || info.sectors_per_cluster == 0) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    std::uint64_t bpc = 0;
    if (!checked_mul_u64(info.bytes_per_sector, info.sectors_per_cluster, bpc) ||
        bpc > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    info.bytes_per_cluster = static_cast<std::uint32_t>(bpc);

    const auto total_sectors = read_u64(sector, 0x28);
    info.mft_start_lcn.value = read_u64(sector, 0x30);
    info.mft_mirror_start_lcn.value = read_u64(sector, 0x38);
    info.volume_serial = read_u64(sector, 0x48);
    if (total_sectors == 0) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    std::uint64_t volume_size = 0;
    if (!checked_mul_u64(total_sectors, info.bytes_per_sector, volume_size)) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    info.volume_size_bytes.value = volume_size;
    info.total_clusters = total_sectors / info.sectors_per_cluster;

    const auto clusters_per_mft =
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(sector[0x40]));
    auto mft_size = decode_record_size(clusters_per_mft, info.bytes_per_cluster, kMaxMftRecordBytes);
    if (!mft_size) {
        return base::Result<BootGeometry>::failure(mft_size.error());
    }
    info.bytes_per_mft_record = mft_size.value();

    const auto clusters_per_index =
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(sector[0x44]));
    if (clusters_per_index == 0) {
        info.bytes_per_index_record = info.bytes_per_cluster;
    } else {
        auto index_size =
            decode_record_size(clusters_per_index, info.bytes_per_cluster, kMaxIndexRecordBytes);
        if (!index_size) {
            return base::Result<BootGeometry>::failure(index_size.error());
        }
        info.bytes_per_index_record = index_size.value();
    }

    if (info.total_clusters != 0 &&
        (info.mft_start_lcn.value >= info.total_clusters ||
         info.mft_mirror_start_lcn.value >= info.total_clusters)) {
        return base::Result<BootGeometry>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    return base::Result<BootGeometry>::success(std::move(info));
}

base::Result<std::vector<std::byte>>
patch_boot_geometry(const std::span<const std::byte> original,
                    const std::uint64_t new_total_sectors,
                    const std::uint64_t new_mft_start_lcn,
                    const std::uint64_t new_mft_mirror_start_lcn) {
    if (original.size() < 512 || new_total_sectors == 0) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.invalid_boot_sector"));
    }
    auto parsed = parse_boot_sector(original);
    if (!parsed) {
        return base::Result<std::vector<std::byte>>::failure(parsed.error());
    }
    std::vector<std::byte> patched(original.begin(), original.end());
    write_unsigned_le(std::span<std::byte>(patched), 0x28, new_total_sectors, 8);
    write_unsigned_le(std::span<std::byte>(patched), 0x30, new_mft_start_lcn, 8);
    write_unsigned_le(std::span<std::byte>(patched), 0x38, new_mft_mirror_start_lcn, 8);
    // Ensure AA55 marker remains at the end of the first sector.
    if (patched.size() >= 512) {
        patched[510] = std::byte{0x55};
        patched[511] = std::byte{0xAA};
    }
    auto verified = parse_boot_sector(patched);
    if (!verified) {
        return base::Result<std::vector<std::byte>>::failure(verified.error());
    }
    if (verified.value().total_clusters !=
            new_total_sectors / verified.value().sectors_per_cluster ||
        verified.value().mft_start_lcn.value != new_mft_start_lcn ||
        verified.value().mft_mirror_start_lcn.value != new_mft_mirror_start_lcn) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(patched));
}

} // namespace aegra::ntfs_core
