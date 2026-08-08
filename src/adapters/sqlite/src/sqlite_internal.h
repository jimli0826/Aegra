#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/control_plane.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::sqlite::detail {

struct SqliteControlPlaneState final {
    ~SqliteControlPlaneState();

    mutable std::mutex mutex;
    sqlite3* db{nullptr};
    std::uint32_t schema_version{0};
    bool write_transaction_open{false};
};

[[nodiscard]] base::Error make_error(base::ErrorCode code, const char* message);
[[nodiscard]] base::Result<void> check_cancelled(base::CancellationToken cancellation);
[[nodiscard]] base::Result<void> check_unit_of_work_active(const bool* active);
[[nodiscard]] base::Result<void> map_sqlite_result(int rc, sqlite3* db, const char* context);

class SqliteStatement final {
  public:
    SqliteStatement() = default;
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    [[nodiscard]] static base::Result<SqliteStatement> prepare(sqlite3* db, std::string_view sql);
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }
    [[nodiscard]] base::Result<void> bind_text(int index, std::string_view value);
    [[nodiscard]] base::Result<void> bind_text_nullable(int index,
                                                        const std::optional<std::string>& value);
    [[nodiscard]] base::Result<void> bind_int64(int index, std::int64_t value);
    [[nodiscard]] base::Result<void> bind_int64_nullable(int index,
                                                         const std::optional<std::uint64_t>& value);
    [[nodiscard]] base::Result<void> bind_null(int index);
    [[nodiscard]] base::Result<int> step();
    [[nodiscard]] base::Result<void> reset();
    void finalize() noexcept;

  private:
    explicit SqliteStatement(sqlite3_stmt* stmt) noexcept;
    sqlite3_stmt* stmt_{nullptr};
};

[[nodiscard]] std::string encode_string_list(const std::vector<std::string>& values);
[[nodiscard]] base::Result<std::vector<std::string>> decode_string_list(std::string_view encoded);
[[nodiscard]] std::string encode_message_arguments(const contracts::MessageArguments& arguments);
[[nodiscard]] base::Result<contracts::MessageArguments>
decode_message_arguments(std::string_view encoded);

[[nodiscard]] std::optional<std::string> column_text_optional(sqlite3_stmt* stmt, int index);
[[nodiscard]] std::string column_text_required(sqlite3_stmt* stmt, int index);
[[nodiscard]] std::uint64_t column_uint64(sqlite3_stmt* stmt, int index);
[[nodiscard]] std::optional<std::uint64_t> column_uint64_optional(sqlite3_stmt* stmt, int index);

[[nodiscard]] base::Result<void>
validate_repository_connection_record(const ports::RepositoryConnectionRecord& record);
[[nodiscard]] base::Result<void> validate_job_record(const ports::JobRecord& record);
[[nodiscard]] base::Result<void> validate_schedule_record(const ports::ScheduleRecord& record);
[[nodiscard]] base::Result<void> validate_audit_event_record(const ports::AuditEventRecord& record);
[[nodiscard]] base::Result<void> validate_command_record(const ports::CommandRecord& record);
[[nodiscard]] base::Result<void>
validate_restore_preflight_record(const ports::RestorePreflightRecord& record);
[[nodiscard]] base::Result<void>
validate_job_transition(const ports::JobStateTransition& transition);

[[nodiscard]] base::Result<void> exec_sql(sqlite3* db, const char* sql);
[[nodiscard]] base::Result<void> apply_schema_v3(sqlite3* db);
[[nodiscard]] base::Result<std::uint32_t> read_schema_version(sqlite3* db);
[[nodiscard]] base::Result<void> write_schema_version(sqlite3* db, std::uint32_t version);

[[nodiscard]] base::Result<ports::RepositoryConnectionRecord>
read_repository_connection(sqlite3_stmt* stmt);
[[nodiscard]] base::Result<ports::JobRecord> read_job(sqlite3_stmt* stmt);
[[nodiscard]] base::Result<ports::ScheduleRecord> read_schedule(sqlite3_stmt* stmt);
[[nodiscard]] base::Result<ports::AuditEventRecord> read_audit_event(sqlite3_stmt* stmt);
[[nodiscard]] base::Result<ports::CommandRecord> read_command(sqlite3_stmt* stmt);
[[nodiscard]] base::Result<ports::RestorePreflightRecord>
read_restore_preflight(sqlite3_stmt* stmt);

[[nodiscard]] contracts::RepositoryConnectionSummary
to_connection_summary(const ports::RepositoryConnectionRecord& record);
[[nodiscard]] contracts::JobSummary to_job_summary(const ports::JobRecord& record);
[[nodiscard]] contracts::ScheduleSummary to_schedule_summary(const ports::ScheduleRecord& record);
[[nodiscard]] contracts::AuditEventSummary to_audit_summary(const ports::AuditEventRecord& record);

class RepositoryConnectionStore final : public ports::IRepositoryConnectionStore {
  public:
    explicit RepositoryConnectionStore(SqliteControlPlaneState& state,
                                       const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<void> upsert(const ports::RepositoryConnectionRecord& record,
                                            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::RepositoryConnectionRecord>>
    get(std::string_view connection_id, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::RepositoryConnectionPage>
    list(const contracts::RepositoryConnectionListRequest& request,
         base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> set_default(std::string_view connection_id,
                                                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> remove(std::string_view connection_id,
                                            base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class JobStore final : public ports::IJobStore {
  public:
    explicit JobStore(SqliteControlPlaneState& state,
                      const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<void> insert(const ports::JobRecord& record,
                                            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::JobRecord>>
    get(std::string_view job_id, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::JobPage>
    list(const contracts::JobListRequest& request, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<ports::JobRecord>
    transition(const ports::JobStateTransition& transition,
               base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::uint64_t>
    mark_active_as_interrupted(std::uint64_t interrupted_utc_ms,
                               base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class ScheduleStore final : public ports::IScheduleStore {
  public:
    explicit ScheduleStore(SqliteControlPlaneState& state,
                           const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<void> upsert(const ports::ScheduleRecord& record,
                                            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::ScheduleRecord>>
    get(std::string_view schedule_id, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::SchedulePage>
    list(const contracts::ScheduleListRequest& request,
         base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> remove(std::string_view schedule_id,
                                            base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class AuditEventStore final : public ports::IAuditEventStore {
  public:
    explicit AuditEventStore(SqliteControlPlaneState& state,
                             const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<void> append(const ports::AuditEventRecord& record,
                                            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::AuditEventPage>
    list(const contracts::AuditEventListRequest& request,
         base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class CommandStore final : public ports::ICommandStore {
  public:
    explicit CommandStore(SqliteControlPlaneState& state,
                          const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<std::optional<ports::CommandRecord>>
    get(std::string_view idempotency_key, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> insert(const ports::CommandRecord& record,
                                            base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class RestorePreflightStore final : public ports::IRestorePreflightStore {
  public:
    explicit RestorePreflightStore(SqliteControlPlaneState& state,
                                   const bool* unit_of_work_active = nullptr) noexcept;
    [[nodiscard]] base::Result<void> insert(const ports::RestorePreflightRecord& record,
                                            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::RestorePreflightRecord>>
    get(std::string_view preflight_token, base::CancellationToken cancellation) override;

  private:
    SqliteControlPlaneState& state_;
    const bool* unit_of_work_active_{nullptr};
};

class ControlPlaneUnitOfWork final : public ports::IControlPlaneUnitOfWork {
  public:
    // write_lock must own state->mutex for the full unit-of-work lifetime.
    ControlPlaneUnitOfWork(std::shared_ptr<SqliteControlPlaneState> state,
                           std::unique_lock<std::mutex> write_lock);
    ~ControlPlaneUnitOfWork() override;
    [[nodiscard]] ports::IRepositoryConnectionStore& repository_connections() noexcept override;
    [[nodiscard]] ports::IJobStore& jobs() noexcept override;
    [[nodiscard]] ports::IScheduleStore& schedules() noexcept override;
    [[nodiscard]] ports::IAuditEventStore& audit_events() noexcept override;
    [[nodiscard]] ports::ICommandStore& commands() noexcept override;
    [[nodiscard]] ports::IRestorePreflightStore& restore_preflights() noexcept override;
    [[nodiscard]] base::Result<void> commit(base::CancellationToken cancellation) override;
    void rollback() noexcept override;

  private:
    void finish_unlocked() noexcept;

    std::shared_ptr<SqliteControlPlaneState> state_;
    std::unique_lock<std::mutex> write_lock_;
    bool active_{true};
    RepositoryConnectionStore repository_connections_;
    JobStore jobs_;
    ScheduleStore schedules_;
    AuditEventStore audit_events_;
    CommandStore commands_;
    RestorePreflightStore restore_preflights_;
};

// Opaque continuation: v1|<scope>|<filter>|<created_utc_ms>|<id>, bound to list kind + filters.
inline constexpr std::string_view kPageScopeRepositoryConnections = "rc";
inline constexpr std::string_view kPageScopeJobs = "job";
inline constexpr std::string_view kPageScopeSchedules = "sch";
inline constexpr std::string_view kPageScopeAuditEvents = "aud";

struct PageCursor final {
    std::uint64_t created_utc_ms{0};
    std::string id;
};

[[nodiscard]] std::string
repository_connection_page_filter(const std::optional<contracts::RepositoryConnectionState>& state);
[[nodiscard]] std::string job_page_filter(const std::optional<contracts::JobOperation>& operation,
                                          const std::optional<contracts::ServiceJobState>& state);
[[nodiscard]] std::string schedule_page_filter(const std::optional<bool>& enabled);
[[nodiscard]] std::string
audit_event_page_filter(const std::optional<contracts::AuditSeverity>& minimum_severity,
                        const std::optional<std::uint64_t>& from_utc_ms,
                        const std::optional<std::uint64_t>& to_utc_ms,
                        const std::optional<std::string>& correlation_id);

[[nodiscard]] base::Result<std::optional<PageCursor>>
decode_page_token(const std::optional<std::string>& token, std::string_view expected_scope,
                  std::string_view expected_filter);
[[nodiscard]] std::string encode_page_token(std::string_view scope, std::string_view filter,
                                            std::uint64_t created_utc_ms, std::string_view id);

[[nodiscard]] base::Result<void> clear_default_repository_flags(sqlite3* db);
[[nodiscard]] base::Result<void> begin_savepoint(sqlite3* db, std::string_view name);
[[nodiscard]] base::Result<void> release_savepoint(sqlite3* db, std::string_view name);
void rollback_savepoint(sqlite3* db, std::string_view name) noexcept;
[[nodiscard]] base::Result<void> bind_page_cursor(SqliteStatement& statement, int& index,
                                                  const std::optional<PageCursor>& cursor);

inline constexpr const char* kSelectConnectionSql =
    "SELECT connection_id, display_name, locator, credential_ref, state, is_default, "
    "capabilities, created_utc_ms, updated_utc_ms FROM repository_connections "
    "WHERE connection_id = ?";

inline constexpr const char* kSelectJobSql =
    "SELECT job_id, trace_id, operation, state, content_kind, created_utc_ms, started_utc_ms, "
    "completed_utc_ms, source_ids, repository_connection_id, target_source_id, backup_type, "
    "parent_recovery_point_id, preflight_token, message_code, idempotency_key, result_error_code, "
    "result_outcome, result_message_code, exclude_page_and_hibernation_files, request_fingerprint, "
    "result_requested_backup_type, result_effective_backup_type, result_effective_parent_uuid, "
    "result_incremental_downgrade_reason "
    "FROM jobs WHERE job_id = ?";

inline constexpr const char* kSelectCommandSql =
    "SELECT idempotency_key, request_fingerprint, command_id, resource_id, created_utc_ms "
    "FROM commands WHERE idempotency_key = ?";

inline constexpr const char* kSelectScheduleSql =
    "SELECT schedule_id, display_name, enabled, content_kind, source_ids, owner_sid, "
    "repository_connection_id, backup_type, trigger_kind, local_minute_of_day, weekday_mask, "
    "timezone_id, next_run_utc_ms, exclude_page_and_hibernation_files, encryption_enabled, "
    "archive_password_protected, backup_set_uuid, last_recovery_point_id, created_utc_ms, "
    "updated_utc_ms FROM schedules WHERE schedule_id = ?";

[[nodiscard]] std::string encode_relative_path_blob(
    const std::vector<contracts::EncodedName>& components);
[[nodiscard]] base::Result<std::vector<contracts::EncodedName>>
decode_relative_path_blob(std::string_view encoded);
[[nodiscard]] base::Result<void>
replace_schedule_file_selections(sqlite3* db, std::string_view schedule_id,
                                 const std::vector<contracts::FileSourceRef>& selections);
[[nodiscard]] base::Result<std::vector<contracts::FileSourceRef>>
load_schedule_file_selections(sqlite3* db, std::string_view schedule_id);
[[nodiscard]] base::Result<void>
replace_restore_preflight_entry_ids(sqlite3* db, std::string_view preflight_token,
                                    const std::vector<std::string>& entry_ids);
[[nodiscard]] base::Result<std::vector<std::string>>
load_restore_preflight_entry_ids(sqlite3* db, std::string_view preflight_token);

} // namespace aegra::adapters::sqlite::detail
