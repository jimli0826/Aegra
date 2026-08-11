#include "aegra/adapters/sqlite/sqlite_control_plane.h"

#include "sqlite_internal.h"

#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

namespace aegra::adapters::sqlite {
namespace {

[[nodiscard]] base::Result<void> configure_connection(sqlite3* const db) {
    auto foreign_keys = detail::exec_sql(db, "PRAGMA foreign_keys = ON");
    if (!foreign_keys) {
        return foreign_keys;
    }
    auto busy = detail::exec_sql(db, "PRAGMA busy_timeout = 3000");
    if (!busy) {
        return busy;
    }
    // DELETE journal keeps single-file tests simple; Service remains single-writer.
    auto journal = detail::exec_sql(db, "PRAGMA journal_mode = DELETE");
    if (!journal) {
        return journal;
    }
    return detail::exec_sql(db, "PRAGMA synchronous = FULL");
}

[[nodiscard]] base::Result<void> migrate_if_needed(sqlite3* const db) {
    auto begin = detail::exec_sql(db, "BEGIN IMMEDIATE");
    if (!begin) {
        return begin;
    }
    auto schema = detail::apply_schema_v3(db);
    if (!schema) {
        (void)detail::exec_sql(db, "ROLLBACK");
        return schema;
    }
    auto version = detail::read_schema_version(db);
    if (!version) {
        (void)detail::exec_sql(db, "ROLLBACK");
        return base::Result<void>::failure(version.error());
    }
    // Unreleased product: no schema migration/compat path (Agents.md / engineering standard).
    // version 0 = brand-new file after CREATE IF NOT EXISTS; any other version must already
    // equal the current schema or the developer deletes the local control-plane.db.
    if (version.value() == 0) {
        auto written = detail::write_schema_version(db, ports::kControlPlaneSchemaVersion);
        if (!written) {
            (void)detail::exec_sql(db, "ROLLBACK");
            return written;
        }
    } else if (version.value() != ports::kControlPlaneSchemaVersion) {
        (void)detail::exec_sql(db, "ROLLBACK");
        return base::Result<void>::failure(detail::make_error(
            base::ErrorCode::kUnsupportedVersion,
            "control plane schema version is unsupported; delete the local database and recreate"));
    }
    auto commit = detail::exec_sql(db, "COMMIT");
    if (!commit) {
        (void)detail::exec_sql(db, "ROLLBACK");
        return commit;
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::filesystem::path>
resolve_database_path(const SqliteControlPlaneOpenRequest& request) {
    std::error_code error_code;
    auto path = request.database_path;
    if (path.empty()) {
        return base::Result<std::filesystem::path>::failure(
            detail::make_error(base::ErrorCode::kInvalidArgument, "database path is empty"));
    }
    if (!path.is_absolute()) {
        path = std::filesystem::absolute(path, error_code);
        if (error_code) {
            return base::Result<std::filesystem::path>::failure(
                detail::make_error(base::ErrorCode::kIoFailure, "database path resolve failed"));
        }
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        if (request.mode == SqliteOpenMode::kCreateIfMissing) {
            std::filesystem::create_directories(parent, error_code);
            if (error_code) {
                return base::Result<std::filesystem::path>::failure(detail::make_error(
                    base::ErrorCode::kIoFailure, "database parent directory create failed"));
            }
        } else if (!std::filesystem::exists(parent, error_code) || error_code) {
            return base::Result<std::filesystem::path>::failure(detail::make_error(
                base::ErrorCode::kNotFound, "database parent directory missing"));
        }
    }
    if (request.mode == SqliteOpenMode::kOpenExisting) {
        if (!std::filesystem::exists(path, error_code) || error_code) {
            return base::Result<std::filesystem::path>::failure(
                detail::make_error(base::ErrorCode::kNotFound, "control plane database not found"));
        }
    }
    return base::Result<std::filesystem::path>::success(std::move(path));
}

} // namespace

namespace detail {

SqliteControlPlaneState::~SqliteControlPlaneState() {
    if (db == nullptr) {
        return;
    }
    if (write_transaction_open) {
        (void)exec_sql(db, "ROLLBACK");
    }
    (void)sqlite3_close(db);
    db = nullptr;
}

ControlPlaneUnitOfWork::ControlPlaneUnitOfWork(std::shared_ptr<SqliteControlPlaneState> state,
                                               std::unique_lock<std::mutex> write_lock)
    : state_(std::move(state)), write_lock_(std::move(write_lock)),
      repository_connections_(*state_, &active_), jobs_(*state_, &active_),
      schedules_(*state_, &active_), audit_events_(*state_, &active_), commands_(*state_, &active_),
      restore_preflights_(*state_, &active_), service_settings_(*state_, &active_) {}

ControlPlaneUnitOfWork::~ControlPlaneUnitOfWork() { rollback(); }

ports::IRepositoryConnectionStore& ControlPlaneUnitOfWork::repository_connections() noexcept {
    return repository_connections_;
}

ports::IJobStore& ControlPlaneUnitOfWork::jobs() noexcept { return jobs_; }

ports::IScheduleStore& ControlPlaneUnitOfWork::schedules() noexcept { return schedules_; }

ports::IAuditEventStore& ControlPlaneUnitOfWork::audit_events() noexcept { return audit_events_; }

ports::ICommandStore& ControlPlaneUnitOfWork::commands() noexcept { return commands_; }

ports::IRestorePreflightStore& ControlPlaneUnitOfWork::restore_preflights() noexcept {
    return restore_preflights_;
}

ports::IServiceSettingsStore& ControlPlaneUnitOfWork::service_settings() noexcept {
    return service_settings_;
}

void ControlPlaneUnitOfWork::finish_unlocked() noexcept {
    active_ = false;
    if (state_ != nullptr) {
        state_->write_transaction_open = false;
    }
    if (write_lock_.owns_lock()) {
        write_lock_.unlock();
    }
}

base::Result<void> ControlPlaneUnitOfWork::commit(const base::CancellationToken cancellation) {
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    if (!active_ || !write_lock_.owns_lock()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kConflict, "unit of work already finished"));
    }
    auto commit = exec_sql(state_->db, "COMMIT");
    if (!commit) {
        (void)exec_sql(state_->db, "ROLLBACK");
        finish_unlocked();
        return commit;
    }
    finish_unlocked();
    return base::Result<void>::success();
}

void ControlPlaneUnitOfWork::rollback() noexcept {
    if (!active_ || state_ == nullptr || state_->db == nullptr || !write_lock_.owns_lock()) {
        return;
    }
    (void)exec_sql(state_->db, "ROLLBACK");
    finish_unlocked();
}

} // namespace detail

base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>
SqliteControlPlaneDatabase::open(const SqliteControlPlaneOpenRequest& request) {
    auto path = resolve_database_path(request);
    if (!path) {
        return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(path.error());
    }
    const auto utf8_path = path.value().u8string();
    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE |
                      (request.mode == SqliteOpenMode::kCreateIfMissing ? SQLITE_OPEN_CREATE : 0) |
                      SQLITE_OPEN_FULLMUTEX;
    const int rc =
        sqlite3_open_v2(reinterpret_cast<const char*>(utf8_path.c_str()), &db, flags, nullptr);
    if (rc != SQLITE_OK || db == nullptr) {
        if (db != nullptr) {
            sqlite3_close(db);
        }
        auto mapped = detail::map_sqlite_result(rc, nullptr, "open control plane database failed");
        if (mapped) {
            return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(
                detail::make_error(base::ErrorCode::kIoFailure,
                                   "open control plane database failed"));
        }
        return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(mapped.error());
    }
    auto configured = configure_connection(db);
    if (!configured) {
        sqlite3_close(db);
        return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(
            configured.error());
    }
    auto migrated = migrate_if_needed(db);
    if (!migrated) {
        sqlite3_close(db);
        return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(migrated.error());
    }
    auto version = detail::read_schema_version(db);
    if (!version) {
        sqlite3_close(db);
        return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::failure(version.error());
    }
    auto state = std::make_shared<detail::SqliteControlPlaneState>();
    state->db = db;
    state->schema_version = version.value();
    return base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>::success(
        std::unique_ptr<SqliteControlPlaneDatabase>(
            new SqliteControlPlaneDatabase(std::move(state))));
}

SqliteControlPlaneDatabase::SqliteControlPlaneDatabase(
    std::shared_ptr<detail::SqliteControlPlaneState> state) noexcept
    : state_(std::move(state)) {}

SqliteControlPlaneDatabase::~SqliteControlPlaneDatabase() = default;

std::uint32_t SqliteControlPlaneDatabase::schema_version() const noexcept {
    return state_->schema_version;
}

base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>
SqliteControlPlaneDatabase::begin_unit_of_work(const base::CancellationToken cancellation) {
    auto cancelled = detail::check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>::failure(
            cancelled.error());
    }
    // Exclusive write lock is held until commit/rollback so public readers cannot observe
    // uncommitted rows on the shared connection.
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>::failure(
            detail::make_error(base::ErrorCode::kConflict, "control plane database is busy"));
    }
    if (state_->write_transaction_open) {
        return base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>::failure(
            detail::make_error(base::ErrorCode::kConflict, "write transaction already open"));
    }
    auto begin = detail::exec_sql(state_->db, "BEGIN IMMEDIATE");
    if (!begin) {
        return base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>::failure(
            begin.error());
    }
    state_->write_transaction_open = true;
    return base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>::success(
        std::make_unique<detail::ControlPlaneUnitOfWork>(state_, std::move(lock)));
}

base::Result<std::optional<ports::RepositoryConnectionRecord>>
SqliteControlPlaneDatabase::get_repository_connection(const std::string_view connection_id,
                                                      const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::RepositoryConnectionStore store(*state_);
    return store.get(connection_id, cancellation);
}

base::Result<contracts::RepositoryConnectionPage>
SqliteControlPlaneDatabase::list_repository_connections(
    const contracts::RepositoryConnectionListRequest& request,
    const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::RepositoryConnectionStore store(*state_);
    return store.list(request, cancellation);
}

base::Result<std::optional<ports::JobRecord>>
SqliteControlPlaneDatabase::get_job(const std::string_view job_id,
                                    const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::JobStore store(*state_);
    return store.get(job_id, cancellation);
}

base::Result<std::optional<ports::JobRecord>>
SqliteControlPlaneDatabase::get_job_by_idempotency_key(const std::string_view idempotency_key,
                                                       const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    if (auto cancelled = detail::check_cancelled(cancellation); !cancelled) {
        return base::Result<std::optional<ports::JobRecord>>::failure(cancelled.error());
    }
    auto statement = detail::SqliteStatement::prepare(
        state_->db,
        "SELECT job_id, trace_id, operation, state, content_kind, created_utc_ms, started_utc_ms, "
        "completed_utc_ms, source_ids, repository_connection_id, target_source_id, backup_type, "
        "parent_recovery_point_id, preflight_token, message_code, idempotency_key, "
        "result_error_code, result_outcome, result_message_code, "
        "exclude_page_and_hibernation_files, request_fingerprint, "
        "result_requested_backup_type, result_effective_backup_type, "
        "result_effective_parent_uuid, result_incremental_downgrade_reason, schedule_id FROM jobs "
        "WHERE idempotency_key = ?");
    if (!statement) {
        return base::Result<std::optional<ports::JobRecord>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, idempotency_key); !bound) {
        return base::Result<std::optional<ports::JobRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::JobRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::JobRecord>>::success(std::nullopt);
    }
    auto record = detail::read_job(statement.value().get());
    return record
               ? base::Result<std::optional<ports::JobRecord>>::success(std::move(record).value())
               : base::Result<std::optional<ports::JobRecord>>::failure(record.error());
}

base::Result<std::optional<ports::JobRecord>>
SqliteControlPlaneDatabase::get_job_by_preflight_token(const std::string_view preflight_token,
                                                       const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    if (auto cancelled = detail::check_cancelled(cancellation); !cancelled) {
        return base::Result<std::optional<ports::JobRecord>>::failure(cancelled.error());
    }
    auto statement = detail::SqliteStatement::prepare(
        state_->db,
        "SELECT job_id, trace_id, operation, state, content_kind, created_utc_ms, started_utc_ms, "
        "completed_utc_ms, source_ids, repository_connection_id, target_source_id, backup_type, "
        "parent_recovery_point_id, preflight_token, message_code, idempotency_key, "
        "result_error_code, result_outcome, result_message_code, "
        "exclude_page_and_hibernation_files, request_fingerprint, "
        "result_requested_backup_type, result_effective_backup_type, "
        "result_effective_parent_uuid, result_incremental_downgrade_reason, schedule_id FROM jobs "
        "WHERE preflight_token = ?");
    if (!statement) {
        return base::Result<std::optional<ports::JobRecord>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, preflight_token); !bound) {
        return base::Result<std::optional<ports::JobRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::JobRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::JobRecord>>::success(std::nullopt);
    }
    auto record = detail::read_job(statement.value().get());
    return record
               ? base::Result<std::optional<ports::JobRecord>>::success(std::move(record).value())
               : base::Result<std::optional<ports::JobRecord>>::failure(record.error());
}

base::Result<contracts::JobPage>
SqliteControlPlaneDatabase::list_jobs(const contracts::JobListRequest& request,
                                      const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::JobStore store(*state_);
    return store.list(request, cancellation);
}

base::Result<std::optional<ports::ScheduleRecord>>
SqliteControlPlaneDatabase::get_schedule(const std::string_view schedule_id,
                                         const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::ScheduleStore store(*state_);
    return store.get(schedule_id, cancellation);
}

base::Result<contracts::SchedulePage>
SqliteControlPlaneDatabase::list_schedules(const contracts::ScheduleListRequest& request,
                                           const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::ScheduleStore store(*state_);
    return store.list(request, cancellation);
}

base::Result<contracts::AuditEventPage>
SqliteControlPlaneDatabase::list_audit_events(const contracts::AuditEventListRequest& request,
                                              const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::AuditEventStore store(*state_);
    return store.list(request, cancellation);
}

base::Result<std::optional<ports::CommandRecord>>
SqliteControlPlaneDatabase::get_command(const std::string_view idempotency_key,
                                        const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::CommandStore store(*state_);
    return store.get(idempotency_key, cancellation);
}

base::Result<std::optional<ports::RestorePreflightRecord>>
SqliteControlPlaneDatabase::get_restore_preflight(const std::string_view preflight_token,
                                                  const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::RestorePreflightStore store(*state_);
    return store.get(preflight_token, cancellation);
}

base::Result<ports::ServiceSettingsRecord>
SqliteControlPlaneDatabase::get_service_settings(const base::CancellationToken cancellation) {
    std::lock_guard lock(state_->mutex);
    detail::ServiceSettingsStore store(*state_);
    return store.get(cancellation);
}

} // namespace aegra::adapters::sqlite
