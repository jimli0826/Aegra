#include "ntfs_internal.h"

#include <algorithm>
#include <cstring>

namespace aegra::adapters::ntfs::detail {

base::Result<std::vector<DataRun>> parse_runlist(const std::span<const std::byte> runlist,
                                                 const std::uint64_t first_vcn,
                                                 const std::uint64_t last_vcn) {
    std::vector<DataRun> runs;
    std::size_t offset = 0;
    std::uint64_t current_vcn = first_vcn;
    std::uint64_t current_lcn = 0;
    constexpr std::size_t kMaxRuns = 65536;

    while (offset < runlist.size()) {
        const auto header = std::to_integer<std::uint8_t>(runlist[offset]);
        if (header == 0) {
            break;
        }
        ++offset;
        const auto length_size = static_cast<std::size_t>(header & 0x0FU);
        const auto offset_size = static_cast<std::size_t>((header >> 4) & 0x0FU);
        if (length_size == 0 || length_size > 8 || offset_size > 8) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        if (length_size > runlist.size() - offset ||
            offset_size > runlist.size() - offset - length_size) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        std::uint64_t cluster_count = 0;
        for (std::size_t i = 0; i < length_size; ++i) {
            cluster_count |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(runlist[offset + i]))
                << (8U * i);
        }
        offset += length_size;
        if (cluster_count == 0) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }

        DataRun run;
        run.vcn = current_vcn;
        run.cluster_count = cluster_count;
        if (offset_size == 0) {
            run.sparse = true;
            run.lcn = 0;
        } else {
            const auto delta = read_signed_le(runlist, offset, offset_size);
            offset += offset_size;
            std::uint64_t next_lcn = 0;
            if (delta >= 0) {
                const auto add = static_cast<std::uint64_t>(delta);
                if (!checked_add_u64(current_lcn, add, next_lcn)) {
                    return base::Result<std::vector<DataRun>>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
                }
            } else {
                // |delta| as unsigned without negating INT64_MIN on a signed type.
                const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(delta);
                if (magnitude > current_lcn) {
                    return base::Result<std::vector<DataRun>>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
                }
                next_lcn = current_lcn - magnitude;
            }
            current_lcn = next_lcn;
            run.sparse = false;
            run.lcn = next_lcn;
        }

        std::uint64_t next_vcn = 0;
        if (!checked_add_u64(current_vcn, cluster_count, next_vcn)) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        current_vcn = next_vcn;
        runs.push_back(run);
        if (runs.size() > kMaxRuns) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
    }

    if (!runs.empty() && last_vcn >= first_vcn) {
        std::uint64_t expected_end = 0;
        if (!checked_add_u64(last_vcn, 1, expected_end) || current_vcn != expected_end) {
            return base::Result<std::vector<DataRun>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
    }
    return base::Result<std::vector<DataRun>>::success(std::move(runs));
}

base::Result<std::size_t> read_from_layout(ports::IRandomAccessReader& reader,
                                           const NtfsVolumeInfo& info, const AttributeValue& data,
                                           const std::uint64_t offset,
                                           const std::span<std::byte> destination,
                                           const base::CancellationToken cancellation) {
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }
    if (offset >= data.data_size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto remaining = data.data_size - offset;
    const auto to_read =
        remaining < destination.size() ? static_cast<std::size_t>(remaining) : destination.size();
    std::memset(destination.data(), 0, to_read);

    if (!data.non_resident) {
        if (offset > data.resident_data.size()) {
            return base::Result<std::size_t>::success(0);
        }
        const auto available = data.resident_data.size() - static_cast<std::size_t>(offset);
        const auto copy_size = available < to_read ? available : to_read;
        std::memcpy(destination.data(),
                    data.resident_data.data() + static_cast<std::size_t>(offset), copy_size);
        return base::Result<std::size_t>::success(copy_size);
    }

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
        const auto file_offset = offset + written;
        const auto file_vcn = file_offset / info.bytes_per_cluster;
        const auto cluster_offset = static_cast<std::size_t>(file_offset % info.bytes_per_cluster);
        const DataRun* run = nullptr;
        for (const auto& candidate : data.runs) {
            if (file_vcn >= candidate.vcn && file_vcn - candidate.vcn < candidate.cluster_count) {
                run = &candidate;
                break;
            }
        }
        if (run == nullptr) {
            // Unmapped hole inside logical size: leave zero-filled.
            const auto hole = static_cast<std::size_t>(
                (std::min)(static_cast<std::uint64_t>(to_read - written),
                           static_cast<std::uint64_t>(info.bytes_per_cluster - cluster_offset)));
            written += hole;
            continue;
        }
        const auto in_run = file_vcn - run->vcn;
        const auto chunk = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(to_read - written),
                       static_cast<std::uint64_t>(info.bytes_per_cluster - cluster_offset)));
        if (run->sparse) {
            written += chunk;
            continue;
        }
        std::uint64_t disk_cluster = 0;
        if (!checked_add_u64(run->lcn, in_run, disk_cluster)) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        std::uint64_t disk_offset = 0;
        if (!checked_mul_u64(disk_cluster, info.bytes_per_cluster, disk_offset) ||
            !checked_add_u64(disk_offset, cluster_offset, disk_offset)) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        if (disk_offset >= info.volume_size_bytes || chunk > info.volume_size_bytes - disk_offset) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt"));
        }
        auto read = reader.read_at(disk_offset, destination.subspan(written, chunk), cancellation);
        if (!read) {
            return base::Result<std::size_t>::failure(read.error());
        }
        if (read.value() < chunk) {
            // Short read below EOF of volume is corrupt for a mapped run.
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kIoFailure, "ntfs.read_failed"));
        }
        written += chunk;
    }
    return base::Result<std::size_t>::success(written);
}

} // namespace aegra::adapters::ntfs::detail
