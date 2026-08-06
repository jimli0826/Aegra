#include "aegra/apps/worker/personal_archive_restore_task.h"

#include "personal_archive_restore_task_backend.h"
#include "worker_task_log.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

[[nodiscard]] std::string_view restore_hint_for(const base::ErrorCode code,
                                                const std::string_view message) noexcept {
    if (message.find("sharing violation") != std::string_view::npos ||
        message.find("Win32 error 32") != std::string_view::npos) {
        return "Close Explorer windows and apps using the target volume, then retry";
    }
    if (message.find("Win32 error 33") != std::string_view::npos) {
        return "Target volume is locked by another process; retry later";
    }
    if (message.find("system volume") != std::string_view::npos ||
        message.find("system disk") != std::string_view::npos) {
        return "Online system volume/disk restore is forbidden; use WinPE";
    }
    if (message.find("in use") != std::string_view::npos) {
        return "Close open handles on the target, then retry";
    }
    switch (code) {
    case base::ErrorCode::kInsufficientSpace:
        return "Choose a larger target volume or disk";
    case base::ErrorCode::kUnauthorized:
        return "Re-enter the archive password and retry";
    case base::ErrorCode::kCorruptData:
        return "Re-backup the source or pick another recovery point";
    case base::ErrorCode::kConflict:
        return "Check that the target is not the archive volume/disk and is not system";
    case base::ErrorCode::kNotFound:
    case base::ErrorCode::kIoFailure:
        return "Check repository path, target path, disk health, and permissions";
    case base::ErrorCode::kInvalidArgument:
        return "Verify restore mode, volume_index/disk_number, and target selection";
    case base::ErrorCode::kCancelled:
        return "Job was cancelled or deadline expired";
    default:
        return {};
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

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override { return {}; }
};

base::Result<ResolvedSecrets>
resolve_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                const base::CancellationToken& cancellation) {
    ResolvedSecrets result;
    result.reserve(job.credential_refs.size());
    for (const auto& reference : job.credential_refs) {
        // Empty SecretRef = unencrypted archive (empty password).
        if (reference.value.empty()) {
            result.push_back(std::make_unique<EmptyPasswordSecret>());
            continue;
        }
        auto secret = credentials.resolve(reference, cancellation);
        if (!secret) {
            return base::Result<ResolvedSecrets>::failure(secret.error());
        }
        if (secret.value() == nullptr) {
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
    if (job.restore && job.restore->disk_restore) {
        request.disk_restore = true;
        request.source_disk_number = job.restore->source_disk_number;
        request.bring_target_online = job.restore->bring_target_online;
        request.preserve_disk_signature = job.restore->preserve_disk_signature;
        request.auto_expand_last_partition = job.restore->auto_expand_last_partition;
    } else if (job.restore) {
        request.source_volume_index = job.restore->source_volume_index;
    }
    return request;
}

void log_restore_request(WorkerTaskLog* log, const contracts::JobRequest& job,
                         const WindowsPersonalBackupTaskOptions& options) {
    if (log == nullptr) {
        return;
    }
    const bool disk = job.restore && job.restore->disk_restore;
    log->section("Job");
    log->field("job_id", job.job_id);
    log->field("trace_id", job.trace_id);
    log->field("mode", disk ? "disk" : "volume");
    log->field_u64("layers", job.source_refs.size());

    log->section("Request");
    if (disk) {
        log->field_u64("source_disk_number", job.restore->source_disk_number);
        log->field_bool("preserve_disk_signature", job.restore->preserve_disk_signature);
        log->field_bool("auto_expand_last_partition", job.restore->auto_expand_last_partition);
        log->field_bool("bring_target_online", job.restore->bring_target_online);
    } else {
        const auto volume_index = job.restore ? job.restore->source_volume_index : std::uint32_t{0};
        log->field_u64("source_volume_index", volume_index);
    }
    log->field("target", job.target_ref);
    log->field_bytes("memory_budget", options.memory_budget_bytes);
    log->field_u64("maximum_chain_depth", options.maximum_restore_chain_depth);
    for (std::size_t index = 0; index < job.source_refs.size(); ++index) {
        log->field("archive[" + std::to_string(index) + "]", job.source_refs[index]);
    }
    std::size_t empty_password_layers = 0;
    for (const auto& credential : job.credential_refs) {
        if (credential.value.empty()) {
            ++empty_password_layers;
        }
    }
    log->field_u64("password_layers", job.credential_refs.size() - empty_password_layers);
    log->field_u64("empty_password_layers", empty_password_layers);
}

void log_restore_failure(WorkerTaskLog* log, const base::Error& error,
                         const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const auto* message_code = message_code_for(error.code);
    const auto hint = restore_hint_for(error.code, error.message);
    log->section("Result");
    log->field("outcome", error.code == base::ErrorCode::kCancelled ? "cancelled" : "failed");
    log->field("message_code", message_code);
    log->field("error_code", base::error_code_name(error.code));
    if (!error.message.empty()) {
        log->field("error_message", error.message);
    }
    if (!hint.empty()) {
        log->field("hint", hint);
    }
    log->field("elapsed", format_duration_ms(elapsed));
}

void log_restore_success(WorkerTaskLog* log, const contracts::TaskResult& result,
                         const pipeline::RestoreSummary& summary,
                         const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    log->field("outcome", "succeeded");
    log->field("message_code", result.message_code);
    log->field_bytes("restored_bytes", summary.restored_bytes);
    log->field_u64("chunks", summary.chunk_count);
    log->field_bytes("peak_buffer", summary.peak_buffered_bytes);
    if (elapsed.count() > 0 && summary.restored_bytes > 0) {
        const auto bps = static_cast<std::uint64_t>(
            (static_cast<double>(summary.restored_bytes) * 1000.0) /
            static_cast<double>(elapsed.count()));
        log->field_bytes("throughput", bps);
    }
    log->field("elapsed", format_duration_ms(elapsed));
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job,
                  const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IPersonalArchiveRestoreTaskBackend& backend) {
    const auto started = std::chrono::steady_clock::now();
    auto task_log = WorkerTaskLog::open("restore", job.job_id);
    WorkerTaskLogScope log_scope(task_log.get());
    log_restore_request(task_log.get(), job, options);
    publish_preparing(job, context.progress);
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        log_restore_failure(task_log.get(),
                            {base::ErrorCode::kCancelled, "restore cancelled before start"},
                            started);
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    ResolvedSecrets secrets;
    {
        ScopedStage stage(task_log.get(), "resolve_credentials");
        auto resolved = resolve_secrets(job, context.credentials, cancellation);
        if (!resolved) {
            const auto code = resolved.error().code == base::ErrorCode::kCancelled
                                  ? base::ErrorCode::kCancelled
                                  : base::ErrorCode::kUnauthorized;
            const base::Error error{code, resolved.error().message.empty()
                                              ? std::string("credential resolve failed")
                                              : resolved.error().message};
            stage.fail(error, "resolve_secret", restore_hint_for(code, error.message));
            log_restore_failure(task_log.get(), error, started);
            return validated_result(failed_result(job, code));
        }
        stage.note_u64("layers", resolved.value().size());
        secrets = std::move(resolved).value();
    }
    auto request = make_backend_request(job, options, context, secrets);
    auto restored = backend.run(request, cancellation);
    if (!restored) {
        log_restore_failure(task_log.get(), restored.error(), started);
        return validated_result(failed_result(job, restored.error().code));
    }
    auto result = validated_result(completed_result(job, restored.value()));
    if (result) {
        log_restore_success(task_log.get(), result.value(), restored.value(), started);
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
