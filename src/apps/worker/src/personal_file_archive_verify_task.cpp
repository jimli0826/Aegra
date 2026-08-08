#include "aegra/apps/worker/personal_file_archive_verify_task.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/progress.h"
#include "aegra/ports/file_recovery_point.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    if (job.content_kind != contracts::ContentKind::kFileSet ||
        job.operation != contracts::JobOperation::kVerify || job.source_refs.empty() ||
        job.source_refs.size() > contracts::kMaximumFileChainDepth || !job.target_ref.empty() ||
        job.credential_refs.size() != job.source_refs.size()) {
        return invalid("file_set verify requires content_kind=file_set, base-first source chain, "
                       "matching credentials, no target");
    }
    if (options.memory_budget_bytes == 0) {
        return invalid("file_set verify memory budget is invalid");
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

contracts::TaskResult failed_result(const contracts::JobRequest& job, const base::ErrorCode code) {
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
                                       const adapters::personal_archive::FileChainVerifyResult& totals) {
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.logical_bytes = totals.tip_resolved_bytes;
    result.stored_bytes = totals.local_payload_bytes;
    result.chunk_count = totals.tip_stream_count;
    result.entry_count = totals.tip_entry_count;
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

void publish_progress(const contracts::JobRequest& job, ports::IProgressSink* progress,
                      const contracts::TaskPhase phase, const std::uint64_t verified_bytes,
                      const std::uint64_t logical_bytes, const std::string& message_code) {
    if (progress == nullptr) {
        return;
    }
    progress->publish(contracts::make_byte_progress(job.job_id, job.trace_id, phase, logical_bytes,
                                                    verified_bytes, verified_bytes, message_code));
}

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override {
        return {};
    }
};

[[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
resolve_one_secret(const contracts::SecretRef& credential, ports::ICredentialResolver& credentials,
                   const base::CancellationToken& cancellation) {
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

[[nodiscard]] base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>
resolve_verify_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                       const base::CancellationToken& cancellation) {
    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    secrets.reserve(job.credential_refs.size());
    for (const auto& credential : job.credential_refs) {
        auto resolved = resolve_one_secret(credential, credentials, cancellation);
        if (!resolved) {
            return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::failure(
                resolved.error());
        }
        secrets.push_back(std::move(resolved).value());
    }
    return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::success(
        std::move(secrets));
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
    log->field("content_kind", "file_set");
    log->field_u64("layers", job.source_refs.size());
    log->field_bytes("memory_budget", options.memory_budget_bytes);
    log->field("password", job.credential_refs.front().value.empty() ? "empty" : "present");
}

void log_verify_success(WorkerTaskLog* log, const contracts::TaskResult& result,
                        const adapters::personal_archive::FileChainVerifyResult& totals,
                        const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    log->field("outcome", "succeeded");
    log->field_u64("layers", totals.layer_count);
    log->field_u64("tip_entries", totals.tip_entry_count);
    log->field_u64("tip_streams", totals.tip_stream_count);
    log->field_bytes("logical_bytes", result.logical_bytes);
    log->field_bytes("local_payload_bytes", totals.local_payload_bytes);
    log->field("elapsed", format_duration_ms(elapsed));
}

[[nodiscard]] base::Result<contracts::TaskResult>
run_file_set_verify(const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
                    const WindowsPersonalBackupTaskContext& context,
                    const base::CancellationToken& cancellation) {
    auto task_log = WorkerTaskLog::open("verify", job.job_id);
    const auto started = std::chrono::steady_clock::now();
    log_verify_request(task_log.get(), job, options);
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    publish_progress(job, context.progress, contracts::TaskPhase::kPreparing, 0, 0,
                     "verify.preparing");

    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    {
        ScopedStage stage(task_log.get(), "resolve_credentials");
        auto resolved = resolve_verify_secrets(job, context.credentials, cancellation);
        if (!resolved) {
            stage.fail(resolved.error(), "resolve_secret",
                       verify_hint_for(resolved.error().code, resolved.error().message));
            return validated_result(failed_result(job, resolved.error().code));
        }
        stage.note_u64("layers", resolved.value().size());
        secrets = std::move(resolved).value();
    }

    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveChainReader> chain;
    {
        ScopedStage stage(task_log.get(), "open_chain");
        adapters::personal_archive::ArchiveChainOpenRequest open_request;
        open_request.maximum_chain_depth = contracts::kMaximumFileChainDepth;
        open_request.layers.reserve(job.source_refs.size());
        for (std::size_t index = 0; index < job.source_refs.size(); ++index) {
            adapters::personal_archive::ArchiveOpenRequest layer;
            layer.source = path_from_utf8(job.source_refs[index]);
            layer.password = secrets[index]->view();
            layer.maximum_chunk_payload_size = options.memory_budget_bytes;
            layer.maximum_chunk_logical_size = options.memory_budget_bytes;
            open_request.layers.push_back(std::move(layer));
        }
        auto opened =
            adapters::personal_archive::PersonalFileArchiveChainReader::open(open_request);
        if (!opened) {
            stage.fail(opened.error(), "PersonalFileArchiveChainReader::open",
                       verify_hint_for(opened.error().code, opened.error().message));
            return validated_result(failed_result(job, opened.error().code));
        }
        chain = std::move(opened).value();
        stage.note_u64("layers", chain->layer_count());
        stage.note_u64("entry_count", chain->entry_count());
        stage.note_u64("stream_count", chain->stream_count());
        stage.note("index_generation", chain->index_root_digest());
    }

    const auto budget = static_cast<std::size_t>(
        (std::min)(options.memory_budget_bytes,
                   static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())));
    adapters::personal_archive::FileChainVerifyResult totals;
    {
        ScopedStage stage(task_log.get(), "verify_recoverability");
        publish_progress(job, context.progress, contracts::TaskPhase::kReading, 0, 0,
                         "verify.reading");
        auto verified = chain->verify_recoverability(budget == 0 ? 1 : budget, cancellation);
        if (!verified) {
            stage.fail(verified.error(), "verify_recoverability",
                       verify_hint_for(verified.error().code, verified.error().message));
            return validated_result(failed_result(job, verified.error().code));
        }
        totals = std::move(verified).value();
        stage.note_u64("layers", totals.layer_count);
        stage.note_bytes("local_payload_bytes", totals.local_payload_bytes);
        stage.note_bytes("tip_resolved_bytes", totals.tip_resolved_bytes);
    }
    publish_progress(job, context.progress, contracts::TaskPhase::kCompleted,
                     totals.tip_resolved_bytes, totals.tip_resolved_bytes, "verify.completed");
    auto result = validated_result(completed_result(job, totals));
    if (result) {
        log_verify_success(task_log.get(), result.value(), totals, started);
    }
    return result;
}

} // namespace

base::Result<contracts::TaskResult>
run_file_set_verify_task(const contracts::JobRequest& job,
                         const WindowsPersonalBackupTaskOptions& options,
                         const WindowsPersonalBackupTaskContext& context,
                         const base::CancellationToken& cancellation) {
    if (auto valid = validate_task(job, options); !valid) {
        return base::Result<contracts::TaskResult>::failure(valid.error());
    }
    return run_file_set_verify(job, options, context, cancellation);
}

} // namespace detail

base::Result<contracts::TaskResult> execute_personal_file_archive_verify_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation) {
    try {
        return detail::run_file_set_verify_task(job, options, context, cancellation);
    } catch (const std::exception&) {
        return base::Result<contracts::TaskResult>::failure(
            {base::ErrorCode::kInternal, "file_set verify entry failed unexpectedly"});
    }
}

} // namespace aegra::apps::worker
