#include "ntfs_bitmap_commit.h"

#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/bitmap.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] bool has_matching_relocation(const ShrinkPlan& plan,
                                           const RecordClassFilter filter) noexcept {
    return std::ranges::any_of(plan.relocation_records(), [filter](const RelocationRecord& record) {
        return matches_record_filter(filter, record.mft_record_number);
    });
}

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

[[nodiscard]] base::Result<void>
set_bitmap_bit(std::vector<std::byte>& bitmap, const std::uint64_t bit_index, const bool allocated) {
    const auto byte_index = static_cast<std::size_t>(bit_index / 8U);
    if (byte_index >= bitmap.size()) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt");
    }
    const auto mask = static_cast<std::uint8_t>(1U << (bit_index % 8U));
    auto value = std::to_integer<std::uint8_t>(bitmap[byte_index]);
    if (allocated) {
        value = static_cast<std::uint8_t>(value | mask);
    } else {
        value = static_cast<std::uint8_t>(value & static_cast<std::uint8_t>(~mask));
    }
    bitmap[byte_index] = static_cast<std::byte>(value);
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
write_bitmap_bytes(CompositeNtfsBlockDevice& device, const ntfs_core::BootGeometry& geometry,
                   const ntfs_core::AttributeValue& bitmap_data,
                   const std::span<const std::byte> bytes,
                   const base::CancellationToken cancellation) {
    if (!bitmap_data.non_resident) {
        // Resident $Bitmap is rare on large volumes; fail closed for SR6.
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    std::uint64_t position = 0;
    for (const auto& run : bitmap_data.runs) {
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

base::Result<BitmapCommitSummary>
commit_bitmap_for_relocations(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                              const ShrinkPlan& plan, const RecordClassFilter filter,
                              const base::CancellationToken cancellation) {
    if (!has_matching_relocation(plan, filter)) {
        return base::Result<BitmapCommitSummary>::success(BitmapCommitSummary{});
    }
    auto bitmap_record = mft_store.read_record_bytes(kFileNumberBitmap, cancellation);
    if (!bitmap_record) {
        return base::Result<BitmapCommitSummary>::failure(bitmap_record.error());
    }
    auto parsed = ntfs_core::parse_mft_record_bytes(bitmap_record.value(),
                                                    mft_store.geometry().bytes_per_sector,
                                                    kFileNumberBitmap);
    if (!parsed) {
        return base::Result<BitmapCommitSummary>::failure(parsed.error());
    }
    const ntfs_core::AttributeValue* bitmap_data = nullptr;
    for (const auto& attribute : parsed.value().attributes) {
        if (attribute.type == ntfs_core::kAttrData && attribute.name.empty()) {
            bitmap_data = &attribute;
            break;
        }
    }
    if (bitmap_data == nullptr) {
        return shrink_fail<BitmapCommitSummary>(base::ErrorCode::kCorruptData,
                                                "restore.shrink_unsupported_layout");
    }

    CompositeReaderAdapter reader(device);
    NtfsVolumeView view;
    view.reader = &reader;
    view.geometry = mft_store.geometry();
    view.mft_data = {}; // unused for payload read
    auto bytes = read_attribute_payload(view, *bitmap_data, kMaxBitmapLoadBytes, cancellation);
    if (!bytes) {
        return base::Result<BitmapCommitSummary>::failure(bytes.error());
    }

    BitmapCommitSummary summary;
    for (const auto& reloc : plan.relocation_records()) {
        if (!matches_record_filter(filter, reloc.mft_record_number)) {
            continue;
        }
        if (cancellation.stop_requested()) {
            return shrink_fail<BitmapCommitSummary>(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        for (auto lcn = reloc.target.begin_lcn; lcn < reloc.target.end_lcn; ++lcn) {
            auto status = set_bitmap_bit(bytes.value(), lcn, true);
            if (!status) {
                return base::Result<BitmapCommitSummary>::failure(status.error());
            }
            ++summary.bits_set;
        }
        for (auto lcn = reloc.source.begin_lcn; lcn < reloc.source.end_lcn; ++lcn) {
            auto status = set_bitmap_bit(bytes.value(), lcn, false);
            if (!status) {
                return base::Result<BitmapCommitSummary>::failure(status.error());
            }
            ++summary.bits_cleared;
        }
    }

    auto written = write_bitmap_bytes(device, mft_store.geometry(), *bitmap_data, bytes.value(),
                                      cancellation);
    if (!written) {
        return base::Result<BitmapCommitSummary>::failure(written.error());
    }
    return base::Result<BitmapCommitSummary>::success(summary);
}

} // namespace aegra::ntfs_resize::detail
