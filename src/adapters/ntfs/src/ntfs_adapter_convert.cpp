#include "ntfs_internal.h"

#include <limits>

namespace aegra::adapters::ntfs::detail {

NtfsFileReference to_explorer_reference(const FileReference reference) noexcept {
    return NtfsFileReference{
        .record_number = reference.record_number,
        .sequence_number = reference.sequence_number,
    };
}

FileReference to_core_reference(const NtfsFileReference reference) noexcept {
    return FileReference{
        .record_number = reference.record_number,
        .sequence_number = reference.sequence_number,
    };
}

NtfsVolumeInfo to_volume_info(const BootGeometry& geometry) noexcept {
    return NtfsVolumeInfo{
        .bytes_per_sector = geometry.bytes_per_sector,
        .sectors_per_cluster = geometry.sectors_per_cluster,
        .bytes_per_cluster = geometry.bytes_per_cluster,
        .bytes_per_mft_record = geometry.bytes_per_mft_record,
        .bytes_per_index_record = geometry.bytes_per_index_record,
        .total_clusters = geometry.total_clusters,
        .volume_size_bytes = geometry.volume_size_bytes.value,
        .mft_start_cluster = geometry.mft_start_lcn.value,
        .volume_serial = geometry.volume_serial,
    };
}

BootGeometry to_boot_geometry(const NtfsVolumeInfo& info) noexcept {
    return BootGeometry{
        .bytes_per_sector = info.bytes_per_sector,
        .sectors_per_cluster = info.sectors_per_cluster,
        .bytes_per_cluster = info.bytes_per_cluster,
        .bytes_per_mft_record = info.bytes_per_mft_record,
        .bytes_per_index_record = info.bytes_per_index_record,
        .total_clusters = info.total_clusters,
        .volume_size_bytes = ntfs_core::ByteCount{info.volume_size_bytes},
        .mft_start_lcn = ntfs_core::LogicalClusterNumber{info.mft_start_cluster},
        .volume_serial = info.volume_serial,
    };
}

base::Result<std::string> encode_continuation(const std::uint32_t skip_count) {
    return base::Result<std::string>::success("s:" + std::to_string(skip_count));
}

base::Result<std::uint32_t> decode_continuation(const std::optional<std::string>& token) {
    if (!token.has_value() || token->empty()) {
        return base::Result<std::uint32_t>::success(0);
    }
    if (token->size() < 3 || !token->starts_with("s:")) {
        return base::Result<std::uint32_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
    }
    try {
        const auto value = std::stoul(token->substr(2));
        if (value > (std::numeric_limits<std::uint32_t>::max)()) {
            return base::Result<std::uint32_t>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
        }
        return base::Result<std::uint32_t>::success(static_cast<std::uint32_t>(value));
    } catch (...) {
        return base::Result<std::uint32_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
    }
}

} // namespace aegra::adapters::ntfs::detail
