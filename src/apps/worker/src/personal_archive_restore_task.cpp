#include "aegra/apps/worker/personal_archive_restore_task.h"

#include "personal_archive_restore_task_backend.h"
#include "worker_task_log.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"

#include <filesystem>
#include <memory>
#include <string>
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
    if (job.operation != contracts::JobOperation::kRestore || job.source_refs.empty() ||
        job.target_ref.empty() || job.credential_refs.size() != job.source_refs.size()) {
        return invalid("personal restore task requires matching sources and credentials");
    }
    if (options.memory_budget_bytes == 0 || options.chunk_size_bytes == 0 ||
        options.memory_budget_bytes < options.chunk_size_bytes ||
        options.maximum_restore_chain_depth == 0 ||
        job.source_refs.size() > options.maximum_restore_chain_depth) {
        return invalid("personal restore task geometry is invalid");
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
        return "restore.cancelled";
    case base::ErrorCode::kUnauthorized:
        return "restore.credential_unavailable";
    case base::ErrorCode::kInsufficientSpace:
        return "restore.target_too_small";
    case base::ErrorCode::kNotFound:
    case base::ErrorCode::kIoFailure:
        return "restore.io_failed";
    case base::ErrorCode::kCorruptData:
        return "restore.archive_corrupt";
    case base::ErrorCode::kConflict:
        return "restore.target_or_chain_conflict";
    case base::ErrorCode::kInvalidArgument:
        return "restore.invalid_request";
    default:
        return "restore.failed";
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
                                       const pipeline::RestoreSummary& summary) {
    return {contracts::kTaskResultSchemaVersion,
            job.job_id,
            job.trace_id,
            contracts::TaskOutcome::kSucceeded,
            base::ErrorCode::kNone,
            summary.restored_bytes,
            summary.restored_bytes,
            summary.chunk_count,
            "restore.completed",
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
                           contracts::TaskPhase::kPreparing, 0, 0, 0, "restore.preparing"});
    }
}

using ResolvedSecrets = std::vector<std::unique_ptr<ports::IResolvedSecret>>;

base::Result<ResolvedSecrets>
resolve_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                const base::CancellationToken& cancellation) {
    ResolvedSecrets result;
    result.reserve(job.credential_refs.size());
    for (const auto& reference : job.credential_refs) {
        auto secret = credentials.resolve(reference, cancellation);
        if (!secret) {
            return base::Result<ResolvedSecrets>::failure(secret.error());
        }
        if (secret.value() == nullptr || secret.value()->view().empty()) {
            return base::Result<ResolvedSecrets>::failure(
                {base::ErrorCode::kUnauthorized, "archive credential is unavailable"});
        }
        result.push_back(std::move(secret).value());
    }
    return base::Result<ResolvedSecrets>::success(std::move(result));
}

PersonalArchiveRestoreBackendRequest
make_backend_request(const contracts::JobRequest& job,
                     const WindowsPersonalBackupTaskOptions& options,
                     const WindowsPersonalBackupTaskContext& context,
                     const ResolvedSecrets& secrets) {
    PersonalArchiveRestoreBackendRequest request;
    request.layers.reserve(job.source_refs.size());
    for (std::size_t index = 0; index < job.source_refs.size(); ++index) {
        request.layers.push_back({path_from_utf8(job.source_refs[index]), secrets[index]->view()});
    }
    request.target = path_from_utf8(job.target_ref);
    request.plan = {job.job_id, job.trace_id, options.memory_budget_bytes};
    request.maximum_chunk_size = options.memory_budget_bytes;
    request.maximum_chain_depth = options.maximum_restore_chain_depth;
    request.progress = context.progress;
    return request;
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job,
                  const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IPersonalArchiveRestoreTaskBackend& backend) {
    auto task_log = WorkerTaskLog::open("restore");
    WorkerTaskLogScope log_scope(task_log.get());
    if (task_log) {
        task_log->info("=== Restore Starting ===");
        task_log->info(std::string("job_id=") + job.job_id);
        task_log->info(std::string("trace_id=") + job.trace_id);
        task_log->info(std::string("layers=") + std::to_string(job.source_refs.size()));
        task_log->info(std::string("target=") + job.target_ref);
    }
    publish_preparing(job, context.progress);
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        if (task_log) {
            task_log->warn("=== Restore Cancelled ===");
        }
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    auto secrets = resolve_secrets(job, context.credentials, cancellation);
    if (!secrets) {
        const auto code = secrets.error().code == base::ErrorCode::kCancelled
                              ? base::ErrorCode::kCancelled
                              : base::ErrorCode::kUnauthorized;
        if (task_log) {
            task_log->error(std::string("=== Restore Failed === message=") +
                            (code == base::ErrorCode::kCancelled ? "restore.cancelled"
                                                                 : "restore.credential_unavailable"));
        }
        return validated_result(failed_result(job, code));
    }
    auto request = make_backend_request(job, options, context, secrets.value());
    auto restored = backend.run(request, cancellation);
    if (!restored) {
        auto result = validated_result(failed_result(job, restored.error().code));
        if (task_log && result) {
            task_log->error(std::string("=== Restore Failed === message=") +
                            result.value().message_code);
        }
        return result;
    }
    auto result = validated_result(completed_result(job, restored.value()));
    if (task_log && result) {
        task_log->info(std::string("=== Restore Complete === message=") +
                       result.value().message_code);
        task_log->info(std::string("logical_bytes=") +
                       std::to_string(result.value().logical_bytes));
    }
    return result;
}

} // namespace

base::Result<contracts::TaskResult> execute_personal_archive_restore_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation, IPersonalArchiveRestoreTaskBackend& backend) {
    auto validation = validate_task(job, options);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return run_accepted_task(job, options, context, cancellation, backend);
}

} // namespace detail

base::Result<contracts::TaskResult> execute_personal_archive_restore_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation) {
    try {
        auto backend = detail::make_personal_archive_restore_task_backend();
        return detail::execute_personal_archive_restore_task_with_backend(
            job, options, context, cancellation, *backend);
    } catch (...) {
        return base::Result<contracts::TaskResult>::failure(
            {base::ErrorCode::kInternal, "personal archive restore entry failed unexpectedly"});
    }
}

} // namespace aegra::apps::worker
