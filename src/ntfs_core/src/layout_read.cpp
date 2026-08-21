#include "aegra/ntfs_core/layout_read.h"

#include "aegra/ntfs_core/binary.h"

#include <algorithm>
#include <cstring>

namespace aegra::ntfs_core {
namespace {

[[nodiscard]] const DataRun* find_run(const std::vector<DataRun>& runs,
                                      const std::uint64_t file_vcn) noexcept {
    for (const auto& candidate : runs) {
        if (file_vcn >= candidate.first_vcn.value &&
            file_vcn - candidate.first_vcn.value < candidate.cluster_count.value) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] base::Result<std::size_t>
read_mapped_chunk(ports::IRandomAccessReader& reader, const BootGeometry& geometry,
                  const DataRun& run, const std::uint64_t in_run, const std::size_t cluster_offset,
                  const std::span<std::byte> destination, const base::CancellationToken cancellation) {
    std::uint64_t disk_cluster = 0;
    if (!checked_add_u64(run.first_lcn.value, in_run, disk_cluster)) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
    }
    std::uint64_t disk_offset = 0;
    if (!checked_mul_u64(disk_cluster, geometry.bytes_per_cluster, disk_offset) ||
        !checked_add_u64(disk_offset, cluster_offset, disk_offset)) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
    }
    if (disk_offset >= geometry.volume_size_bytes.value ||
        destination.size() > geometry.volume_size_bytes.value - disk_offset) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
    }
    auto read = reader.read_at(disk_offset, destination, cancellation);
    if (!read) {
        return base::Result<std::size_t>::failure(read.error());
    }
    if (read.value() < destination.size()) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kIoFailure, "ntfs.read_failed"));
    }
    return base::Result<std::size_t>::success(destination.size());
}

[[nodiscard]] base::Result<std::size_t>
read_non_resident(ports::IRandomAccessReader& reader, const BootGeometry& geometry,
                  const AttributeValue& data, const ByteOffset offset,
                  const std::span<std::byte> destination, const std::size_t to_read,
                  const base::CancellationToken cancellation) {
    if (data.compressed) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.compressed_unsupported"));
    }
    if (data.encrypted) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.efs_unsupported"));
    }
    std::size_t written = 0;
    while (written < to_read) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCancelled, "ntfs.read_failed"));
        }
        const auto file_offset = offset.value + written;
        const auto file_vcn = file_offset / geometry.bytes_per_cluster;
        const auto cluster_offset =
            static_cast<std::size_t>(file_offset % geometry.bytes_per_cluster);
        const auto* run = find_run(data.runs, file_vcn);
        const auto chunk = static_cast<std::size_t>((std::min)(
            static_cast<std::uint64_t>(to_read - written),
            static_cast<std::uint64_t>(geometry.bytes_per_cluster - cluster_offset)));
        if (run == nullptr) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        if (run->sparse) {
            std::fill(destination.begin() + static_cast<std::ptrdiff_t>(written),
                      destination.begin() + static_cast<std::ptrdiff_t>(written + chunk),
                      std::byte{0});
            written += chunk;
            continue;
        }
        auto mapped = read_mapped_chunk(reader, geometry, *run, file_vcn - run->first_vcn.value,
                                        cluster_offset, destination.subspan(written, chunk),
                                        cancellation);
        if (!mapped) {
            return mapped;
        }
        written += mapped.value();
    }
    return base::Result<std::size_t>::success(written);
}

} // namespace

base::Result<std::size_t>
read_from_attribute(ports::IRandomAccessReader& reader, const BootGeometry& geometry,
                    const AttributeValue& data, const ByteOffset offset,
                    const std::span<std::byte> destination,
                    const base::CancellationToken cancellation) {
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }
    if (offset.value >= data.data_size.value) {
        return base::Result<std::size_t>::success(0);
    }
    const auto remaining = data.data_size.value - offset.value;
    const auto to_read =
        remaining < destination.size() ? static_cast<std::size_t>(remaining) : destination.size();
    std::memset(destination.data(), 0, to_read);

    if (!data.non_resident) {
        if (offset.value > data.resident_data.size()) {
            return base::Result<std::size_t>::success(0);
        }
        const auto available = data.resident_data.size() - static_cast<std::size_t>(offset.value);
        const auto copy_size = available < to_read ? available : to_read;
        std::memcpy(destination.data(),
                    data.resident_data.data() + static_cast<std::size_t>(offset.value), copy_size);
        return base::Result<std::size_t>::success(copy_size);
    }
    return read_non_resident(reader, geometry, data, offset, destination, to_read, cancellation);
}

} // namespace aegra::ntfs_core
