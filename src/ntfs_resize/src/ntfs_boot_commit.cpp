#include "aegra/ntfs_resize/ntfs_boot_commit.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/boot_sector.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] base::Result<void>
write_boot_verified(ports::IRandomAccessBlockDevice& target, const std::uint64_t offset,
                    const std::span<const std::byte> boot,
                    const base::CancellationToken cancellation) {
    auto written = target.write_at(offset, boot, cancellation);
    if (!written) {
        return written;
    }
    auto flushed = target.flush(cancellation);
    if (!flushed) {
        return flushed;
    }
    std::vector<std::byte> readback(boot.size());
    auto read = target.read_at(offset, readback, cancellation);
    if (!read) {
        return base::Result<void>::failure(read.error());
    }
    if (read.value() != boot.size() ||
        !std::equal(boot.begin(), boot.end(), readback.begin())) {
        return base::Result<void>::failure(
            {base::ErrorCode::kOutcomeUnknown, "restore.shrink_commit_outcome_unknown"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<std::byte>>
build_committed_boot(const BootCommitRequest& request, const base::CancellationToken cancellation) {
    const auto sector = request.plan->source_ntfs_geometry().bytes_per_sector;
    if (sector == 0) {
        return detail::shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                           "restore.shrink_unsupported_layout");
    }
    std::vector<std::byte> original(sector);
    auto read = request.composite->read_at(0, original, cancellation);
    if (!read || read.value() != original.size()) {
        return detail::shrink_fail<std::vector<std::byte>>(
            base::ErrorCode::kIoFailure, "restore.shrink_target_incomplete");
    }
    return ntfs_core::patch_boot_geometry(
        original, request.plan->new_total_sector_count(), request.plan->new_mft_start_lcn(),
        request.plan->new_mft_mirror_start_lcn());
}

} // namespace

base::Result<BootCommitResult>
NtfsBootCommitExecutor::execute(const BootCommitRequest& request,
                                const base::CancellationToken cancellation) {
    BootCommitResult result;
    result.status = BootCommitStatus::kTargetIncomplete;
    result.reached = BootCommitStage::kPrepared;
    result.message_code = "restore.shrink_target_incomplete";

    if (request.plan == nullptr || request.target == nullptr || request.composite == nullptr) {
        return detail::shrink_fail<BootCommitResult>(base::ErrorCode::kInvalidArgument,
                                                     "restore.shrink_plan_corrupt");
    }
    const auto sector = request.plan->source_ntfs_geometry().bytes_per_sector;
    const auto capacity = request.target->geometry().capacity_bytes;
    if (sector == 0 || capacity < static_cast<std::uint64_t>(sector) * 2U ||
        capacity % sector != 0 || capacity != request.plan->target_capacity_bytes()) {
        return detail::shrink_fail<BootCommitResult>(base::ErrorCode::kConflict,
                                                     "restore.shrink_plan_changed");
    }

    // Cancel is deferred: finish current stable step, then return incomplete/unknown.
    auto boot = build_committed_boot(request, base::CancellationToken{});
    if (!boot) {
        result.message_code = boot.error().message;
        if (boot.error().code == base::ErrorCode::kOutcomeUnknown) {
            result.status = BootCommitStatus::kOutcomeUnknown;
            result.message_code = "restore.shrink_commit_outcome_unknown";
        }
        return base::Result<BootCommitResult>::success(std::move(result));
    }
    result.committed_boot = std::move(boot).value();

    const auto backup_offset = capacity - sector;
    auto backup = write_boot_verified(*request.target, backup_offset, result.committed_boot,
                                      base::CancellationToken{});
    if (!backup) {
        result.reached = BootCommitStage::kPrepared;
        if (backup.error().code == base::ErrorCode::kOutcomeUnknown) {
            result.status = BootCommitStatus::kOutcomeUnknown;
            result.message_code = "restore.shrink_commit_outcome_unknown";
        } else {
            result.status = BootCommitStatus::kTargetIncomplete;
            result.message_code = "restore.shrink_target_incomplete";
        }
        return base::Result<BootCommitResult>::success(std::move(result));
    }
    result.reached = BootCommitStage::kBackupCommitted;

    auto primary =
        write_boot_verified(*request.target, 0, result.committed_boot, base::CancellationToken{});
    if (!primary) {
        // Backup already committed; primary uncertain or failed.
        result.reached = BootCommitStage::kBackupCommitted;
        if (primary.error().code == base::ErrorCode::kOutcomeUnknown ||
            primary.error().code == base::ErrorCode::kIoFailure) {
            result.status = BootCommitStatus::kOutcomeUnknown;
            result.message_code = "restore.shrink_commit_outcome_unknown";
        } else {
            result.status = BootCommitStatus::kTargetIncomplete;
            result.message_code = "restore.shrink_target_incomplete";
        }
        return base::Result<BootCommitResult>::success(std::move(result));
    }

    result.reached = BootCommitStage::kPrimaryCommitted;
    result.status = BootCommitStatus::kSuccess;
    result.message_code = "restore.shrink_boot_committed";
    if (cancellation.stop_requested()) {
        // Stable success already reached; surface cancel only as informational via message.
        result.message_code = "restore.shrink_boot_committed";
    }
    return base::Result<BootCommitResult>::success(std::move(result));
}

} // namespace aegra::ntfs_resize
