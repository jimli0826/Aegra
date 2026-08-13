#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {
namespace {

[[nodiscard]] base::Result<ports::ScheduleRecord>
attach_file_selections(sqlite3* const db, ports::ScheduleRecord record) {
    if (record.content_kind != contracts::ContentKind::kFileSet) {
        auto valid = validate_schedule_record(record);
        return valid ? base::Result<ports::ScheduleRecord>::success(std::move(record))
                     : base::Result<ports::ScheduleRecord>::failure(valid.error());
    }
    auto selections = load_schedule_file_selections(db, record.schedule_id);
    if (!selections) {
        return base::Result<ports::ScheduleRecord>::failure(selections.error());
    }
    record.file_selections = std::move(selections).value();
    auto valid = validate_schedule_record(record);
    return valid ? base::Result<ports::ScheduleRecord>::success(std::move(record))
                 : base::Result<ports::ScheduleRecord>::failure(valid.error());
}

} // namespace

ScheduleStore::ScheduleStore(SqliteControlPlaneState& state,
                             const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> ScheduleStore::upsert(const ports::ScheduleRecord& record,
                                         const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto valid = validate_schedule_record(record);
    if (!valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO schedules(schedule_id, display_name, enabled, content_kind, source_ids, "
        "owner_sid, repository_connection_id, backup_type, trigger_kind, local_minutes_of_day, "
        "weekday_mask, day_of_month_mask, timezone_id, next_run_utc_ms, "
        "exclude_page_and_hibernation_files, "
        "deduplication_enabled, encryption_enabled, archive_password_protected, backup_set_uuid, "
        "last_recovery_point_id, created_utc_ms, updated_utc_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(schedule_id) DO UPDATE SET "
        "display_name=excluded.display_name, enabled=excluded.enabled, "
        "content_kind=excluded.content_kind, source_ids=excluded.source_ids, "
        "owner_sid=excluded.owner_sid, "
        "repository_connection_id=excluded.repository_connection_id, "
        "backup_type=excluded.backup_type, trigger_kind=excluded.trigger_kind, "
        "local_minutes_of_day=excluded.local_minutes_of_day, weekday_mask=excluded.weekday_mask, "
        "day_of_month_mask=excluded.day_of_month_mask, "
        "timezone_id=excluded.timezone_id, next_run_utc_ms=excluded.next_run_utc_ms, "
        "exclude_page_and_hibernation_files=excluded.exclude_page_and_hibernation_files, "
        "deduplication_enabled=excluded.deduplication_enabled, "
        "encryption_enabled=excluded.encryption_enabled, "
        "archive_password_protected=excluded.archive_password_protected, "
        "backup_set_uuid=excluded.backup_set_uuid, "
        "last_recovery_point_id=excluded.last_recovery_point_id, "
        "updated_utc_ms=excluded.updated_utc_ms");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_text(1, record.schedule_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(2, record.display_name); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(3, record.enabled ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(4, static_cast<std::int64_t>(record.content_kind)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(5, encode_string_list(record.source_ids)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(6, record.owner_sid); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(7, record.repository_connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(8, static_cast<std::int64_t>(record.backup_type)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(9, static_cast<std::int64_t>(record.trigger.kind)); !bound) {
        return bound;
    }
    if (auto bound =
            stmt.bind_text(10, encode_local_minutes_of_day(record.trigger.local_minutes_of_day));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(11, record.trigger.weekday_mask); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(12, record.trigger.day_of_month_mask); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(13, record.trigger.timezone_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64_nullable(14, record.next_run_utc_ms); !bound) {
        return bound;
    }
    if (auto bound =
            stmt.bind_int64(15, record.exclude_page_and_hibernation_files ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(16, record.deduplication_enabled ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(17, record.encryption_enabled ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(18, record.archive_password_protected); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(19, record.backup_set_uuid); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(20, record.last_recovery_point_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(21, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(22, static_cast<std::int64_t>(record.updated_utc_ms)); !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    if (record.content_kind == contracts::ContentKind::kFileSet) {
        auto replaced =
            replace_schedule_file_selections(state_.db, record.schedule_id, record.file_selections);
        if (!replaced) {
            return replaced;
        }
    } else {
        auto cleared =
            replace_schedule_file_selections(state_.db, record.schedule_id, {});
        if (!cleared) {
            return cleared;
        }
    }
    return base::Result<void>::success();
}

base::Result<std::optional<ports::ScheduleRecord>>
ScheduleStore::get(const std::string_view schedule_id, const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(cancelled.error());
    }
    auto statement = SqliteStatement::prepare(state_.db, kSelectScheduleSql);
    if (!statement) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, schedule_id); !bound) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::ScheduleRecord>>::success(std::nullopt);
    }
    auto record = read_schedule(statement.value().get());
    if (!record) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(record.error());
    }
    auto attached = attach_file_selections(state_.db, std::move(record).value());
    if (!attached) {
        return base::Result<std::optional<ports::ScheduleRecord>>::failure(attached.error());
    }
    return base::Result<std::optional<ports::ScheduleRecord>>::success(std::move(attached).value());
}

base::Result<contracts::SchedulePage>
ScheduleStore::list(const contracts::ScheduleListRequest& request,
                    const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<contracts::SchedulePage>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<contracts::SchedulePage>::failure(cancelled.error());
    }
    auto valid_page = contracts::validate_schedule_list_request(request);
    if (!valid_page) {
        return base::Result<contracts::SchedulePage>::failure(valid_page.error());
    }
    const auto filter = schedule_page_filter(request.enabled);
    auto token = decode_page_token(request.page.continuation_token, kPageScopeSchedules, filter);
    if (!token) {
        return base::Result<contracts::SchedulePage>::failure(token.error());
    }
    std::string sql =
        "SELECT schedule_id, display_name, enabled, content_kind, source_ids, owner_sid, "
        "repository_connection_id, backup_type, trigger_kind, local_minutes_of_day, weekday_mask, "
        "day_of_month_mask, timezone_id, next_run_utc_ms, exclude_page_and_hibernation_files, "
        "deduplication_enabled, encryption_enabled, archive_password_protected, backup_set_uuid, "
        "last_recovery_point_id, created_utc_ms, updated_utc_ms FROM schedules WHERE 1=1";
    if (request.enabled) {
        sql += " AND enabled = ?";
    }
    if (token.value()) {
        sql += " AND (created_utc_ms < ? OR (created_utc_ms = ? AND schedule_id > ?))";
    }
    sql += " ORDER BY created_utc_ms DESC, schedule_id ASC LIMIT ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return base::Result<contracts::SchedulePage>::failure(statement.error());
    }
    int bind_index = 1;
    if (request.enabled) {
        if (auto bound = statement.value().bind_int64(bind_index++, *request.enabled ? 1 : 0);
            !bound) {
            return base::Result<contracts::SchedulePage>::failure(bound.error());
        }
    }
    if (auto bound = bind_page_cursor(statement.value(), bind_index, token.value()); !bound) {
        return base::Result<contracts::SchedulePage>::failure(bound.error());
    }
    if (auto bound = statement.value().bind_int64(
            bind_index, static_cast<std::int64_t>(request.page.maximum_results + 1U));
        !bound) {
        return base::Result<contracts::SchedulePage>::failure(bound.error());
    }

    contracts::SchedulePage page;
    std::optional<ports::ScheduleRecord> last_full;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<contracts::SchedulePage>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_schedule(statement.value().get());
        if (!record) {
            return base::Result<contracts::SchedulePage>::failure(record.error());
        }
        auto attached = attach_file_selections(state_.db, std::move(record).value());
        if (!attached) {
            return base::Result<contracts::SchedulePage>::failure(attached.error());
        }
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token =
                encode_page_token(kPageScopeSchedules, filter, last_full->created_utc_ms,
                                  last_full->schedule_id);
            break;
        }
        last_full = attached.value();
        page.items.push_back(to_schedule_summary(attached.value()));
    }
    return base::Result<contracts::SchedulePage>::success(std::move(page));
}

base::Result<void> ScheduleStore::remove(const std::string_view schedule_id,
                                         const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto statement = SqliteStatement::prepare(state_.db, "DELETE FROM schedules WHERE schedule_id = ?");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, schedule_id); !bound) {
        return bound;
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    if (sqlite3_changes(state_.db) != 1) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kNotFound, "schedule not found"));
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::sqlite::detail
