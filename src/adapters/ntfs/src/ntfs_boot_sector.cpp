#include "ntfs_internal.h"

#include <cstring>
#include <limits>
#include <utility>

namespace aegra::adapters::ntfs::detail {
namespace {

[[nodiscard]] bool is_power_of_two(const std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

} // namespace

base::Result<NtfsVolumeInfo> parse_boot_sector(const std::span<const std::byte> sector) {
    if (sector.size() < 512) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }
    if (std::memcmp(sector.data() + 3, "NTFS    ", 8) != 0) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }

    NtfsVolumeInfo info;
    info.bytes_per_sector = read_u16(sector, 0x0B);
    info.sectors_per_cluster = std::to_integer<std::uint8_t>(sector[0x0D]);
    if (!is_power_of_two(info.bytes_per_sector) || info.bytes_per_sector < 512 ||
        info.bytes_per_sector > 4096 || info.sectors_per_cluster == 0) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    std::uint64_t bpc = 0;
    if (!checked_mul_u64(info.bytes_per_sector, info.sectors_per_cluster, bpc) ||
        bpc > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    info.bytes_per_cluster = static_cast<std::uint32_t>(bpc);

    const auto total_sectors = read_u64(sector, 0x28);
    info.mft_start_cluster = read_u64(sector, 0x30);
    info.volume_serial = read_u64(sector, 0x48);
    if (total_sectors == 0) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    std::uint64_t volume_size = 0;
    if (!checked_mul_u64(total_sectors, info.bytes_per_sector, volume_size)) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    info.volume_size_bytes = volume_size;
    info.total_clusters = total_sectors / info.sectors_per_cluster;

    const auto clusters_per_mft =
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(sector[0x40]));
    if (clusters_per_mft > 0) {
        std::uint64_t mft_bytes = 0;
        if (!checked_mul_u64(static_cast<std::uint64_t>(clusters_per_mft), info.bytes_per_cluster,
                             mft_bytes) ||
            mft_bytes == 0 || mft_bytes > kMaximumMftRecordBytes) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        info.bytes_per_mft_record = static_cast<std::uint32_t>(mft_bytes);
    } else if (clusters_per_mft < 0) {
        const auto shift = static_cast<std::uint32_t>(-clusters_per_mft);
        if (shift >= 31) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        info.bytes_per_mft_record = 1U << shift;
        if (info.bytes_per_mft_record < 512 ||
            info.bytes_per_mft_record > kMaximumMftRecordBytes) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
    } else {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }

    const auto clusters_per_index =
        static_cast<std::int8_t>(std::to_integer<std::uint8_t>(sector[0x44]));
    if (clusters_per_index > 0) {
        std::uint64_t index_bytes = 0;
        if (!checked_mul_u64(static_cast<std::uint64_t>(clusters_per_index),
                             info.bytes_per_cluster, index_bytes) ||
            index_bytes == 0 || index_bytes > kMaximumIndexRecordBytes) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        info.bytes_per_index_record = static_cast<std::uint32_t>(index_bytes);
    } else if (clusters_per_index < 0) {
        const auto shift = static_cast<std::uint32_t>(-clusters_per_index);
        if (shift >= 31) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
        info.bytes_per_index_record = 1U << shift;
        if (info.bytes_per_index_record < 512 ||
            info.bytes_per_index_record > kMaximumIndexRecordBytes) {
            return base::Result<NtfsVolumeInfo>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
        }
    } else {
        info.bytes_per_index_record = info.bytes_per_cluster;
    }

    if (info.mft_start_cluster >= info.total_clusters && info.total_clusters != 0) {
        return base::Result<NtfsVolumeInfo>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_geometry"));
    }
    return base::Result<NtfsVolumeInfo>::success(std::move(info));
}

} // namespace aegra::adapters::ntfs::detail
