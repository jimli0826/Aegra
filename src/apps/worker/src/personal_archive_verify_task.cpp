#include "aegra/apps/worker/personal_archive_verify_task.h"

#include "personal_archive_verify_task_backend.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"
#include "aegra/pipeline/verify_pipeline.h"

#include <filesystem>
#include <string>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task(const contracts::JobRequest& job,
                                 const WindowsPersonalBackupTaskOptions& options) {
    auto valid_job = contracts::validate_job_request(job);
    if (!valid_job) {
        return valid_job;
    }
    if (job.operation != contracts::JobOperation::kVerify || job.source_refs.size() != 1 ||
        !job.target_ref.empty() || job.credential_refs.size() != 1) {
        return invalid("personal verify task requires one source, no target and one credential");
    }
    if (options.memory_budget_bytes == 0) {
        return invalid("personal verify task memory budget is invalid");
    }
    return base::Result<void>::success();
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

const char* message_code_for(const base::ErrorCode code) noexcept {
    switch (code) {
    case base::ErrorCode::kCancelled:
        return "verify.cancelled";
    case base::ErrorCode::kUnauthorized:
        return "verify.credential_unavailable";
    case base::ErrorCode::kNotFound:
    case base::ErrorCode::kIoFailure:
        return "verify.source_unavailable";
    case base::ErrorCode::kCorruptData:
        return "verify.corrupt";
    case base::ErrorCode::kInvalidArgument:
        return "verify.invalid_request";
    default:
        return "verify.failed";
    }
}

contracts::TaskResult failed_result(const contracts::JobRequest& job,
                                    const base::ErrorCode code) {
    const auto outcome = code == base::ErrorCode::kCancelled ? contracts::TaskOutcome::kCancelled
                                                             : contracts::TaskOutcome::kFailed;
    return {contracts::kTaskResultSchemaVersion, job.job_id, job.trace_id, outcome, code, 0, 0, 0,
            message_code_for(code), {}};
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const pipeline::VerifySummary& summary) {
    return {contracts::kTaskResultSchemaVersion,
            job.job_id,
            job.trace_id,
            contracts::TaskOutcome::kSucceeded,
            base::ErrorCode::kNone,
            summary.logical_bytes,
            summary.verified_bytes,
            summary.chunk_count,
            "verify.completed",
            {}};
}

base::Result<contracts::TaskResult> validated_result(contracts::TaskResult result) {
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

void publish_preparing(const contracts::JobRequest& job, ports::IProgressSink* progress) {
    if (progress != nullptr) {
        progress->publish({contracts::kTaskProgressSchemaVersion, job.job_id, job.trace_id,
                           contracts::TaskPhase::kPreparing, 0, 0, 0, "verify.preparing"});
    }
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job,
                  const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IPersonalArchiveVerifyTaskBackend& backend) {
    publish_preparing(job, context.progress);
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    auto secret = context.credentials.resolve(job.credential_refs.front(), cancellation);
    if (!secret || secret.value() == nullptr || secret.value()->view().empty()) {
        const auto code = !secret && secret.error().code == base::ErrorCode::kCancelled
                              ? base::ErrorCode::kCancelled
                              : base::ErrorCode::kUnauthorized;
        return validated_result(failed_result(job, code));
    }
    auto result = backend.run(path_from_utf8(job.source_refs.front()), secret.value()->view(),
                              {job.job_id, job.trace_id}, options, cancellation, context.progress);
    if (!result) {
        return validated_result(failed_result(job, result.error().code));
    }
    return validated_result(completed_result(job, result.value()));
}

} // namespace

base::Result<contracts::TaskResult> execute_personal_archive_verify_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation, IPersonalArchiveVerifyTaskBackend& backend) {
    auto validation = validate_task(job, options);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return run_accepted_task(job, options, context, cancellation, backend);
}

} // namespace detail

base::Result<contracts::TaskResult> execute_personal_archive_verify_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation) {
    try {
        auto backend = detail::make_personal_archive_verify_task_backend();
        return detail::execute_personal_archive_verify_task_with_backend(
            job, options, context, cancellation, *backend);
    } catch (...) {
        return base::Result<contracts::TaskResult>::failure(
            {base::ErrorCode::kInternal, "personal archive verify entry failed unexpectedly"});
    }
}

} // namespace aegra::apps::worker
