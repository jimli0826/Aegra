#include "aegra/apps/service/worker_job_service.h"

#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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
    return record.operation == contracts::JobOperation::kBackup &&
           record.source_id == command.source_id &&
           record.repository_connection_id == command.repository_connection_id &&
           record.backup_type == command.backup_type &&
           record.parent_recovery_point_id == command.parent_recovery_point_id;
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
               ports::IControlPlaneDatabase& control_plane, ports::IRandomSource& random,
               const base::CancellationToken cancellation) {
    auto source = source_inventory.resolve_source(command.source_id, cancellation);
    if (!source)
        return base::Result<PreparedBackup>::failure(source.error());
    auto repository =
        control_plane.get_repository_connection(command.repository_connection_id, cancellation);
    if (!repository)
        return base::Result<PreparedBackup>::failure(repository.error());
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable ||
        !repository.value()->credential_ref) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kConflict, "repository is unavailable or has no credential"});
    }
    auto root = path_from_utf8(repository.value()->locator);
    auto job_id = random_id("job-", random, cancellation);
    auto trace_id = random_id("trace-", random, cancellation);
    if (!root || !job_id || !trace_id) {
        if (!root)
            return base::Result<PreparedBackup>::failure(root.error());
        return base::Result<PreparedBackup>::failure(!job_id ? job_id.error() : trace_id.error());
    }
    const auto archive_directory = root.value() / L"archives";
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
    worker.source_refs = {source.value().stable_key};
    worker.target_ref = path_to_utf8(archive_directory / (job_id.value() + ".bkf"));
    worker.credential_refs = {*repository.value()->credential_ref};
    worker.backup = contracts::BackupOptions{contracts::BackupType::kFull, {}, {}};
    worker.trace_id = trace_id.value();
    WorkerJobRequest request{std::move(worker),
                             command.source_id,
                             command.repository_connection_id,
                             std::nullopt,
                             {},
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

WorkerJobService::WorkerJobService(application::ISourceInventoryQuery& source_inventory,
                                   ports::IControlPlaneDatabase& control_plane,
                                   WorkerSupervisor& supervisor, ports::IClock& clock,
                                   ports::IRandomSource& random) noexcept
    : source_inventory_(source_inventory), control_plane_(control_plane), supervisor_(supervisor),
      clock_(clock), random_(random) {}

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
        prepare_backup(command, source_inventory_, control_plane_, random_, cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }
    prepared.value().request.idempotency_key = std::string(idempotency_key);
    auto submitted = supervisor_.submit(prepared.value().request, cancellation);
    if (!submitted) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submitted.error());
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
