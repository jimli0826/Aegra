#include "aegra/apps/worker/windows_personal_backup_task.h"

#include "windows_personal_backup_task_backend.h"

#include "aegra/apps/worker/windows_personal_backup.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace detail {
namespace {

using BackupIds = std::pair<std::array<std::byte, 16>, std::array<std::byte, 16>>;

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task(const contracts::JobRequest& job,
                                 const WindowsPersonalBackupTaskOptions& options) {
    auto valid_job = contracts::validate_job_request(job);
    if (!valid_job) {
        return valid_job;
    }
    if (job.operation != contracts::JobOperation::kBackup || job.source_refs.size() != 1 ||
        job.credential_refs.size() != 1) {
        return invalid("personal backup task requires one source and one credential");
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
                                       const WindowsPersonalVolumeBackupResult& backup) {
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

void normalize_uuid(std::array<std::byte, 16>& uuid) noexcept {
    uuid[6] = (uuid[6] & std::byte{0x0F}) | std::byte{0x40};
    uuid[8] = (uuid[8] & std::byte{0x3F}) | std::byte{0x80};
}

base::Result<BackupIds> make_backup_ids(ports::IRandomSource& random,
                                        const base::CancellationToken& cancellation) {
    constexpr std::size_t kMaximumAttempts = 4;
    for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        BackupIds ids;
        auto file_random = random.fill(ids.first, cancellation);
        if (!file_random) {
            return base::Result<BackupIds>::failure(file_random.error());
        }
        auto set_random = random.fill(ids.second, cancellation);
        if (!set_random) {
            return base::Result<BackupIds>::failure(set_random.error());
        }
        normalize_uuid(ids.first);
        normalize_uuid(ids.second);
        if (ids.first != ids.second) {
            return base::Result<BackupIds>::success(ids);
        }
    }
    return base::Result<BackupIds>::failure(base::Error{
        base::ErrorCode::kInternal,
        "random source returned repeated backup identifiers",
    });
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

WindowsPersonalVolumeBackupRequest make_backup_request(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const std::string_view password, const BackupIds& ids, std::string created_utc) {
    WindowsPersonalVolumeBackupRequest request;
    request.job_id = job.job_id;
    request.trace_id = job.trace_id;
    request.volume_guid_path = path_from_utf8(job.source_refs.front());
    request.destination = path_from_utf8(job.target_ref);
    request.password = password;
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

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation,
                  IWindowsPersonalBackupTaskBackend& backend) {
    publish_preparing(job, context.progress);
    if (cancellation.stop_requested()) {
        return validated_task_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    const auto now_utc_ms = context.clock.now_utc_ms();
    if (job.deadline_utc_ms > 0 && now_utc_ms >= job.deadline_utc_ms) {
        return validated_task_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    auto created_utc = format_utc(now_utc_ms);
    if (!created_utc) {
        return validated_task_result(failed_result(job, created_utc.error().code));
    }
    auto ids = make_backup_ids(context.random, cancellation);
    if (!ids) {
        return validated_task_result(failed_result(job, ids.error().code));
    }
    auto secret = context.credentials.resolve(job.credential_refs.front(), cancellation);
    if (!secret) {
        return validated_task_result(
            failed_result(job, credential_error_code(secret.error().code)));
    }
    if (secret.value() == nullptr || secret.value()->view().empty()) {
        return validated_task_result(failed_result(job, base::ErrorCode::kUnauthorized));
    }
    const auto request = make_backup_request(job, options, secret.value()->view(), ids.value(),
                                             std::move(created_utc).value());
    auto backup = backend.run(request, cancellation, context.progress);
    if (!backup) {
        return validated_task_result(failed_result(job, backup.error().code));
    }
    return validated_task_result(completed_result(job, backup.value()));
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
