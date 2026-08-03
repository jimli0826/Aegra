#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::ports {

// Personal-edition control-plane schema version for durable local SQLite.
// Not Recovery Point / Archive / Chunk Index authority.
inline constexpr std::uint32_t kControlPlaneSchemaVersion = 1;

// ---- Durable records (control-plane only; no plaintext secrets, no RP authority) ----

struct RepositoryConnectionRecord final {
    std::string connection_id;
    std::string display_name;
    std::string locator;
    std::optional<contracts::SecretRef> credential_ref;
    contracts::RepositoryConnectionState state{contracts::RepositoryConnectionState::kUnavailable};
    bool is_default{false};
    std::vector<std::string> capabilities;
    std::uint64_t created_utc_ms{0};
    std::uint64_t updated_utc_ms{0};
};

struct JobRecord final {
    std::string job_id;
    std::string trace_id;
    contracts::JobOperation operation{contracts::JobOperation::kBackup};
    contracts::ServiceJobState state{contracts::ServiceJobState::kQueued};
    std::uint64_t created_utc_ms{0};
    std::optional<std::uint64_t> started_utc_ms;
    std::optional<std::uint64_t> completed_utc_ms;
    std::optional<std::string> source_id;
    std::optional<std::string> repository_connection_id;
    std::optional<std::string> target_source_id;
    std::optional<contracts::BackupType> backup_type;
    std::optional<std::string> parent_recovery_point_id;
    std::optional<std::string> preflight_token;
    std::string message_code;
    std::optional<std::string> idempotency_key;
    std::optional<std::uint32_t> result_error_code;
    std::optional<std::uint32_t> result_outcome;
    std::optional<std::string> result_message_code;
};

struct ScheduleRecord final {
    std::string schedule_id;
    std::string display_name;
    bool enabled{false};
    std::string source_id;
    std::string repository_connection_id;
    contracts::BackupType backup_type{contracts::BackupType::kFull};
    contracts::ScheduleTrigger trigger;
    std::optional<std::uint64_t> next_run_utc_ms;
    std::uint64_t created_utc_ms{0};
    std::uint64_t updated_utc_ms{0};
};

struct AuditEventRecord final {
    std::string event_id;
    std::uint64_t created_utc_ms{0};
    contracts::AuditSeverity severity{contracts::AuditSeverity::kInformation};
    std::string message_code;
    contracts::MessageArguments message_arguments;
    std::string correlation_id;
};

// ---- Job state machine (shared pure rules) ----

[[nodiscard]] constexpr bool is_terminal_job_state(const contracts::ServiceJobState state) noexcept {
    return state == contracts::ServiceJobState::kSucceeded ||
           state == contracts::ServiceJobState::kFailed ||
           state == contracts::ServiceJobState::kCancelled ||
           state == contracts::ServiceJobState::kInterrupted;
}

[[nodiscard]] constexpr bool is_active_job_state(const contracts::ServiceJobState state) noexcept {
    return state == contracts::ServiceJobState::kQueued ||
           state == contracts::ServiceJobState::kRunning ||
           state == contracts::ServiceJobState::kCancelling;
}

// Returns true when a single-step transition is legal for the personal Service control plane.
[[nodiscard]] constexpr bool
is_valid_job_state_transition(const contracts::ServiceJobState from,
                              const contracts::ServiceJobState to) noexcept {
    if (from == to) {
        return false;
    }
    switch (from) {
    case contracts::ServiceJobState::kQueued:
        return to == contracts::ServiceJobState::kRunning ||
               to == contracts::ServiceJobState::kCancelled ||
               to == contracts::ServiceJobState::kInterrupted;
    case contracts::ServiceJobState::kRunning:
        return to == contracts::ServiceJobState::kCancelling ||
               to == contracts::ServiceJobState::kSucceeded ||
               to == contracts::ServiceJobState::kFailed ||
               to == contracts::ServiceJobState::kCancelled ||
               to == contracts::ServiceJobState::kInterrupted;
    case contracts::ServiceJobState::kCancelling:
        return to == contracts::ServiceJobState::kCancelled ||
               to == contracts::ServiceJobState::kSucceeded ||
               to == contracts::ServiceJobState::kFailed ||
               to == contracts::ServiceJobState::kInterrupted;
    case contracts::ServiceJobState::kSucceeded:
    case contracts::ServiceJobState::kFailed:
    case contracts::ServiceJobState::kCancelled:
    case contracts::ServiceJobState::kInterrupted:
        return false;
    }
    return false;
}

// ---- Store ports ----

class IRepositoryConnectionStore {
  public:
    IRepositoryConnectionStore() = default;
    virtual ~IRepositoryConnectionStore() = default;
    IRepositoryConnectionStore(const IRepositoryConnectionStore&) = delete;
    IRepositoryConnectionStore& operator=(const IRepositoryConnectionStore&) = delete;
    IRepositoryConnectionStore(IRepositoryConnectionStore&&) = delete;
    IRepositoryConnectionStore& operator=(IRepositoryConnectionStore&&) = delete;

    // Insert or replace by connection_id. Enforces at most one default in the same transaction.
    [[nodiscard]] virtual base::Result<void>
    upsert(const RepositoryConnectionRecord& record, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<std::optional<RepositoryConnectionRecord>>
    get(std::string_view connection_id, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::RepositoryConnectionPage>
    list(const contracts::RepositoryConnectionListRequest& request,
         base::CancellationToken cancellation) = 0;

    // Clears default from every other row when making this connection the default.
    [[nodiscard]] virtual base::Result<void>
    set_default(std::string_view connection_id, base::CancellationToken cancellation) = 0;

    // Removes control-plane reference only; never deletes Repository objects or Archives.
    [[nodiscard]] virtual base::Result<void> remove(std::string_view connection_id,
                                                    base::CancellationToken cancellation) = 0;
};

struct JobStateTransition final {
    std::string job_id;
    contracts::ServiceJobState expected_state{contracts::ServiceJobState::kQueued};
    contracts::ServiceJobState next_state{contracts::ServiceJobState::kRunning};
    std::uint64_t transition_utc_ms{0};
    std::string message_code;
    std::optional<std::uint32_t> result_error_code;
    std::optional<std::uint32_t> result_outcome;
    std::optional<std::string> result_message_code;
};

class IJobStore {
  public:
    IJobStore() = default;
    virtual ~IJobStore() = default;
    IJobStore(const IJobStore&) = delete;
    IJobStore& operator=(const IJobStore&) = delete;
    IJobStore(IJobStore&&) = delete;
    IJobStore& operator=(IJobStore&&) = delete;

    [[nodiscard]] virtual base::Result<void> insert(const JobRecord& record,
                                                    base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<std::optional<JobRecord>>
    get(std::string_view job_id, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::JobPage>
    list(const contracts::JobListRequest& request, base::CancellationToken cancellation) = 0;

    // CAS transition: fails with Conflict when expected_state does not match or transition is illegal.
    [[nodiscard]] virtual base::Result<JobRecord>
    transition(const JobStateTransition& transition, base::CancellationToken cancellation) = 0;

    // Startup recovery: every running/cancelling job becomes interrupted with completed_utc_ms set.
    // Returns the number of rows updated.
    [[nodiscard]] virtual base::Result<std::uint64_t>
    mark_active_as_interrupted(std::uint64_t interrupted_utc_ms,
                               base::CancellationToken cancellation) = 0;
};

class IScheduleStore {
  public:
    IScheduleStore() = default;
    virtual ~IScheduleStore() = default;
    IScheduleStore(const IScheduleStore&) = delete;
    IScheduleStore& operator=(const IScheduleStore&) = delete;
    IScheduleStore(IScheduleStore&&) = delete;
    IScheduleStore& operator=(IScheduleStore&&) = delete;

    [[nodiscard]] virtual base::Result<void> upsert(const ScheduleRecord& record,
                                                    base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<std::optional<ScheduleRecord>>
    get(std::string_view schedule_id, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::SchedulePage>
    list(const contracts::ScheduleListRequest& request, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> remove(std::string_view schedule_id,
                                                    base::CancellationToken cancellation) = 0;
};

class IAuditEventStore {
  public:
    IAuditEventStore() = default;
    virtual ~IAuditEventStore() = default;
    IAuditEventStore(const IAuditEventStore&) = delete;
    IAuditEventStore& operator=(const IAuditEventStore&) = delete;
    IAuditEventStore(IAuditEventStore&&) = delete;
    IAuditEventStore& operator=(IAuditEventStore&&) = delete;

    // Append-only. Duplicate event_id returns Conflict.
    [[nodiscard]] virtual base::Result<void> append(const AuditEventRecord& record,
                                                    base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::AuditEventPage>
    list(const contracts::AuditEventListRequest& request, base::CancellationToken cancellation) = 0;
};

// Single-writer unit of work. Commit is explicit; destruction without commit rolls back.
// After commit or rollback, every Store reference obtained from this unit rejects further access.
class IControlPlaneUnitOfWork {
  public:
    IControlPlaneUnitOfWork() = default;
    virtual ~IControlPlaneUnitOfWork() = default;
    IControlPlaneUnitOfWork(const IControlPlaneUnitOfWork&) = delete;
    IControlPlaneUnitOfWork& operator=(const IControlPlaneUnitOfWork&) = delete;
    IControlPlaneUnitOfWork(IControlPlaneUnitOfWork&&) = delete;
    IControlPlaneUnitOfWork& operator=(IControlPlaneUnitOfWork&&) = delete;

    [[nodiscard]] virtual IRepositoryConnectionStore& repository_connections() noexcept = 0;
    [[nodiscard]] virtual IJobStore& jobs() noexcept = 0;
    [[nodiscard]] virtual IScheduleStore& schedules() noexcept = 0;
    [[nodiscard]] virtual IAuditEventStore& audit_events() noexcept = 0;

    [[nodiscard]] virtual base::Result<void> commit(base::CancellationToken cancellation) = 0;
    virtual void rollback() noexcept = 0;
};

class IControlPlaneDatabase {
  public:
    IControlPlaneDatabase() = default;
    virtual ~IControlPlaneDatabase() = default;
    IControlPlaneDatabase(const IControlPlaneDatabase&) = delete;
    IControlPlaneDatabase& operator=(const IControlPlaneDatabase&) = delete;
    IControlPlaneDatabase(IControlPlaneDatabase&&) = delete;
    IControlPlaneDatabase& operator=(IControlPlaneDatabase&&) = delete;

    [[nodiscard]] virtual std::uint32_t schema_version() const noexcept = 0;

    // Begins an exclusive write transaction for the Service single-writer model.
    [[nodiscard]] virtual base::Result<std::unique_ptr<IControlPlaneUnitOfWork>>
    begin_unit_of_work(base::CancellationToken cancellation) = 0;

    // Read-only snapshot helpers for queries outside an explicit unit of work.
    [[nodiscard]] virtual base::Result<std::optional<RepositoryConnectionRecord>>
    get_repository_connection(std::string_view connection_id,
                              base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::RepositoryConnectionPage>
    list_repository_connections(const contracts::RepositoryConnectionListRequest& request,
                                base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<std::optional<JobRecord>>
    get_job(std::string_view job_id, base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::JobPage>
    list_jobs(const contracts::JobListRequest& request, base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<std::optional<ScheduleRecord>>
    get_schedule(std::string_view schedule_id, base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::SchedulePage>
    list_schedules(const contracts::ScheduleListRequest& request,
                   base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::AuditEventPage>
    list_audit_events(const contracts::AuditEventListRequest& request,
                      base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
