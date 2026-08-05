#include "aegra/apps/service/worker_job_service.h"

#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/base/uuid.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<std::string> random_id(const std::string_view prefix,
                                                  ports::IRandomSource& random,
                                                  const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string id(prefix);
    id.reserve(prefix.size() + 32U);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        id.push_back(kHex[value >> 4U]);
        id.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(id));
}

[[nodiscard]] base::Result<std::string>
random_uuid(ports::IRandomSource& random, const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return base::Result<std::string>::success(base::format_uuid(bytes));
}

[[nodiscard]] base::Result<std::string> archive_key(const std::string_view file_uuid,
                                                    const std::int64_t created_utc_ms) {
    if (created_utc_ms <= 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "service clock returned invalid time"});
    }
    const auto seconds = static_cast<std::time_t>(created_utc_ms / 1000);
    std::tm utc{};
    if (::gmtime_s(&utc, &seconds) != 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "service clock time is out of range"});
    }
    std::array<char, 96> buffer{};
    const auto count = std::snprintf(buffer.data(), buffer.size(), "archives/%04d/%02d/%.*s.bkf",
                                     utc.tm_year + 1900, utc.tm_mon + 1,
                                     static_cast<int>(file_uuid.size()), file_uuid.data());
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size()) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "archive object key formatting failed"});
    }
    return base::Result<std::string>::success(std::string(buffer.data()));
}

[[nodiscard]] base::Result<std::filesystem::path> path_from_utf8(const std::string_view value) {
    try {
        const auto* begin = reinterpret_cast<const char8_t*>(value.data());
        std::filesystem::path path(std::u8string(begin, begin + value.size()));
        if (!path.is_absolute()) {
            return base::Result<std::filesystem::path>::failure(
                {base::ErrorCode::kInvalidArgument, "repository locator must be absolute"});
        }
        return base::Result<std::filesystem::path>::success(std::move(path));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "repository locator is invalid UTF-8"});
    }
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] contracts::CommandAcknowledgement
acknowledgement(std::string command_id, const contracts::CommandDisposition disposition,
                std::optional<std::string> resource_id) {
    return {std::move(command_id), disposition, std::move(resource_id)};
}

[[nodiscard]] bool same_backup(const ports::JobRecord& record,
                               const contracts::StartBackupCommand& command) noexcept {
    if (record.operation != contracts::JobOperation::kBackup ||
        record.source_ids != command.source_ids ||
        record.repository_connection_id != command.repository_connection_id ||
        record.backup_type != command.backup_type ||
        record.parent_recovery_point_id != command.parent_recovery_point_id) {
        return false;
    }
    // Exclude option is part of the request identity (true vs false must not replay).
    if (!record.exclude_page_and_hibernation_files) {
        return false;
    }
    return *record.exclude_page_and_hibernation_files == command.exclude_page_and_hibernation_files;
}

template <typename Command, typename Matcher>
[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
reconcile_submission_conflict(ports::IControlPlaneDatabase& control_plane,
                              const std::string_view idempotency_key, const Command& command,
                              Matcher&& matches, const base::Error& submission_error,
                              const base::CancellationToken cancellation) {
    if (submission_error.code != base::ErrorCode::kConflict) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submission_error);
    }
    auto existing = control_plane.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (!existing.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submission_error);
    }
    if (!std::forward<Matcher>(matches)(*existing.value(), command)) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "idempotency key request mismatch"});
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                        existing.value()->job_id));
}

[[nodiscard]] std::string cancel_fingerprint(const std::string_view job_id) {
    return "cancel|" + std::to_string(job_id.size()) + ":" + std::string(job_id);
}

[[nodiscard]] ports::JobStateTransition cancelling_transition(const std::string_view job_id,
                                                              const std::uint64_t utc_ms) {
    ports::JobStateTransition transition;
    transition.job_id = std::string(job_id);
    transition.expected_state = contracts::ServiceJobState::kRunning;
    transition.next_state = contracts::ServiceJobState::kCancelling;
    transition.transition_utc_ms = utc_ms;
    transition.message_code = "job.cancelling";
    return transition;
}

struct PreparedBackup final {
    WorkerJobRequest request;
    std::string job_id;
};

[[nodiscard]] base::Result<PreparedBackup>
prepare_backup(const contracts::StartBackupCommand& command,
                application::ISourceInventoryQuery& source_inventory,
                ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                ports::IRandomSource& random,
                const base::CancellationToken cancellation) {
    std::vector<std::string> stable_source_refs;
    stable_source_refs.reserve(command.source_ids.size());
    for (const auto& source_id : command.source_ids) {
        auto source = source_inventory.resolve_source(source_id, cancellation);
        if (!source) {
            return base::Result<PreparedBackup>::failure(source.error());
        }
        stable_source_refs.push_back(std::move(source).value().stable_key);
    }
    auto repository =
        control_plane.get_repository_connection(command.repository_connection_id, cancellation);
    if (!repository)
        return base::Result<PreparedBackup>::failure(repository.error());
    // Personal local repositories often have no credential_ref; only require Available state.
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kConflict, "repository is unavailable"});
    }
    auto root = path_from_utf8(repository.value()->locator);
    auto job_id = random_id("job-", random, cancellation);
    auto trace_id = random_id("trace-", random, cancellation);
    auto file_uuid = random_uuid(random, cancellation);
    auto backup_set_uuid = random_uuid(random, cancellation);
    const auto created_utc_ms = clock.now_utc_ms();
    auto key = file_uuid ? archive_key(file_uuid.value(), created_utc_ms)
                         : base::Result<std::string>::failure(file_uuid.error());
    if (!root || !job_id || !trace_id || !file_uuid || !backup_set_uuid || !key) {
        if (!root)
            return base::Result<PreparedBackup>::failure(root.error());
        if (!job_id || !trace_id)
            return base::Result<PreparedBackup>::failure(!job_id ? job_id.error() : trace_id.error());
        if (!file_uuid || !backup_set_uuid)
            return base::Result<PreparedBackup>::failure(!file_uuid ? file_uuid.error()
                                                                    : backup_set_uuid.error());
        return base::Result<PreparedBackup>::failure(key.error());
    }
    const auto archive_path = root.value() / std::filesystem::path(key.value());
    const auto archive_directory = archive_path.parent_path();
    std::error_code error_code;
    std::filesystem::create_directories(archive_directory, error_code);
    if (error_code) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kIoFailure, "archive directory create failed"});
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kBackup;
    worker.source_refs = std::move(stable_source_refs);
    worker.target_ref = path_to_utf8(archive_path);
    if (repository.value()->credential_ref && !repository.value()->credential_ref->value.empty()) {
        worker.credential_refs = {*repository.value()->credential_ref};
    }
    contracts::BackupOptions backup;
    backup.type = contracts::BackupType::kFull;
    backup.file_uuid = file_uuid.value();
    backup.backup_set_uuid = backup_set_uuid.value();
    backup.created_utc_ms = created_utc_ms;
    backup.exclude_page_and_hibernation_files = command.exclude_page_and_hibernation_files;
    worker.backup = std::move(backup);
    worker.trace_id = trace_id.value();
    WorkerJobRequest request{std::move(worker),
                             command.source_ids,
                             command.repository_connection_id,
                             std::nullopt,
                             {},
                             key.value(),
                             {}};
    return base::Result<PreparedBackup>::success({std::move(request), std::move(job_id).value()});
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
persist_cancel_command(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                       ports::IRandomSource& random, const ports::JobRecord& current,
                       const contracts::ResourceRef& job, const std::string_view idempotency_key,
                       std::string fingerprint, const base::CancellationToken cancellation) {
    auto command_id = random_id("cmd-", random, cancellation);
    if (!command_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto result = acknowledgement(command_id.value(), contracts::CommandDisposition::kAccepted,
                                  job.resource_id);
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit)
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    const auto now = static_cast<std::uint64_t>((std::max)(clock.now_utc_ms(), 0LL));
    if (current.state == contracts::ServiceJobState::kRunning) {
        auto transitioned = unit.value()->jobs().transition(
            cancelling_transition(job.resource_id, now), cancellation);
        if (!transitioned) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(transitioned.error());
        }
    }
    ports::CommandRecord record{std::string(idempotency_key), std::move(fingerprint),
                                command_id.value(), job.resource_id, now};
    auto stored = unit.value()->commands().insert(record, cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(std::move(result))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

} // namespace

[[nodiscard]] bool same_verify(const ports::JobRecord& record,
                               const contracts::StartVerifyCommand& command) noexcept {
    return record.operation == contracts::JobOperation::kVerify &&
           record.source_ids == std::vector<std::string>{command.recovery_point_id} &&
           record.repository_connection_id == command.repository_connection_id &&
           !record.parent_recovery_point_id;
}

[[nodiscard]] base::Result<std::string>
resolve_archive_absolute_path(const std::string& locator, const std::string& archive_main_key) {
    auto root = path_from_utf8(locator);
    if (!root) {
        return base::Result<std::string>::failure(root.error());
    }
    if (!archive_main_key.starts_with("archives/") ||
        archive_main_key.find('\\') != std::string::npos ||
        archive_main_key.find(':') != std::string::npos ||
        archive_main_key.find("..") != std::string::npos) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is outside the archive root"});
    }
    std::filesystem::path relative;
    try {
        relative = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(archive_main_key.data()), archive_main_key.size()));
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is invalid"});
    }
    std::error_code error_code;
    const auto canonical_root = std::filesystem::weakly_canonical(root.value(), error_code);
    if (error_code) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kIoFailure, "repository root cannot be resolved"});
    }
    const auto canonical_archive =
        std::filesystem::weakly_canonical(canonical_root / relative, error_code);
    if (error_code) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kIoFailure, "archive path cannot be resolved"});
    }
    const auto relative_to_root = canonical_archive.lexically_relative(canonical_root);
    if (relative_to_root.empty() || relative_to_root == L".." ||
        relative_to_root.native().starts_with(
            L".." + std::wstring(1, std::filesystem::path::preferred_separator))) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kConflict, "archive path escapes repository root"});
    }
    return base::Result<std::string>::success(path_to_utf8(canonical_archive));
}

[[nodiscard]] base::Result<PreparedBackup>
prepare_verify(const contracts::StartVerifyCommand& command,
               ports::IControlPlaneDatabase& control_plane,
               ports::IRepositoryStorageFactory& storage_factory, ports::IRandomSource& random,
               const base::CancellationToken cancellation) {
    auto repository =
        control_plane.get_repository_connection(command.repository_connection_id, cancellation);
    if (!repository) {
        return base::Result<PreparedBackup>::failure(repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<PreparedBackup>::failure(storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    std::optional<std::string> token;
    std::optional<personal_repository::CatalogEntry> found;
    for (;;) {
        personal_repository::CatalogScanRequest request;
        request.continuation_token = token;
        request.maximum_results = 100;
        auto page = scanner.scan(request, cancellation);
        if (!page) {
            return base::Result<PreparedBackup>::failure(page.error());
        }
        for (const auto& point : page.value().recovery_points) {
            if (point.entry.file_uuid == command.recovery_point_id) {
                found = point.entry;
                break;
            }
        }
        if (found || !page.value().continuation_token) {
            break;
        }
        token = std::move(page.value().continuation_token);
    }
    if (!found) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kNotFound, "recovery point was not found"});
    }
    // Archive credential selection: never reuse a connection SecretRef without an explicit
    // repository_uuid+file_uuid mapping. Mapping store is not yet durable; refuse with
    // credential_required rather than guessing with the repository connection credential.
    static_cast<void>(found->repository_uuid);
    if (!repository.value()->credential_ref) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kUnauthorized, "archive.credential_required"});
    }
    // Explicit mapping table is not available yet. Connection credential is only valid when the
    // connection advertises archive.default_credential (Service-managed single-secret repos).
    // Import/connections without that capability must register a per-file mapping (future S5+).
    const auto& capabilities = repository.value()->capabilities;
    const bool allows_connection_secret =
        std::find(capabilities.begin(), capabilities.end(), "archive.default_credential") !=
        capabilities.end();
    if (!allows_connection_secret) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kUnauthorized, "archive.credential_required"});
    }
    auto archive_path =
        resolve_archive_absolute_path(repository.value()->locator, found->archive_main_key);
    auto job_id = random_id("job-", random, cancellation);
    auto trace_id = random_id("trace-", random, cancellation);
    if (!archive_path || !job_id || !trace_id) {
        if (!archive_path)
            return base::Result<PreparedBackup>::failure(archive_path.error());
        return base::Result<PreparedBackup>::failure(!job_id ? job_id.error() : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kVerify;
    worker.source_refs = {archive_path.value()};
    worker.target_ref.clear();
    worker.credential_refs = {*repository.value()->credential_ref};
    worker.trace_id = trace_id.value();
    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = {command.recovery_point_id};
    request.repository_connection_id = command.repository_connection_id;
    return base::Result<PreparedBackup>::success({std::move(request), std::move(job_id).value()});
}

WorkerJobService::WorkerJobService(application::ISourceInventoryQuery& source_inventory,
                                   ports::IControlPlaneDatabase& control_plane,
                                   ports::IRepositoryStorageFactory& storage_factory,
                                   WorkerSupervisor& supervisor, ports::IClock& clock,
                                   ports::IRandomSource& random) noexcept
    : source_inventory_(source_inventory), control_plane_(control_plane),
      storage_factory_(storage_factory), supervisor_(supervisor), clock_(clock), random_(random) {}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_backup(const contracts::StartBackupCommand& command,
                               const std::string_view idempotency_key,
                               const base::CancellationToken cancellation) {
    if (command.backup_type != contracts::BackupType::kFull || command.parent_recovery_point_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "incremental backup is not available"});
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        if (!same_backup(*existing.value(), command)) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto prepared =
        prepare_backup(command, source_inventory_, control_plane_, clock_, random_, cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }
    prepared.value().request.idempotency_key = std::string(idempotency_key);
    auto submitted = supervisor_.submit(prepared.value().request, cancellation);
    if (!submitted) {
        return reconcile_submission_conflict(control_plane_, idempotency_key, command, same_backup,
                                             submitted.error(), cancellation);
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(prepared.value().job_id, contracts::CommandDisposition::kAccepted,
                        prepared.value().job_id));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_verify(const contracts::StartVerifyCommand& command,
                               const std::string_view idempotency_key,
                               const base::CancellationToken cancellation) {
    auto valid = contracts::validate_start_verify_command(command);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        if (!same_verify(*existing.value(), command)) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto prepared =
        prepare_verify(command, control_plane_, storage_factory_, random_, cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }
    prepared.value().request.idempotency_key = std::string(idempotency_key);
    auto submitted = supervisor_.submit(prepared.value().request, cancellation);
    if (!submitted) {
        return reconcile_submission_conflict(control_plane_, idempotency_key, command, same_verify,
                                             submitted.error(), cancellation);
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(prepared.value().job_id, contracts::CommandDisposition::kAccepted,
                        prepared.value().job_id));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::cancel_job(const contracts::ResourceRef& job,
                             const std::string_view idempotency_key,
                             const base::CancellationToken cancellation) {
    auto valid_job = contracts::validate_resource_ref(job);
    if (!valid_job) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid_job.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is required"});
    }
    const auto fingerprint = cancel_fingerprint(job.resource_id);
    auto prior = control_plane_.get_command(idempotency_key, cancellation);
    if (!prior)
        return base::Result<contracts::CommandAcknowledgement>::failure(prior.error());
    if (prior.value()) {
        if (prior.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(prior.value()->command_id, contracts::CommandDisposition::kReplayed,
                            prior.value()->resource_id));
    }
    auto current = control_plane_.get_job(job.resource_id, cancellation);
    if (!current) {
        return base::Result<contracts::CommandAcknowledgement>::failure(current.error());
    }
    if (!current.value() || (current.value()->state != contracts::ServiceJobState::kRunning &&
                             current.value()->state != contracts::ServiceJobState::kCancelling)) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {current.value() ? base::ErrorCode::kConflict : base::ErrorCode::kNotFound,
             "job is not cancellable"});
    }
    auto result = persist_cancel_command(control_plane_, clock_, random_, *current.value(), job,
                                         idempotency_key, fingerprint, cancellation);
    if (!result)
        return result;
    auto signalled = supervisor_.cancel_job(job.resource_id, {});
    if (!signalled && signalled.error().code != base::ErrorCode::kNotFound) {
        return base::Result<contracts::CommandAcknowledgement>::failure(signalled.error());
    }
    return result;
}

} // namespace aegra::apps::service
