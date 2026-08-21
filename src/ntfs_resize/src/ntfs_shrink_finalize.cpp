#include "aegra/ntfs_resize/ntfs_shrink_finalize.h"

#include "ntfs_record_writer.h"
#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/mft_record.h"

#include <utility>

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

[[nodiscard]] ShrinkFinalizeResult
make_result(const ShrinkFinalizeOutcome outcome, const ShrinkFinalizePhase phase,
            std::string message_code, std::string failure_detail = {}) {
    ShrinkFinalizeResult result;
    result.outcome = outcome;
    result.reached = phase;
    result.message_code = std::move(message_code);
    result.failure_detail = std::move(failure_detail);
    return result;
}

[[nodiscard]] base::Result<void>
readback_critical_records(CompositeNtfsBlockDevice& composite, const ShrinkPlan& plan,
                          const base::CancellationToken cancellation) {
    CompositeReaderAdapter reader(composite);
    auto view =
        detail::open_ntfs_volume_view(reader, plan.source_logical_size_bytes(), cancellation);
    if (!view) {
        return base::Result<void>::failure(view.error());
    }
    detail::MftRecordStore store(composite, plan.source_ntfs_geometry(), view.value().mft_data);
    for (const std::uint64_t record_number : {std::uint64_t{0}, std::uint64_t{1},
                                              std::uint64_t{3}, std::uint64_t{6}}) {
        auto bytes = store.read_record_bytes(record_number, cancellation);
        if (!bytes) {
            return base::Result<void>::failure(bytes.error());
        }
        auto parsed = ntfs_core::parse_mft_record_bytes(bytes.value(),
                                                        plan.source_ntfs_geometry().bytes_per_sector,
                                                        record_number);
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool chkdsk_allows_continue(const ChkdskMappedResult mapped) noexcept {
    return mapped == ChkdskMappedResult::kClean || mapped == ChkdskMappedResult::kFixed ||
           mapped == ChkdskMappedResult::kCleanupWarning;
}

} // namespace

base::Result<ShrinkFinalizeResult> NtfsShrinkFinalizeExecutor::execute_locked_commit(
    const ShrinkFinalizeRequest& request, const base::CancellationToken cancellation) {
    if (request.plan == nullptr || request.target == nullptr || request.composite == nullptr) {
        return detail::shrink_fail<ShrinkFinalizeResult>(base::ErrorCode::kInvalidArgument,
                                                         "restore.shrink_plan_corrupt");
    }

    // 1) Flush all pending composite/target writes.
    {
        auto flushed = request.composite->flush(base::CancellationToken{});
        if (!flushed) {
            return base::Result<ShrinkFinalizeResult>::success(make_result(
                ShrinkFinalizeOutcome::kTargetIncomplete, ShrinkFinalizePhase::kFlushing,
                "restore.shrink_target_incomplete",
                "step=flush error=" + flushed.error().message));
        }
    }

    // 2) Readback critical MFT / mirror / volume / bitmap records.
    {
        auto readback =
            readback_critical_records(*request.composite, *request.plan, base::CancellationToken{});
        if (!readback) {
            return base::Result<ShrinkFinalizeResult>::success(make_result(
                ShrinkFinalizeOutcome::kTargetIncomplete, ShrinkFinalizePhase::kReadbackCritical,
                "restore.shrink_target_incomplete",
                "step=readback_critical error=" + readback.error().message));
        }
    }

    // 3-6) Backup Boot then Primary Boot commit.
    BootCommitRequest boot_request;
    boot_request.plan = request.plan;
    boot_request.target = request.target;
    boot_request.composite = request.composite;
    auto boot = NtfsBootCommitExecutor::execute(boot_request, cancellation);
    if (!boot) {
        return base::Result<ShrinkFinalizeResult>::failure(boot.error());
    }
    ShrinkFinalizeResult result;
    result.boot = std::move(boot).value();
    if (result.boot.status == BootCommitStatus::kOutcomeUnknown) {
        result.outcome = ShrinkFinalizeOutcome::kCommitOutcomeUnknown;
        result.reached = ShrinkFinalizePhase::kCommittingPrimaryBoot;
        result.message_code = "restore.shrink_commit_outcome_unknown";
        return base::Result<ShrinkFinalizeResult>::success(std::move(result));
    }
    if (result.boot.status != BootCommitStatus::kSuccess) {
        result.outcome = ShrinkFinalizeOutcome::kTargetIncomplete;
        result.reached = result.boot.reached == BootCommitStage::kBackupCommitted
                             ? ShrinkFinalizePhase::kCommittingBackupBoot
                             : ShrinkFinalizePhase::kCommittingPrimaryBoot;
        result.message_code = "restore.shrink_target_incomplete";
        return base::Result<ShrinkFinalizeResult>::success(std::move(result));
    }
    result.outcome = ShrinkFinalizeOutcome::kReadyForPostcheck;
    result.reached = ShrinkFinalizePhase::kCommittingPrimaryBoot;
    result.message_code = "restore.shrink_postcheck_pending";
    return base::Result<ShrinkFinalizeResult>::success(std::move(result));
}

base::Result<ShrinkFinalizeResult> NtfsShrinkFinalizeExecutor::execute_postcommit(
    const ShrinkPostcommitRequest& request, ShrinkFinalizeResult result,
    const base::CancellationToken cancellation) {
    if (request.plan == nullptr || request.postcheck == nullptr ||
        result.outcome != ShrinkFinalizeOutcome::kReadyForPostcheck) {
        return detail::shrink_fail<ShrinkFinalizeResult>(base::ErrorCode::kInvalidArgument,
                                                         "restore.shrink_plan_corrupt");
    }

    // CHKDSK is optional: it runs only when the Worker provides an executable path. The Worker
    // has destroyed the locked raw target before this call. Cancel does not kill CHKDSK.
    if (!request.chkdsk_executable_path.empty()) {
        if (request.process_launcher == nullptr || request.volume_name.empty()) {
            return detail::shrink_fail<ShrinkFinalizeResult>(base::ErrorCode::kInvalidArgument,
                                                             "restore.shrink_plan_corrupt");
        }
        ChkdskRunRequest chkdsk_request;
        chkdsk_request.launcher = request.process_launcher;
        chkdsk_request.chkdsk_executable_path = request.chkdsk_executable_path;
        chkdsk_request.volume_name = request.volume_name;
        auto chkdsk = run_shrink_chkdsk(chkdsk_request, cancellation);
        if (!chkdsk) {
            result.outcome = ShrinkFinalizeOutcome::kPostcheckFailed;
            result.reached = ShrinkFinalizePhase::kRunningChkdsk;
            result.message_code = "restore.shrink_postcheck_failed";
            result.failure_detail = "step=chkdsk_launch error=" + chkdsk.error().message;
            return base::Result<ShrinkFinalizeResult>::success(std::move(result));
        }
        result.chkdsk = std::move(chkdsk).value();
        result.reached = ShrinkFinalizePhase::kRunningChkdsk;
        if (result.chkdsk.mapped == ChkdskMappedResult::kOutcomeUnknown) {
            result.outcome = ShrinkFinalizeOutcome::kCommitOutcomeUnknown;
            result.message_code = "restore.shrink_commit_outcome_unknown";
            result.failure_detail =
                "step=chkdsk_wait terminated=" +
                std::string(result.chkdsk.process_terminated ? "true" : "false");
            return base::Result<ShrinkFinalizeResult>::success(std::move(result));
        }
        if (!chkdsk_allows_continue(result.chkdsk.mapped)) {
            result.outcome = ShrinkFinalizeOutcome::kPostcheckFailed;
            result.message_code = "restore.shrink_postcheck_failed";
            result.failure_detail =
                "step=chkdsk_result exit_code=" + std::to_string(result.chkdsk.exit_code);
            return base::Result<ShrinkFinalizeResult>::success(std::move(result));
        }
    }

    // 10) Volume dirty/mount/geometry postcheck via Worker-provided probe.
    auto post = request.postcheck->query(base::CancellationToken{});
    result.reached = ShrinkFinalizePhase::kPostchecking;
    if (!post || !post.value().query_succeeded || post.value().volume_dirty ||
        post.value().capacity_bytes != request.plan->target_capacity_bytes()) {
        result.outcome = ShrinkFinalizeOutcome::kPostcheckFailed;
        result.message_code = "restore.shrink_postcheck_failed";
        if (!post) {
            result.failure_detail = "step=postcheck_query error=" + post.error().message;
        } else {
            result.failure_detail =
                "step=postcheck query_succeeded=" +
                std::string(post.value().query_succeeded ? "true" : "false") +
                " volume_dirty=" + std::string(post.value().volume_dirty ? "true" : "false") +
                " capacity=" + std::to_string(post.value().capacity_bytes) +
                " expected_capacity=" + std::to_string(request.plan->target_capacity_bytes());
        }
        return base::Result<ShrinkFinalizeResult>::success(std::move(result));
    }
    if (post.value().bytes_per_sector != 0 &&
        post.value().bytes_per_sector !=
            request.plan->source_ntfs_geometry().bytes_per_sector) {
        result.outcome = ShrinkFinalizeOutcome::kPostcheckFailed;
        result.message_code = "restore.shrink_postcheck_failed";
        result.failure_detail =
            "step=postcheck_sector bytes_per_sector=" +
            std::to_string(post.value().bytes_per_sector) + " expected=" +
            std::to_string(request.plan->source_ntfs_geometry().bytes_per_sector);
        return base::Result<ShrinkFinalizeResult>::success(std::move(result));
    }

    result.outcome = ShrinkFinalizeOutcome::kCompleted;
    result.reached = ShrinkFinalizePhase::kCompleted;
    result.message_code = "restore.shrink_completed";
    return base::Result<ShrinkFinalizeResult>::success(std::move(result));
}

} // namespace aegra::ntfs_resize
