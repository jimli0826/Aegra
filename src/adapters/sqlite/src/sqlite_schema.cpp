#include "sqlite_internal.h"

namespace aegra::adapters::sqlite::detail {
base::Result<void> apply_schema_v3(sqlite3* const db) {
    static constexpr char kSchema[] = R"sql(
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS schema_meta (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    version INTEGER NOT NULL CHECK (version > 0)
);
CREATE TABLE IF NOT EXISTS repository_connections (
    connection_id TEXT PRIMARY KEY NOT NULL,
    display_name TEXT NOT NULL,
    locator TEXT NOT NULL,
    credential_ref TEXT,
    state INTEGER NOT NULL CHECK (state IN (1, 2)),
    is_default INTEGER NOT NULL CHECK (is_default IN (0, 1)),
    capabilities TEXT NOT NULL,
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0),
    updated_utc_ms INTEGER NOT NULL CHECK (updated_utc_ms >= 0),
    CHECK (updated_utc_ms >= created_utc_ms)
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_repository_connections_locator
    ON repository_connections(locator);
CREATE UNIQUE INDEX IF NOT EXISTS ux_repository_connections_one_default
    ON repository_connections(is_default) WHERE is_default = 1;
CREATE TABLE IF NOT EXISTS jobs (
    job_id TEXT PRIMARY KEY NOT NULL,
    trace_id TEXT NOT NULL,
    operation INTEGER NOT NULL CHECK (operation BETWEEN 1 AND 4),
    state INTEGER NOT NULL CHECK (state BETWEEN 1 AND 7),
    content_kind INTEGER NOT NULL DEFAULT 1 CHECK (content_kind IN (1, 2)),
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0),
    started_utc_ms INTEGER CHECK (started_utc_ms IS NULL OR started_utc_ms >= 0),
    completed_utc_ms INTEGER CHECK (completed_utc_ms IS NULL OR completed_utc_ms >= 0),
    source_ids TEXT NOT NULL,
    repository_connection_id TEXT,
    target_source_id TEXT,
    backup_type INTEGER CHECK (backup_type IS NULL OR backup_type BETWEEN 1 AND 3),
    parent_recovery_point_id TEXT,
    preflight_token TEXT,
    message_code TEXT NOT NULL,
    idempotency_key TEXT,
    result_error_code INTEGER,
    result_outcome INTEGER,
    result_message_code TEXT,
    exclude_page_and_hibernation_files INTEGER
        CHECK (exclude_page_and_hibernation_files IS NULL
               OR exclude_page_and_hibernation_files IN (0, 1)),
    request_fingerprint TEXT NOT NULL DEFAULT '',
    result_requested_backup_type INTEGER
        CHECK (result_requested_backup_type IS NULL
               OR result_requested_backup_type BETWEEN 1 AND 3),
    result_effective_backup_type INTEGER
        CHECK (result_effective_backup_type IS NULL
               OR result_effective_backup_type BETWEEN 1 AND 3),
    result_effective_parent_uuid TEXT,
    result_incremental_downgrade_reason INTEGER
        CHECK (result_incremental_downgrade_reason IS NULL
               OR result_incremental_downgrade_reason BETWEEN 0 AND 9),
    schedule_id TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (repository_connection_id)
        REFERENCES repository_connections(connection_id) ON DELETE SET NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_jobs_idempotency_key
    ON jobs(idempotency_key) WHERE idempotency_key IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS ux_jobs_preflight_token
    ON jobs(preflight_token) WHERE preflight_token IS NOT NULL;
CREATE INDEX IF NOT EXISTS ix_jobs_created ON jobs(created_utc_ms DESC, job_id ASC);
CREATE INDEX IF NOT EXISTS ix_jobs_state ON jobs(state, created_utc_ms DESC);
CREATE TABLE IF NOT EXISTS schedules (
    schedule_id TEXT PRIMARY KEY NOT NULL,
    display_name TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),
    content_kind INTEGER NOT NULL DEFAULT 1 CHECK (content_kind IN (1, 2)),
    source_ids TEXT NOT NULL,
    owner_sid TEXT NOT NULL DEFAULT '',
    repository_connection_id TEXT NOT NULL,
    backup_type INTEGER NOT NULL CHECK (backup_type BETWEEN 1 AND 3),
    trigger_kind INTEGER NOT NULL CHECK (trigger_kind IN (1, 2)),
    local_minute_of_day INTEGER NOT NULL CHECK (local_minute_of_day >= 0 AND local_minute_of_day < 1440),
    weekday_mask INTEGER NOT NULL CHECK (weekday_mask >= 0 AND weekday_mask <= 127),
    timezone_id TEXT NOT NULL,
    next_run_utc_ms INTEGER CHECK (next_run_utc_ms IS NULL OR next_run_utc_ms >= 0),
    exclude_page_and_hibernation_files INTEGER NOT NULL DEFAULT 1
        CHECK (exclude_page_and_hibernation_files IN (0, 1)),
    deduplication_enabled INTEGER NOT NULL DEFAULT 1
        CHECK (deduplication_enabled IN (0, 1)),
    encryption_enabled INTEGER NOT NULL DEFAULT 0
        CHECK (encryption_enabled IN (0, 1)),
    archive_password_protected TEXT NOT NULL DEFAULT '',
    backup_set_uuid TEXT NOT NULL,
    last_recovery_point_id TEXT,
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0),
    updated_utc_ms INTEGER NOT NULL CHECK (updated_utc_ms >= 0),
    CHECK (updated_utc_ms >= created_utc_ms),
    FOREIGN KEY (repository_connection_id)
        REFERENCES repository_connections(connection_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS ix_schedules_enabled ON schedules(enabled, schedule_id);
CREATE TABLE IF NOT EXISTS schedule_file_selections (
    schedule_id TEXT NOT NULL,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 100),
    selection_id TEXT NOT NULL,
    volume_identity TEXT NOT NULL,
    relative_path_blob TEXT NOT NULL,
    entry_kind INTEGER NOT NULL CHECK (entry_kind IN (1, 2)),
    recursion INTEGER NOT NULL CHECK (recursion IN (1, 2)),
    unreadable_policy INTEGER NOT NULL CHECK (unreadable_policy = 1),
    display_label TEXT NOT NULL,
    PRIMARY KEY (schedule_id, ordinal),
    UNIQUE (schedule_id, selection_id),
    FOREIGN KEY (schedule_id) REFERENCES schedules(schedule_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS ix_schedule_file_selections_schedule
    ON schedule_file_selections(schedule_id, ordinal);
CREATE TABLE IF NOT EXISTS audit_events (
    event_id TEXT PRIMARY KEY NOT NULL,
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0),
    severity INTEGER NOT NULL CHECK (severity BETWEEN 1 AND 4),
    message_code TEXT NOT NULL,
    message_arguments TEXT NOT NULL,
    correlation_id TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_audit_created ON audit_events(created_utc_ms DESC, event_id ASC);
CREATE INDEX IF NOT EXISTS ix_audit_correlation ON audit_events(correlation_id, created_utc_ms DESC);
CREATE TABLE IF NOT EXISTS commands (
    idempotency_key TEXT PRIMARY KEY NOT NULL,
    request_fingerprint TEXT NOT NULL,
    command_id TEXT NOT NULL UNIQUE,
    resource_id TEXT,
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0)
);
CREATE TABLE IF NOT EXISTS restore_preflights (
    preflight_token TEXT PRIMARY KEY NOT NULL,
    repository_connection_id TEXT NOT NULL,
    repository_uuid TEXT NOT NULL,
    recovery_point_id TEXT NOT NULL,
    target_source_id TEXT NOT NULL,
    chain_fingerprint TEXT NOT NULL,
    logical_size_bytes INTEGER NOT NULL CHECK (logical_size_bytes >= 0),
    target_capacity_bytes INTEGER NOT NULL CHECK (target_capacity_bytes >= logical_size_bytes),
    chain_depth INTEGER NOT NULL CHECK (chain_depth > 0 AND chain_depth <= 4294967295),
    created_utc_ms INTEGER NOT NULL CHECK (created_utc_ms >= 0),
    expires_utc_ms INTEGER NOT NULL CHECK (expires_utc_ms > created_utc_ms)
);
CREATE INDEX IF NOT EXISTS ix_restore_preflights_expires
    ON restore_preflights(expires_utc_ms);
CREATE TABLE IF NOT EXISTS restore_preflight_entry_ids (
    preflight_token TEXT NOT NULL,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 10000),
    entry_id TEXT NOT NULL,
    PRIMARY KEY (preflight_token, ordinal),
    UNIQUE (preflight_token, entry_id),
    FOREIGN KEY (preflight_token) REFERENCES restore_preflights(preflight_token) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS ix_restore_preflight_entry_ids_token
    ON restore_preflight_entry_ids(preflight_token, ordinal);
)sql";
    return exec_sql(db, kSchema);
}

} // namespace aegra::adapters::sqlite::detail
