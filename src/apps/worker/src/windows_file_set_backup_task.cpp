#include "aegra/apps/worker/windows_file_set_backup_task.h"

#include "windows_file_set_backup.h"
#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/error.h"
#include "aegra/base/uuid.h"
#include "aegra/contracts/file_set.h"
#include "aegra/format/personal_archive.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace {
namespace detail_task {

using BackupIds = std::pair<std::array<std::byte, 16>, std::array<std::byte, 16>>;

class FixedPasswordSecret final : public ports::IResolvedSecret {
  public:
    explicit FixedPasswordSecret(const std::string_view password) noexcept : password_(password) {}
    [[nodiscard]] std::string_view view() const noexcept override { return password_; }

  private:
    std::string_view password_;
};

struct ResolvedBackupSecrets final {
    std::unique_ptr<ports::IResolvedSecret> archive;
    bool encryption_enabled{false};
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
    if (job.content_kind != contracts::ContentKind::kFileSet ||
        job.operation != contracts::JobOperation::kBackup) {
        return invalid("file_set backup task requires content_kind=file_set and backup operation");
    }
    if (!job.backup || (job.backup->type != contracts::BackupType::kFull &&
                        job.backup->type != contracts::BackupType::kIncremental)) {
        return invalid("file_set backup task requires full or incremental type");
    }
    if (options.block_size_bytes == 0 || options.chunk_size_bytes < options.block_size_bytes ||
        options.memory_budget_bytes < options.chunk_size_bytes) {
        return invalid("file_set backup task geometry is invalid");
    }
    if (options.kdf_opslimit == 0 || options.kdf_memlimit_bytes == 0 ||
        options.application_version.empty() || options.hostname.empty()) {
        return invalid("file_set backup task options are incomplete");
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

[[nodiscard]] bool is_product_message_code(const std::string& message) noexcept {
    return message.starts_with("file_source.") || message.starts_with("file_restore.") ||
           message.starts_with("backup.");
}

base::ErrorCode credential_error_code(const base::ErrorCode code) noexcept {
    return code == base::ErrorCode::kCancelled ? base::ErrorCode::kCancelled
                                               : base::ErrorCode::kUnauthorized;
}

contracts::TaskResult failed_result(const contracts::JobRequest& job, const base::ErrorCode code,
                                    const base::Error* detail = nullptr) {
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = code == base::ErrorCode::kCancelled ? contracts::TaskOutcome::kCancelled
                                                         : contracts::TaskOutcome::kFailed;
    result.error_code = code;
    if (detail != nullptr && detail->message.starts_with("file_source.unreadable:")) {
        result.message_code = "file_source.unreadable";
    } else if (detail != nullptr &&
               detail->message.starts_with("file_source.security_descriptor_unreadable:")) {
        result.message_code = "file_source.security_descriptor_unreadable";
    } else if (detail != nullptr && is_product_message_code(detail->message)) {
        result.message_code = detail->message;
    } else {
        result.message_code = message_code_for(code);
    }
    return result;
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const WindowsFileSetBackupResult& backup) {
    const bool has_warning = backup.snapshot_cleanup_error.has_value();
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = has_warning ? contracts::TaskOutcome::kSucceededWithWarning
                                 : contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.logical_bytes = backup.backup.logical_bytes;
    result.stored_bytes = backup.backup.stored_bytes;
    result.chunk_count = backup.backup.chunk_count;
    result.entry_count = backup.backup.entry_count;
    result.stream_count = backup.backup.stream_count;
    result.message_code = has_warning ? "backup.completed_with_warning" : "backup.completed";
    if (has_warning) {
        result.warning_codes.emplace_back("backup.snapshot_cleanup_failed");
    }
    result.requested_backup_type = static_cast<std::uint8_t>(job.backup->type);
    result.effective_backup_type = static_cast<std::uint8_t>(backup.effective_type);
    if (backup.effective_type == contracts::BackupType::kIncremental &&
        !backup.effective_parent_uuid.empty()) {
        result.effective_parent_uuid = backup.effective_parent_uuid;
    }
    if (backup.incremental_downgrade_reason) {
        result.incremental_downgrade_reason = backup.incremental_downgrade_reason;
    }
    return result;
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
    progress->publish(contracts::make_byte_progress(
        job.job_id, job.trace_id, contracts::TaskPhase::kPreparing, 0, 0, 0, "backup.preparing"));
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
    auto backup_set_uuid = base::parse_uuid(options.backup_set_uuid);
    if (!backup_set_uuid) {
        return base::Result<BackupIds>::failure(backup_set_uuid.error());
    }
    return base::Result<BackupIds>::success({file_uuid.value(), backup_set_uuid.value()});
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

[[nodiscard]] std::filesystem::path resolve_data_dir_for_staging() {
    const auto read_env = [](const wchar_t* name) -> std::filesystem::path {
        const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) {
            return {};
        }
        std::vector<wchar_t> value(required);
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0 || written >= required) {
            return {};
        }
        return std::filesystem::path(value.data());
    };
    if (const auto from_service = read_env(L"AEGRA_DATA_DIR"); !from_service.empty()) {
        return from_service;
    }
    if (const auto local = read_env(L"LOCALAPPDATA"); !local.empty()) {
        return local / L"Aegra";
    }
    if (const auto program_data = read_env(L"ProgramData"); !program_data.empty()) {
        return program_data / L"Aegra";
    }
    return {};
}

[[nodiscard]] base::Result<std::filesystem::path>
make_index_spool_directory(const std::string& job_id) {
    const auto data_dir = resolve_data_dir_for_staging();
    if (data_dir.empty()) {
        return base::Result<std::filesystem::path>::failure(base::Error{
            base::ErrorCode::kIoFailure,
            "worker data directory is unavailable for index spool",
        });
    }
    // job_id is a canonical UUID from JobRequest validation; safe as a path component.
    return base::Result<std::filesystem::path>::success(data_dir / L"staging" / ("job-" + job_id) /
                                                        L"index-spool");
}

base::Result<ResolvedBackupSecrets>
resolve_backup_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                       const base::CancellationToken& cancellation) {
    ResolvedBackupSecrets result;
    result.encryption_enabled = job.backup->encryption_enabled;
    if (!result.encryption_enabled) {
        if (!job.credential_refs.empty()) {
            return base::Result<ResolvedBackupSecrets>::failure(
                {base::ErrorCode::kInvalidArgument,
                 "unencrypted backup must not supply credentials"});
        }
        result.archive = std::make_unique<FixedPasswordSecret>(std::string_view{});
        return base::Result<ResolvedBackupSecrets>::success(std::move(result));
    }
    if (job.credential_refs.empty() || job.credential_refs.front().value.empty()) {
        return base::Result<ResolvedBackupSecrets>::failure(
            {base::ErrorCode::kUnauthorized, "encrypted backup requires a password credential"});
    }
    auto archive = credentials.resolve(job.credential_refs.front(), cancellation);
    if (!archive || archive.value() == nullptr || archive.value()->view().empty()) {
        const auto code = !archive ? archive.error().code : base::ErrorCode::kUnauthorized;
        return base::Result<ResolvedBackupSecrets>::failure({code, "archive credential failed"});
    }
    result.archive = std::move(archive).value();
    return base::Result<ResolvedBackupSecrets>::success(std::move(result));
}

WindowsFileSetBackupRequest make_backup_request(const contracts::JobRequest& job,
                                                const WindowsPersonalBackupTaskOptions& options,
                                                const ResolvedBackupSecrets& secrets,
                                                const BackupIds& ids, std::string created_utc,
                                                std::filesystem::path spool_directory) {
    WindowsFileSetBackupRequest request;
    request.job_id = job.job_id;
    request.trace_id = job.trace_id;
    request.selections = job.file_source_refs;
    request.destination = path_from_utf8(job.target_ref);
    request.index_spool_directory = std::move(spool_directory);
    request.password = secrets.archive->view();
    request.encryption_enabled = secrets.encryption_enabled;
    request.file_uuid = ids.first;
    request.backup_set_uuid = ids.second;
    request.requested_type = job.backup->type;
    // Service demotion keeps type=Incremental with service_full_reason; Worker writes Full.
    if (job.backup->service_full_reason) {
        request.effective_type = contracts::BackupType::kFull;
        request.service_full_reason = job.backup->service_full_reason;
    } else if (job.backup->type == contracts::BackupType::kIncremental) {
        request.effective_type = contracts::BackupType::kIncremental;
    } else {
        request.effective_type = contracts::BackupType::kFull;
    }
    // File index leaf pages are capped at 1 MiB plain. Each stream extent maps to one
    // block-sized write; a 64 KiB volume-style quantum inflates leaf CBOR past the budget
    // after only a few hundred MiB of payload. Use the Worker chunk size (clamped to the
    // format max block size) as the file_set stream quantum.
    namespace archive = format::personal_archive;
    auto block = options.chunk_size_bytes;
    if (block > archive::kMaximumBlockSizeBytes) {
        block = archive::kMaximumBlockSizeBytes;
    }
    if (block < archive::kMinimumFileBlockSizeBytes) {
        block = archive::kMinimumFileBlockSizeBytes;
    }
    block -= block % archive::kFileBlockSizeAlignment;
    request.block_size_bytes = block;
    request.chunk_size_bytes = options.chunk_size_bytes;
    if (request.chunk_size_bytes < request.block_size_bytes ||
        request.chunk_size_bytes % request.block_size_bytes != 0) {
        request.chunk_size_bytes = request.block_size_bytes;
    }
    request.memory_budget_bytes = options.memory_budget_bytes;
    request.split_size_bytes = options.split_size_bytes;
    request.kdf_opslimit = options.kdf_opslimit;
    request.kdf_memlimit_bytes = options.kdf_memlimit_bytes;
    request.created_utc = std::move(created_utc);
    request.application_version = options.application_version;
    request.hostname = options.hostname;
    return request;
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>
open_parent_archive(const contracts::BackupOptions& options, const std::string_view password,
                    const std::size_t memory_budget_bytes) {
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = path_from_utf8(options.parent_source_ref);
    open_request.password = password;
    open_request.maximum_chunk_payload_size = memory_budget_bytes;
    open_request.maximum_chunk_logical_size = memory_budget_bytes;
    auto reader = adapters::personal_archive::PersonalFileArchiveReader::open(open_request);
    if (!reader) {
        // Parent credential/open failures are hard failures — never silent Full.
        return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
            failure(reader.error().code == base::ErrorCode::kUnauthorized
                        ? base::Error{base::ErrorCode::kUnauthorized, "parent archive credential failed"}
                        : reader.error());
    }
    auto expected = base::parse_uuid(options.candidate_parent_uuid);
    if (!expected) {
        return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
            failure(expected.error());
    }
    if (reader.value()->identity().file_uuid != expected.value()) {
        return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
            failure(base::Error{base::ErrorCode::kConflict,
                                "parent archive identity does not match candidate_parent_uuid"});
    }
    auto set = base::parse_uuid(options.backup_set_uuid);
    if (!set) {
        return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
            failure(set.error());
    }
    if (reader.value()->identity().backup_set_uuid != set.value()) {
        return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
            failure(base::Error{base::ErrorCode::kConflict,
                                "parent archive backup set does not match schedule"});
    }
    return reader;
}

[[nodiscard]] std::string_view backup_hint_for(const base::ErrorCode code,
                                               const std::string_view message) noexcept {
    if (message.starts_with("file_source.")) {
        return "Check source access permissions and retry the backup";
    }
    if (message.find("VSS") != std::string_view::npos) {
        return "Check VSS service health and that volumes support snapshots";
    }
    if (message.find("space") != std::string_view::npos ||
        code == base::ErrorCode::kInsufficientSpace) {
        return "Free space on the repository volume or choose another destination";
    }
    switch (code) {
    case base::ErrorCode::kUnauthorized:
        return "Re-enter the archive password and retry";
    case base::ErrorCode::kNotFound:
        return "Refresh source inventory and confirm selected paths still exist";
    case base::ErrorCode::kCancelled:
        return "Job was cancelled or deadline expired";
    case base::ErrorCode::kInvalidArgument:
        return "Verify file selections, volume identities, and repository destination";
    default:
        return "Check volume access, repository path, disk health, and Service privileges";
    }
}

void log_backup_request(WorkerTaskLog* log, const contracts::JobRequest& job) {
    if (log == nullptr) {
        return;
    }
    log->section("Job");
    log->field("job_id", job.job_id);
    log->field("trace_id", job.trace_id);
    log->field("content_kind", "file_set");
    log->field("requested_backup_type",
               job.backup->type == contracts::BackupType::kIncremental ? "incremental" : "full");
    if (job.backup->service_full_reason) {
        log->field_u64("service_full_reason",
                       static_cast<std::uint64_t>(*job.backup->service_full_reason));
    }
    if (!job.backup->candidate_parent_uuid.empty()) {
        log->field("candidate_parent_uuid", job.backup->candidate_parent_uuid);
    }
    log->field_u64("selection_count", job.file_source_refs.size());
    log->section("Request");
    log->field("destination", job.target_ref);
    log->field_bool("encryption_enabled", job.backup->encryption_enabled);
    // Paths and relative components are customer data; log only opaque selection IDs.
    for (std::size_t index = 0; index < job.file_source_refs.size(); ++index) {
        log->field("selection[" + std::to_string(index) + "]",
                   job.file_source_refs[index].selection_id);
    }
}

void log_backup_result(WorkerTaskLog* log, const contracts::TaskResult& result,
                       const base::Error* error,
                       const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    if (result.outcome == contracts::TaskOutcome::kSucceeded ||
        result.outcome == contracts::TaskOutcome::kSucceededWithWarning) {
        log->field("outcome", result.outcome == contracts::TaskOutcome::kSucceededWithWarning
                                  ? "succeeded_with_warning"
                                  : "succeeded");
        log->field("message_code", result.message_code);
        log->field_bytes("logical_bytes", result.logical_bytes);
        log->field_bytes("stored_bytes", result.stored_bytes);
        log->field_u64("chunks", result.chunk_count);
        log->field_u64("entries", result.entry_count);
        log->field_u64("streams", result.stream_count);
        for (const auto& warning : result.warning_codes) {
            log->warn(std::string("  warning                 : ") + warning);
        }
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
        const auto hint = backup_hint_for(error->code, error->message);
        if (!hint.empty()) {
            log->field("hint", hint);
        }
    }
    log->field("elapsed", format_duration_ms(elapsed));
}

base::Result<contracts::TaskResult>
run_accepted_task(const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
                  const WindowsPersonalBackupTaskContext& context,
                  const base::CancellationToken& cancellation) {
    const auto started = std::chrono::steady_clock::now();
    auto task_log = WorkerTaskLog::open("backup", job.job_id);
    WorkerTaskLogScope log_scope(task_log.get());
    WorkerTaskLog* log = task_log.get();
    publish_preparing(job, context.progress);

    auto fail = [&](const base::ErrorCode code,
                    const base::Error* detail = nullptr) -> base::Result<contracts::TaskResult> {
        auto result = validated_task_result(failed_result(job, code, detail));
        if (result) {
            log_backup_result(log, result.value(), detail, started);
        }
        return result;
    };

    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        const base::Error error{base::ErrorCode::kCancelled, "file_set backup cancelled before start"};
        return fail(base::ErrorCode::kCancelled, &error);
    }
    auto created_utc = format_utc(job.backup->created_utc_ms);
    if (!created_utc) {
        return fail(created_utc.error().code, &created_utc.error());
    }
    auto ids = requested_ids(*job.backup);
    if (!ids) {
        return fail(ids.error().code, &ids.error());
    }
    auto spool = make_index_spool_directory(job.job_id);
    if (!spool) {
        return fail(spool.error().code, &spool.error());
    }

    ResolvedBackupSecrets secrets;
    {
        ScopedStage stage(log, "resolve_credentials");
        auto resolved = resolve_backup_secrets(job, context.credentials, cancellation);
        if (!resolved) {
            const auto code = credential_error_code(resolved.error().code);
            const base::Error error{code, resolved.error().message};
            stage.fail(error, "resolve_secret", backup_hint_for(code, error.message));
            return fail(code, &error);
        }
        stage.note_bool("encryption_enabled", resolved.value().encryption_enabled);
        secrets = std::move(resolved).value();
    }

    auto request =
        make_backup_request(job, options, secrets, ids.value(), std::move(created_utc).value(),
                            std::move(spool).value());
    log_backup_request(log, job);

    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader> parent_reader;
    if (request.effective_type == contracts::BackupType::kIncremental) {
        ScopedStage stage(log, "open_parent_archive");
        auto opened =
            open_parent_archive(*job.backup, secrets.archive->view(), options.memory_budget_bytes);
        if (!opened) {
            const auto code = credential_error_code(opened.error().code);
            const base::Error error{code, opened.error().message};
            stage.fail(error, "open_parent", backup_hint_for(code, error.message));
            return fail(code, &error);
        }
        parent_reader = std::move(opened).value();
        request.parent_uuid = parent_reader->identity().file_uuid;
        request.parent_checkpoints =
            parent_reader->manifest().file_set_baseline.journal_checkpoints;
        request.parent_reader = parent_reader.get();
        stage.note("parent_uuid", job.backup->candidate_parent_uuid);
        stage.note_u64("parent_checkpoint_count", request.parent_checkpoints.size());
    }

    auto backup = detail::backup_windows_file_set(request, cancellation, context.progress);
    if (!backup) {
        return fail(backup.error().code, &backup.error());
    }
    auto result = validated_task_result(completed_result(job, backup.value()));
    if (result) {
        log_backup_result(log, result.value(), nullptr, started);
    }
    return result;
}

} // namespace detail_task
} // namespace

base::Result<contracts::TaskResult> execute_windows_file_set_backup_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation) {
    try {
        auto validation = detail_task::validate_task(job, options);
        if (!validation) {
            return base::Result<contracts::TaskResult>::failure(validation.error());
        }
        return detail_task::run_accepted_task(job, options, context, cancellation);
    } catch (...) {
        return base::Result<contracts::TaskResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows file_set backup task entry failed unexpectedly",
        });
    }
}

} // namespace aegra::apps::worker
