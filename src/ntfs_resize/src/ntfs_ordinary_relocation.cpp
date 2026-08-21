#include "aegra/ntfs_resize/ntfs_ordinary_relocation.h"

#include "ntfs_bitmap_commit.h"
#include "ntfs_cluster_mover.h"
#include "ntfs_metadata_editor.h"
#include "ntfs_record_writer.h"
#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/mft_record.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] bool is_ordinary_record(const std::uint64_t record_number) noexcept {
    return record_number > 11U;
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
validate_request(const OrdinaryRelocationRequest& request) {
    if (request.plan == nullptr || request.device == nullptr) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "restore.shrink_plan_corrupt");
    }
    if (request.plan->plan_version() != kShrinkPlanVersion) {
        return detail::shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                        "restore.shrink_unsupported_layout");
    }
    if (request.device->source_logical_size_bytes() != request.plan->source_logical_size_bytes() ||
        request.device->target_capacity_bytes() != request.plan->target_capacity_bytes()) {
        return detail::shrink_fail_void(base::ErrorCode::kConflict, "restore.shrink_plan_changed");
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::vector<RelocationRecord>
ordinary_relocations_sorted(const ShrinkPlan& plan) {
    std::vector<RelocationRecord> records;
    for (const auto& record : plan.relocation_records()) {
        if (is_ordinary_record(record.mft_record_number)) {
            records.push_back(record);
        }
    }
    std::sort(records.begin(), records.end(),
              [](const RelocationRecord& left, const RelocationRecord& right) {
                  return left.plan_order < right.plan_order;
              });
    return records;
}

} // namespace

base::Result<OrdinaryRelocationSummary>
NtfsOrdinaryRelocationExecutor::execute(const OrdinaryRelocationRequest& request,
                                        const base::CancellationToken cancellation) {
    if (auto valid = validate_request(request); !valid) {
        return base::Result<OrdinaryRelocationSummary>::failure(valid.error());
    }

    CompositeReaderAdapter reader(*request.device);
    auto view = detail::open_ntfs_volume_view(reader, request.plan->source_logical_size_bytes(),
                                              cancellation);
    if (!view) {
        return base::Result<OrdinaryRelocationSummary>::failure(view.error());
    }
    // Prefer geometry frozen in the plan for allocation math consistency.
    view.value().geometry = request.plan->source_ntfs_geometry();

    detail::MftRecordStore mft_store(*request.device, request.plan->source_ntfs_geometry(),
                                     view.value().mft_data);

    auto relocations = ordinary_relocations_sorted(*request.plan);
    OrdinaryRelocationSummary summary;
    OrdinaryRelocationProgress progress;
    progress.total_ordinary_relocations = relocations.size();

    for (const auto& record : relocations) {
        if (cancellation.stop_requested()) {
            return detail::shrink_fail<OrdinaryRelocationSummary>(base::ErrorCode::kCancelled,
                                                                  "ntfs.read_failed");
        }
        auto moved = detail::move_relocation_extent(*request.device, view.value().geometry, record,
                                                    cancellation);
        if (!moved) {
            return base::Result<OrdinaryRelocationSummary>::failure(moved.error());
        }
        summary.verified_moved_bytes += moved.value();
        ++summary.relocation_count;
        progress.verified_moved_bytes = summary.verified_moved_bytes;
        progress.completed_relocations = summary.relocation_count;
        if (request.progress != nullptr) {
            request.progress->on_progress(progress);
        }
    }

    auto updated = detail::apply_ordinary_runlist_mutations(
        mft_store, *request.plan, request.plan->new_total_cluster_count(), cancellation);
    if (!updated) {
        return base::Result<OrdinaryRelocationSummary>::failure(updated.error());
    }
    summary.records_updated = updated.value();

    auto bitmap =
        detail::commit_ordinary_bitmap(*request.device, mft_store, *request.plan, cancellation);
    if (!bitmap) {
        return base::Result<OrdinaryRelocationSummary>::failure(bitmap.error());
    }
    summary.bitmap_bits_set = bitmap.value().bits_set;
    summary.bitmap_bits_cleared = bitmap.value().bits_cleared;
    return base::Result<OrdinaryRelocationSummary>::success(summary);
}

} // namespace aegra::ntfs_resize
