#include "ntfs_logfile_invalidation.h"

#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"

#include <algorithm>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

class CompositeReaderAdapter final : public ports::IRandomAccessReader {
  public:
    explicit CompositeReaderAdapter(CompositeNtfsBlockDevice& device) noexcept : device_(&device) {}
    [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
        return device_->source_logical_size_bytes();
    }
    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override {
        return device_->read_at(offset, destination, cancellation);
    }

  private:
    CompositeNtfsBlockDevice* device_;
};

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
load_logfile_data(MftRecordStore& mft_store, const base::CancellationToken cancellation) {
    auto raw = mft_store.read_record_bytes(kFileNumberLogFile, cancellation);
    if (!raw) {
        return base::Result<ntfs_core::AttributeValue>::failure(raw.error());
    }
    auto parsed = ntfs_core::parse_mft_record_bytes(raw.value(), mft_store.geometry().bytes_per_sector,
                                                    kFileNumberLogFile);
    if (!parsed) {
        return base::Result<ntfs_core::AttributeValue>::failure(parsed.error());
    }
    for (const auto& attribute : parsed.value().attributes) {
        if (attribute.type == ntfs_core::kAttrData && attribute.name.empty()) {
            return base::Result<ntfs_core::AttributeValue>::success(attribute);
        }
    }
    return shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                   "restore.shrink_unsupported_layout");
}

[[nodiscard]] base::Result<void>
fill_attribute_bytes(CompositeNtfsBlockDevice& device, const ntfs_core::BootGeometry& geometry,
                     const ntfs_core::AttributeValue& data, const std::byte fill_value,
                     const std::uint64_t total_bytes, const base::CancellationToken cancellation) {
    if (!data.non_resident) {
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    constexpr std::size_t kFillChunkBytes = 1U * 1024U * 1024U;
    const std::vector<std::byte> chunk(kFillChunkBytes, fill_value);
    std::uint64_t remaining = total_bytes;
    for (const auto& run : data.runs) {
        if (remaining == 0) {
            break;
        }
        if (run.sparse) {
            // A partially filled log would read back as a mix of fill bytes and sparse zeros,
            // which the NTFS driver treats as a corrupt log.
            return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                    "restore.shrink_unsupported_layout");
        }
        std::uint64_t run_offset = 0;
        std::uint64_t run_bytes = 0;
        if (!ntfs_core::checked_mul_u64(run.first_lcn.value, geometry.bytes_per_cluster,
                                        run_offset) ||
            !ntfs_core::checked_mul_u64(run.cluster_count.value, geometry.bytes_per_cluster,
                                        run_bytes)) {
            return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt");
        }
        std::uint64_t run_remaining = (std::min)(run_bytes, remaining);
        while (run_remaining != 0) {
            const auto piece =
                static_cast<std::size_t>((std::min<std::uint64_t>)(chunk.size(), run_remaining));
            auto written = device.write_at(
                run_offset, std::span<const std::byte>(chunk).first(piece), cancellation);
            if (!written) {
                return written;
            }
            run_offset += piece;
            run_remaining -= piece;
            remaining -= piece;
        }
    }
    if (remaining != 0) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    return device.flush(cancellation);
}

} // namespace

base::Result<void>
invalidate_logfile_restart_area(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                                const base::CancellationToken cancellation) {
    auto logfile = load_logfile_data(mft_store, cancellation);
    if (!logfile) {
        return base::Result<void>::failure(logfile.error());
    }
    if (logfile.value().data_size.value == 0) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    // Fill the whole $LogFile with 0xFF: the NTFS driver treats an all-0xFF log as pristine,
    // reinitializes it on the next mount, and does not mark the volume dirty. Zeroed restart
    // pages would instead read as a corrupt log, forcing a dirty mount that demands CHKDSK.
    return fill_attribute_bytes(device, mft_store.geometry(), logfile.value(), std::byte{0xFF},
                                logfile.value().data_size.value, cancellation);
}

} // namespace aegra::ntfs_resize::detail
