#include "aegra/apps/worker/personal_archive_verify_task.h"

#include "personal_archive_verify_task_backend.h"
#include "worker_task_log.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"
#include "aegra/pipeline/verify_pipeline.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
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

[[nodiscard]] std::string_view verify_hint_for(const base::ErrorCode code,
                                               const std::string_view message) noexcept {
    if (message.find("password") != std::string_view::npos ||
        code == base::ErrorCode::kUnauthorized) {
        return "Re-enter the archive password and retry";
    }
    if (code == base::ErrorCode::kCorruptData) {
        return "Archive authentication failed; re-backup or pick another recovery point";
    }
    if (code == base::ErrorCode::kNotFound || code == base::ErrorCode::kIoFailure) {
        return "Check archive path, repository connectivity, and file permissions";
    }
    if (code == base::ErrorCode::kCancelled) {
        return "Job was cancelled or deadline expired";
    }
    return "Inspect error_message and retry after fixing the reported condition";
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

void log_verify_request(WorkerTaskLog* log, const contracts::JobRequest& job,
                        const WindowsPersonalBackupTaskOptions& options) {
    if (log == nullptr) {
        return;
    }
    log->section("Job");
    log->field("job_id", job.job_id);
    log->field("trace_id", job.trace_id);
    log->field("operation", "verify");

    log->section("Request");
    log->field("source", job.source_refs.front());
    log->field_bytes("memory_budget", options.memory_budget_bytes);
    log->field("password", job.credential_refs.front().value.empty() ? "empty" : "present");
}

void log_verify_result(WorkerTaskLog* log, const contracts::TaskResult& result,
                       const base::Error* error,
                       const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    if (result.outcome == contracts::TaskOutcome::kSucceeded) {
        log->field("outcome", "succeeded");
        log->field("message_code", result.message_code);
        log->field_bytes("logical_bytes", result.logical_bytes);
        log->field_bytes("verified_bytes", result.stored_bytes);
        log->field_u64("chunks", result.chunk_count);
        log->field("elapsed", format_duration_ms(elapsed));
        return;
    }
    log->field("outcome",
               result.outcome == contracts::TaskOutcome::kCancelled ? "cancelled" : "failed");
    log->field("message_code", result.message_code);
    if (error != nullptr) {
        log->field("error_code", base::error_code_name(error->code));
        if (!error->message.empty()) {
            log->field("error_message", error->message);
        }
        const auto hint = verify_hint_for(error->code, error->message);
        if (!hint.empty()) {
            log->field("hint", hint);
        }
    }
    log->field("elapsed", format_duration_ms(elapsed));
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job,
                  const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IPersonalArchiveVerifyTaskBackend& backend) {
    const auto started = std::chrono::steady_clock::now();
    auto task_log = WorkerTaskLog::open("verify", job.job_id);
    WorkerTaskLogScope log_scope(task_log.get());
    log_verify_request(task_log.get(), job, options);
    publish_preparing(job, context.progress);

    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        const base::Error error{base::ErrorCode::kCancelled, "verify cancelled before start"};
        auto result = validated_result(failed_result(job, base::ErrorCode::kCancelled));
        if (result) {
            log_verify_result(task_log.get(), result.value(), &error, started);
        }
        return result;
    }

    std::unique_ptr<ports::IResolvedSecret> secret;
    {
        ScopedStage stage(task_log.get(), "resolve_credentials");
        auto resolved = context.credentials.resolve(job.credential_refs.front(), cancellation);
        if (!resolved || resolved.value() == nullptr || resolved.value()->view().empty()) {
            const auto code = !resolved && resolved.error().code == base::ErrorCode::kCancelled
                                  ? base::ErrorCode::kCancelled
                                  : base::ErrorCode::kUnauthorized;
            const base::Error error{code, !resolved ? resolved.error().message
                                                    : "archive credential is unavailable"};
            stage.fail(error, "resolve_secret", verify_hint_for(code, error.message));
            auto result = validated_result(failed_result(job, code));
            if (result) {
                log_verify_result(task_log.get(), result.value(), &error, started);
            }
            return result;
        }
        stage.note("password", "present");
        secret = std::move(resolved).value();
    }

    auto verified = backend.run(path_from_utf8(job.source_refs.front()), secret->view(),
                                {job.job_id, job.trace_id}, options, cancellation, context.progress);
    if (!verified) {
        auto result = validated_result(failed_result(job, verified.error().code));
        if (result) {
            log_verify_result(task_log.get(), result.value(), &verified.error(), started);
        }
        return result;
    }
    auto completed = validated_result(completed_result(job, verified.value()));
    if (completed) {
        log_verify_result(task_log.get(), completed.value(), nullptr, started);
    }
    return completed;
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
