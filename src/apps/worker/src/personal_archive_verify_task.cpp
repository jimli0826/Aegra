#include "aegra/apps/worker/personal_archive_verify_task.h"

#include "personal_archive_verify_task_backend.h"
#include "aegra/apps/worker/worker_task_log.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"
#include "aegra/pipeline/verify_pipeline.h"
#include "aegra/ports/progress.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
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
    if (job.operation != contracts::JobOperation::kVerify || job.source_refs.empty() ||
        job.source_refs.size() > contracts::kMaximumVerifyRecoveryPoints ||
        !job.target_ref.empty() || job.credential_refs.size() != job.source_refs.size() ||
        job.verify_recovery_point_ids.size() != job.source_refs.size()) {
        return invalid(
            "personal verify task requires matching sources, credentials and recovery point ids");
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
        return "verify.archive_missing";
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
    if (code == base::ErrorCode::kNotFound) {
        return "Archive file does not exist";
    }
    if (code == base::ErrorCode::kIoFailure) {
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
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = outcome;
    result.error_code = code;
    result.message_code = message_code_for(code);
    return result;
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const pipeline::VerifySummary& summary) {
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.logical_bytes = summary.logical_bytes;
    result.stored_bytes = summary.verified_bytes;
    result.chunk_count = summary.chunk_count;
    result.message_code = "verify.completed";
    return result;
}

base::Result<contracts::TaskResult> validated_result(contracts::TaskResult result) {
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override { return {}; }
};

[[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
resolve_verify_secret(const contracts::SecretRef& credential,
                      ports::ICredentialResolver& credentials,
                      const base::CancellationToken& cancellation) {
    // Empty SecretRef = unencrypted archive (empty password), matching restore and
    // file_set verify.
    if (credential.value.empty()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::success(
            std::make_unique<EmptyPasswordSecret>());
    }
    auto resolved = credentials.resolve(credential, cancellation);
    if (!resolved || resolved.value() == nullptr || resolved.value()->view().empty()) {
        const auto code = !resolved && resolved.error().code == base::ErrorCode::kCancelled
                              ? base::ErrorCode::kCancelled
                              : base::ErrorCode::kUnauthorized;
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            {code, !resolved ? resolved.error().message : "archive credential is unavailable"});
    }
    return resolved;
}

void publish_with_recovery_point(ports::IProgressSink* progress, contracts::TaskProgress event,
                                 std::string recovery_point_id) {
    if (progress == nullptr) {
        return;
    }
    event.recovery_point_id = std::move(recovery_point_id);
    progress->publish(event);
}

void publish_preparing(const contracts::JobRequest& job, ports::IProgressSink* progress) {
    const auto current =
        job.verify_recovery_point_ids.empty() ? std::string{} : job.verify_recovery_point_ids.front();
    publish_with_recovery_point(
        progress,
        contracts::make_byte_progress(job.job_id, job.trace_id, contracts::TaskPhase::kPreparing, 0,
                                      0, 0, "verify.preparing"),
        current);
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
    log->field_u64("recovery_point_count", job.source_refs.size());
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

[[nodiscard]] base::Result<pipeline::VerifySummary>
add_verify_summary(pipeline::VerifySummary total, const pipeline::VerifySummary& item) {
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    if (item.verified_bytes > max - total.verified_bytes ||
        item.chunk_count > max - total.chunk_count) {
        return base::Result<pipeline::VerifySummary>::failure(
            {base::ErrorCode::kInvalidArgument, "verify totals overflow"});
    }
    // Same backup set shares one volume logical size; summing it N times is not capacity.
    total.logical_bytes = (std::max)(total.logical_bytes, item.logical_bytes);
    total.verified_bytes += item.verified_bytes;
    total.chunk_count += item.chunk_count;
    return base::Result<pipeline::VerifySummary>::success(total);
}

class StampedProgressSink final : public ports::IProgressSink {
  public:
    StampedProgressSink(ports::IProgressSink* inner, std::string recovery_point_id)
        : inner_(inner), recovery_point_id_(std::move(recovery_point_id)) {}

    void publish(const contracts::TaskProgress& progress) noexcept override {
        if (inner_ == nullptr) {
            return;
        }
        auto stamped = progress;
        stamped.recovery_point_id = recovery_point_id_;
        inner_->publish(stamped);
    }

  private:
    ports::IProgressSink* inner_{nullptr};
    std::string recovery_point_id_;
};

struct VolumeVerifyCall final {
    const contracts::JobRequest* job{nullptr};
    const WindowsPersonalBackupTaskOptions* options{nullptr};
    const WindowsPersonalBackupTaskContext* context{nullptr};
    IPersonalArchiveVerifyTaskBackend* backend{nullptr};
};

[[nodiscard]] base::Result<pipeline::VerifySummary>
verify_volume_item(const VolumeVerifyCall& call, const std::size_t index,
                   const base::CancellationToken& cancellation) {
    const auto& job = *call.job;
    const auto& recovery_point_id = job.verify_recovery_point_ids[index];
    if (auto* log = WorkerTaskLog::active()) {
        log->section("RecoveryPoint");
        log->field_u64("index", index + 1);
        log->field_u64("count", job.source_refs.size());
        log->field("recovery_point_id", recovery_point_id);
        log->field("source", job.source_refs[index]);
    }
    publish_with_recovery_point(
        call.context->progress,
        contracts::make_byte_progress(job.job_id, job.trace_id, contracts::TaskPhase::kReading, 0, 0,
                                      0, "verify.reading"),
        recovery_point_id);
    auto resolved =
        resolve_verify_secret(job.credential_refs[index], call.context->credentials, cancellation);
    if (!resolved) {
        return base::Result<pipeline::VerifySummary>::failure(resolved.error());
    }
    StampedProgressSink stamped(call.context->progress, recovery_point_id);
    return call.backend->run(path_from_utf8(job.source_refs[index]), resolved.value()->view(),
                            {job.job_id, job.trace_id}, *call.options, cancellation, &stamped);
}

[[nodiscard]] base::Result<pipeline::VerifySummary>
verify_volume_batch(const VolumeVerifyCall& call, const base::CancellationToken& cancellation) {
    pipeline::VerifySummary totals;
    std::optional<base::Error> first_error;
    std::string first_failed_id;
    for (std::size_t index = 0; index < call.job->source_refs.size(); ++index) {
        if (cancellation.stop_requested()) {
            return base::Result<pipeline::VerifySummary>::failure(
                {base::ErrorCode::kCancelled, "verify cancelled"});
        }
        auto verified = verify_volume_item(call, index, cancellation);
        if (!verified) {
            if (!first_error) {
                first_error = verified.error();
                first_failed_id = call.job->verify_recovery_point_ids[index];
            }
            continue;
        }
        auto added = add_verify_summary(totals, verified.value());
        if (!added) {
            return added;
        }
        totals = std::move(added).value();
    }
    if (first_error) {
        publish_with_recovery_point(
            call.context->progress,
            contracts::make_byte_progress(call.job->job_id, call.job->trace_id,
                                          contracts::TaskPhase::kReading, 0, 0, 0,
                                          message_code_for(first_error->code)),
            first_failed_id);
        return base::Result<pipeline::VerifySummary>::failure(*first_error);
    }
    return base::Result<pipeline::VerifySummary>::success(totals);
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

    const VolumeVerifyCall call{&job, &options, &context, &backend};
    auto verified = verify_volume_batch(call, cancellation);
    if (!verified) {
        auto result = validated_result(failed_result(job, verified.error().code));
        if (result) {
            log_verify_result(task_log.get(), result.value(), &verified.error(), started);
        }
        return result;
    }
    const auto& totals = verified.value();
    auto completed = validated_result(completed_result(job, totals));
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
