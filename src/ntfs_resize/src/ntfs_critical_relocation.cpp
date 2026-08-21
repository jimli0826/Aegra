#include "aegra/ntfs_resize/ntfs_critical_relocation.h"

#include "ntfs_bitmap_commit.h"
#include "ntfs_cluster_mover.h"
#include "ntfs_logfile_invalidation.h"
#include "ntfs_metadata_editor.h"
#include "ntfs_mft_mirror_sync.h"
#include "ntfs_record_class.h"
#include "ntfs_record_writer.h"
#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize {
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

[[nodiscard]] base::Result<void>
validate_request(const CriticalRelocationRequest& request) {
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
critical_relocations_sorted(const ShrinkPlan& plan) {
    std::vector<RelocationRecord> records;
    for (const auto& record : plan.relocation_records()) {
        if (detail::matches_record_filter(detail::RecordClassFilter::kCritical,
                                          record.mft_record_number)) {
            records.push_back(record);
        }
    }
    std::sort(records.begin(), records.end(),
              [](const RelocationRecord& left, const RelocationRecord& right) {
                  // Relocate $MFT before other critical records when plan_order ties.
                  if (left.plan_order != right.plan_order) {
                      return left.plan_order < right.plan_order;
                  }
                  return left.mft_record_number < right.mft_record_number;
              });
    return records;
}

} // namespace

base::Result<CriticalRelocationSummary>
NtfsCriticalRelocationExecutor::execute(const CriticalRelocationRequest& request,
                                        const base::CancellationToken cancellation) {
    if (auto valid = validate_request(request); !valid) {
        return base::Result<CriticalRelocationSummary>::failure(valid.error());
    }

    CompositeReaderAdapter reader(*request.device);
    auto view = detail::open_ntfs_volume_view(reader, request.plan->source_logical_size_bytes(),
                                              cancellation);
    if (!view) {
        return base::Result<CriticalRelocationSummary>::failure(view.error());
    }
    view.value().geometry = request.plan->source_ntfs_geometry();

    detail::MftRecordStore mft_store(*request.device, request.plan->source_ntfs_geometry(),
                                     view.value().mft_data);

    auto relocations = critical_relocations_sorted(*request.plan);
    CriticalRelocationSummary summary;
    CriticalRelocationProgress progress;
    progress.total_critical_relocations = relocations.size();

    // 1) Move critical clusters (including $MFT outbound extents) with verified copy.
    for (const auto& record : relocations) {
        if (cancellation.stop_requested()) {
            return detail::shrink_fail<CriticalRelocationSummary>(base::ErrorCode::kCancelled,
                                                                  "ntfs.read_failed");
        }
        auto moved = detail::move_relocation_extent(*request.device, view.value().geometry, record,
                                                    cancellation);
        if (!moved) {
            return base::Result<CriticalRelocationSummary>::failure(moved.error());
        }
        summary.verified_moved_bytes += moved.value();
        ++summary.relocation_count;
        progress.verified_moved_bytes = summary.verified_moved_bytes;
        progress.completed_relocations = summary.relocation_count;
        if (request.progress != nullptr) {
            request.progress->on_progress(progress);
        }
    }

    // 2) Update critical runlists. Prefer updating $MFT (record 0) before dependents.
    auto updated = detail::apply_runlist_mutations(
        mft_store, *request.plan, request.plan->new_total_cluster_count(),
        detail::RecordClassFilter::kCritical, cancellation);
    if (!updated) {
        return base::Result<CriticalRelocationSummary>::failure(updated.error());
    }
    summary.records_updated = updated.value();

    // 3) Bitmap for critical moves: allocate new then free old.
    auto bitmap = detail::commit_bitmap_for_relocations(
        *request.device, mft_store, *request.plan, detail::RecordClassFilter::kCritical,
        cancellation);
    if (!bitmap) {
        return base::Result<CriticalRelocationSummary>::failure(bitmap.error());
    }
    summary.bitmap_bits_set = bitmap.value().bits_set;
    summary.bitmap_bits_cleared = bitmap.value().bits_cleared;

    // 4) Invalidate $LogFile restart area (no Boot commit).
    auto logfile = detail::invalidate_logfile_restart_area(*request.device, mft_store, cancellation);
    if (!logfile) {
        return base::Result<CriticalRelocationSummary>::failure(logfile.error());
    }
    summary.logfile_invalidated = true;
    progress.logfile_invalidated = true;

    // 5) Sync $MFTMirr from the first FILE records.
    auto mirror = detail::sync_mft_mirror(*request.device, mft_store, cancellation);
    if (!mirror) {
        return base::Result<CriticalRelocationSummary>::failure(mirror.error());
    }
    summary.mft_mirror_synced = true;
    progress.mft_mirror_synced = true;
    if (request.progress != nullptr) {
        request.progress->on_progress(progress);
    }
    return base::Result<CriticalRelocationSummary>::success(summary);
}

} // namespace aegra::ntfs_resize
