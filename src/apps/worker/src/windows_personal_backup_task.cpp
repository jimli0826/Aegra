#include "aegra/apps/worker/windows_personal_backup_task.h"

#include "windows_personal_backup_task_backend.h"
#include "worker_task_log.h"

#include "aegra/apps/worker/windows_personal_backup.h"
#include "aegra/base/uuid.h"
#include "aegra/contracts/service_control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace detail {
namespace {

using BackupIds = std::pair<std::array<std::byte, 16>, std::array<std::byte, 16>>;

// Personal edition allows jobs without wincred:// credentials; archive crypto still needs a
// non-empty password material (personal_archive_session rejects empty passwords).
constexpr std::string_view kDefaultLocalArchivePassword = "aegra-local";

class FixedPasswordSecret final : public ports::IResolvedSecret {
  public:
    explicit FixedPasswordSecret(const std::string_view password) noexcept : password_(password) {}
    [[nodiscard]] std::string_view view() const noexcept override { return password_; }

  private:
    std::string_view password_;
};

struct ResolvedBackupSecrets final {
    std::unique_ptr<ports::IResolvedSecret> archive;
    std::unique_ptr<ports::IResolvedSecret> parent;
};

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task(const contracts::JobRequest& job,
                                 const WindowsPersonalBackupTaskOptions& options) {
    auto valid_job = contracts::validate_job_request(job);
    if (!valid_job) {
        return valid_job;
    }
    // Credential refs are optional for personal local backup (no wincred required).
    if (job.operation != contracts::JobOperation::kBackup || job.source_refs.empty() ||
        job.source_refs.size() > contracts::kMaximumBackupSources) {
        return invalid("personal backup task source count is invalid");
    }
    if (!job.backup || job.backup->type == contracts::BackupType::kDifferential) {
        return invalid("personal backup task supports full and incremental backup types");
    }
    if (options.block_size_bytes == 0 || options.chunk_size_bytes < options.block_size_bytes ||
        options.memory_budget_bytes < options.chunk_size_bytes) {
        return invalid("personal backup task geometry is invalid");
    }
    if (options.kdf_opslimit == 0 || options.kdf_memlimit_bytes == 0 ||
        options.application_version.empty() || options.hostname.empty()) {
        return invalid("personal backup task options are incomplete");
    }
    return base::Result<void>::success();
}

const char* message_code_for(const base::ErrorCode code) noexcept {
    switch (code) {
    case base::ErrorCode::kCancelled:
        return "backup.cancelled";
    case base::ErrorCode::kUnauthorized:
        return "backup.credential_unavailable";
    case base::ErrorCode::kInsufficientSpace:
        return "backup.insufficient_space";
    case base::ErrorCode::kNotFound:
        return "backup.source_not_found";
    case base::ErrorCode::kInvalidArgument:
        return "backup.invalid_request";
    default:
        return "backup.failed";
    }
}

base::ErrorCode credential_error_code(const base::ErrorCode code) noexcept {
    return code == base::ErrorCode::kCancelled ? base::ErrorCode::kCancelled
                                               : base::ErrorCode::kUnauthorized;
}

contracts::TaskResult failed_result(const contracts::JobRequest& job, const base::ErrorCode code) {
    const auto outcome = code == base::ErrorCode::kCancelled ? contracts::TaskOutcome::kCancelled
                                                             : contracts::TaskOutcome::kFailed;
    return contracts::TaskResult{
        contracts::kTaskResultSchemaVersion,
        job.job_id,
        job.trace_id,
        outcome,
        code,
        0,
        0,
        0,
        message_code_for(code),
        {},
    };
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const WindowsPersonalBackupResult& backup) {
    const bool has_warning = backup.snapshot_cleanup_error.has_value();
    std::vector<std::string> warnings;
    if (has_warning) {
        warnings.emplace_back("backup.snapshot_cleanup_failed");
    }
    return contracts::TaskResult{
        contracts::kTaskResultSchemaVersion,
        job.job_id,
        job.trace_id,
        has_warning ? contracts::TaskOutcome::kSucceededWithWarning
                    : contracts::TaskOutcome::kSucceeded,
        base::ErrorCode::kNone,
        backup.backup.logical_bytes,
        backup.backup.stored_bytes,
        backup.backup.chunk_count,
        has_warning ? "backup.completed_with_warning" : "backup.completed",
        std::move(warnings),
    };
}

base::Result<contracts::TaskResult> validated_task_result(contracts::TaskResult result) {
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

void publish_preparing(const contracts::JobRequest& job, ports::IProgressSink* progress) {
    if (progress == nullptr) {
        return;
    }
    progress->publish(contracts::TaskProgress{
        contracts::kTaskProgressSchemaVersion,
        job.job_id,
        job.trace_id,
        contracts::TaskPhase::kPreparing,
        0,
        0,
        0,
        "backup.preparing",
    });
}

base::Result<std::string> format_utc(const std::int64_t utc_ms) {
    if (utc_ms < 0) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "worker clock returned invalid time"});
    }
    const auto seconds = static_cast<std::time_t>(utc_ms / 1000);
    std::tm utc{};
    if (::gmtime_s(&utc, &seconds) != 0) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "worker clock time is out of range"});
    }
    std::array<char, 32> buffer{};
    const auto count =
        std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                      utc.tm_sec, static_cast<long long>(utc_ms % 1000));
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size()) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "worker clock formatting failed"});
    }
    return base::Result<std::string>::success(std::string(buffer.data()));
}

base::Result<BackupIds> requested_ids(const contracts::BackupOptions& options) {
    auto file_uuid = base::parse_uuid(options.file_uuid);
    if (!file_uuid) {
        return base::Result<BackupIds>::failure(file_uuid.error());
    }
    std::array<std::byte, 16> backup_set_uuid{};
    if (options.type == contracts::BackupType::kFull) {
        auto parsed_set = base::parse_uuid(options.backup_set_uuid);
        if (!parsed_set) {
            return base::Result<BackupIds>::failure(parsed_set.error());
        }
        backup_set_uuid = parsed_set.value();
    }
    return base::Result<BackupIds>::success({file_uuid.value(), backup_set_uuid});
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

WindowsPersonalBackupRequest make_backup_request(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const ResolvedBackupSecrets& secrets, const BackupIds& ids, std::string created_utc) {
    WindowsPersonalBackupRequest request;
    request.job_id = job.job_id;
    request.trace_id = job.trace_id;
    request.volume_guid_paths.reserve(job.source_refs.size());
    for (const auto& source_ref : job.source_refs) {
        request.volume_guid_paths.push_back(path_from_utf8(source_ref));
    }
    request.destination = path_from_utf8(job.target_ref);
    request.password = secrets.archive->view();
    request.backup_type = job.backup->type == contracts::BackupType::kFull
                              ? WindowsPersonalBackupType::kFull
                              : WindowsPersonalBackupType::kIncremental;
    if (secrets.parent) {
        request.parent_source = path_from_utf8(job.backup->parent_source_ref);
        request.parent_password = secrets.parent->view();
    }
    request.file_uuid = ids.first;
    request.backup_set_uuid = ids.second;
    request.block_size_bytes = options.block_size_bytes;
    request.chunk_size_bytes = options.chunk_size_bytes;
    request.memory_budget_bytes = options.memory_budget_bytes;
    request.split_size_bytes = options.split_size_bytes;
    request.kdf_opslimit = options.kdf_opslimit;
    request.kdf_memlimit_bytes = options.kdf_memlimit_bytes;
    request.created_utc = std::move(created_utc);
    request.application_version = options.application_version;
    request.hostname = options.hostname;
    return request;
}

base::Result<ResolvedBackupSecrets>
resolve_backup_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                       const base::CancellationToken& cancellation) {
    ResolvedBackupSecrets result;
    if (!job.credential_refs.empty() && !job.credential_refs.front().value.empty()) {
        auto archive = credentials.resolve(job.credential_refs.front(), cancellation);
        if (!archive || archive.value() == nullptr || archive.value()->view().empty()) {
            const auto code = !archive ? archive.error().code : base::ErrorCode::kUnauthorized;
            return base::Result<ResolvedBackupSecrets>::failure({code, "archive credential failed"});
        }
        result.archive = std::move(archive).value();
    } else {
        // No credential on the job: use the fixed personal-local password material.
        result.archive = std::make_unique<FixedPasswordSecret>(kDefaultLocalArchivePassword);
    }
    if (job.backup->type == contracts::BackupType::kFull) {
        return base::Result<ResolvedBackupSecrets>::success(std::move(result));
    }
    // Incremental: parent credential optional; fall back to the same local password.
    if (!job.backup->parent_credential_ref.value.empty()) {
        auto parent = credentials.resolve(job.backup->parent_credential_ref, cancellation);
        if (!parent || parent.value() == nullptr || parent.value()->view().empty()) {
            const auto code = !parent ? parent.error().code : base::ErrorCode::kUnauthorized;
            return base::Result<ResolvedBackupSecrets>::failure({code, "parent credential failed"});
        }
        result.parent = std::move(parent).value();
    } else {
        result.parent = std::make_unique<FixedPasswordSecret>(kDefaultLocalArchivePassword);
    }
    return base::Result<ResolvedBackupSecrets>::success(std::move(result));
}

void log_backup_start(WorkerTaskLog* log, const contracts::JobRequest& job,
                      const WindowsPersonalBackupRequest& request) {
    if (log == nullptr) {
        return;
    }
    log->info("=== Backup Starting ===");
    log->info(std::string("job_id=") + job.job_id);
    log->info(std::string("trace_id=") + job.trace_id);
    log->info(std::string("backup_type=") +
              (request.backup_type == WindowsPersonalBackupType::kFull ? "full" : "incremental"));
    for (const auto& source_ref : job.source_refs) {
        log->info(std::string("Volume path: ") + source_ref);
    }
    log->info(std::string("Backup file: ") + job.target_ref);
    if (!request.parent_source.empty()) {
        log->info(std::string("Parent archive: ") +
                  [&request] {
                      const auto encoded = request.parent_source.generic_u8string();
                      std::string text;
                      text.reserve(encoded.size());
                      for (const auto value : encoded) {
                          text.push_back(static_cast<char>(value));
                      }
                      return text;
                  }());
    }
    {
        std::ostringstream geometry;
        geometry << "Geometry: block=" << request.block_size_bytes
                 << " chunk=" << request.chunk_size_bytes
                 << " memory_budget=" << request.memory_budget_bytes;
        log->info(geometry.str());
    }
}

void log_backup_result(WorkerTaskLog* log, const contracts::TaskResult& result) {
    if (log == nullptr) {
        return;
    }
    if (result.outcome == contracts::TaskOutcome::kSucceeded ||
        result.outcome == contracts::TaskOutcome::kSucceededWithWarning) {
        log->info("=== Backup Complete ===");
        std::ostringstream stats;
        stats << "logical_bytes=" << result.logical_bytes << " stored_bytes=" << result.stored_bytes
              << " chunks=" << result.chunk_count << " message=" << result.message_code;
        log->info(stats.str());
        for (const auto& warning : result.warning_codes) {
            log->warn(std::string("warning=") + warning);
        }
        return;
    }
    if (result.outcome == contracts::TaskOutcome::kCancelled) {
        log->warn(std::string("=== Backup Cancelled === message=") + result.message_code);
        return;
    }
    log->error(std::string("=== Backup Failed === message=") + result.message_code);
}

base::Result<contracts::TaskResult> finish_logged(WorkerTaskLog* log,
                                                  base::Result<contracts::TaskResult> result) {
    if (result) {
        log_backup_result(log, result.value());
    }
    return result;
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IWindowsPersonalBackupTaskBackend& backend) {
    auto task_log = WorkerTaskLog::open("backup");
    WorkerTaskLogScope log_scope(task_log.get());
    WorkerTaskLog* log = task_log.get();
    publish_preparing(job, context.progress);
    if (cancellation.stop_requested()) {
        return finish_logged(log, validated_task_result(failed_result(job, base::ErrorCode::kCancelled)));
    }
    const auto now_utc_ms = context.clock.now_utc_ms();
    if (job.deadline_utc_ms > 0 && now_utc_ms >= job.deadline_utc_ms) {
        return finish_logged(log, validated_task_result(failed_result(job, base::ErrorCode::kCancelled)));
    }
    auto created_utc = format_utc(job.backup->created_utc_ms);
    if (!created_utc) {
        return finish_logged(log, validated_task_result(failed_result(job, created_utc.error().code)));
    }
    auto ids = requested_ids(*job.backup);
    if (!ids) {
        return finish_logged(log, validated_task_result(failed_result(job, ids.error().code)));
    }
    auto secrets = resolve_backup_secrets(job, context.credentials, cancellation);
    if (!secrets) {
        return finish_logged(
            log, validated_task_result(failed_result(job, credential_error_code(secrets.error().code))));
    }
    const auto request = make_backup_request(job, options, secrets.value(), ids.value(),
                                             std::move(created_utc).value());
    log_backup_start(log, job, request);
    if (log != nullptr) {
        log->info("Opening volume / creating VSS snapshot");
    }
    auto backup = backend.run(request, cancellation, context.progress);
    if (!backup) {
        return finish_logged(log, validated_task_result(failed_result(job, backup.error().code)));
    }
    return finish_logged(log, validated_task_result(completed_result(job, backup.value())));
}

} // namespace

base::Result<contracts::TaskResult> execute_windows_personal_backup_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation,
    IWindowsPersonalBackupTaskBackend& backend) {
    auto validation = validate_task(job, options);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return run_accepted_task(job, options, context, cancellation, backend);
}

} // namespace detail

base::Result<contracts::TaskResult> execute_windows_personal_backup_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation) {
    try {
        auto backend = detail::make_windows_personal_backup_task_backend();
        return detail::execute_windows_personal_backup_task_with_backend(job, options, context,
                                                                         cancellation, *backend);
    } catch (...) {
        return base::Result<contracts::TaskResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows personal backup task entry failed unexpectedly",
        });
    }
}

} // namespace aegra::apps::worker
