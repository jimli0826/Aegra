#include "ntfs_mft_mirror_sync.h"

#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"

#include <algorithm>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

constexpr std::uint32_t kMirrorRecordCount = 4;
constexpr std::uint32_t kFileNumberMftMirr = 1;

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
load_mirr_data(MftRecordStore& mft_store, const base::CancellationToken cancellation) {
    auto raw = mft_store.read_record_bytes(kFileNumberMftMirr, cancellation);
    if (!raw) {
        return base::Result<ntfs_core::AttributeValue>::failure(raw.error());
    }
    auto parsed = ntfs_core::parse_mft_record_bytes(raw.value(), mft_store.geometry().bytes_per_sector,
                                                    kFileNumberMftMirr);
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
write_mirr_bytes(CompositeNtfsBlockDevice& device, const ntfs_core::BootGeometry& geometry,
                 const ntfs_core::AttributeValue& mirr_data, const std::span<const std::byte> bytes,
                 const base::CancellationToken cancellation) {
    if (!mirr_data.non_resident) {
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    std::uint64_t position = 0;
    for (const auto& run : mirr_data.runs) {
        if (position >= bytes.size()) {
            break;
        }
        if (run.sparse) {
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
        const auto piece =
            static_cast<std::size_t>((std::min)(bytes.size() - position, run_bytes));
        auto written =
            device.write_at(run_offset, bytes.subspan(static_cast<std::size_t>(position), piece),
                            cancellation);
        if (!written) {
            return written;
        }
        position += piece;
    }
    if (position != bytes.size()) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    return device.flush(cancellation);
}

} // namespace

base::Result<void> sync_mft_mirror(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                                   const base::CancellationToken cancellation) {
    auto mirr_data = load_mirr_data(mft_store, cancellation);
    if (!mirr_data) {
        return base::Result<void>::failure(mirr_data.error());
    }
    const auto record_bytes = mft_store.geometry().bytes_per_mft_record;
    std::vector<std::byte> mirror_image(static_cast<std::size_t>(record_bytes) * kMirrorRecordCount);
    for (std::uint32_t record_number = 0; record_number < kMirrorRecordCount; ++record_number) {
        if (cancellation.stop_requested()) {
            return shrink_fail_void(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        auto record = mft_store.read_record_bytes(record_number, cancellation);
        if (!record) {
            return base::Result<void>::failure(record.error());
        }
        if (record.value().size() != record_bytes) {
            return shrink_fail_void(base::ErrorCode::kCorruptData,
                                    "restore.shrink_unsupported_layout");
        }
        std::copy(record.value().begin(), record.value().end(),
                  mirror_image.begin() + static_cast<std::ptrdiff_t>(record_number * record_bytes));
    }
    return write_mirr_bytes(device, mft_store.geometry(), mirr_data.value(), mirror_image,
                            cancellation);
}

} // namespace aegra::ntfs_resize::detail
