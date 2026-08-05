#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {
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
        "INSERT INTO schedules(schedule_id, display_name, enabled, source_ids, "
        "repository_connection_id, backup_type, trigger_kind, local_minute_of_day, weekday_mask, "
        "timezone_id, next_run_utc_ms, exclude_page_and_hibernation_files, created_utc_ms, "
        "updated_utc_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(schedule_id) DO UPDATE SET "
        "display_name=excluded.display_name, enabled=excluded.enabled, source_ids=excluded.source_ids, "
        "repository_connection_id=excluded.repository_connection_id, "
        "backup_type=excluded.backup_type, trigger_kind=excluded.trigger_kind, "
        "local_minute_of_day=excluded.local_minute_of_day, weekday_mask=excluded.weekday_mask, "
        "timezone_id=excluded.timezone_id, next_run_utc_ms=excluded.next_run_utc_ms, "
        "exclude_page_and_hibernation_files=excluded.exclude_page_and_hibernation_files, "
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
    if (auto bound = stmt.bind_text(4, encode_string_list(record.source_ids)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(5, record.repository_connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(6, static_cast<std::int64_t>(record.backup_type)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(7, static_cast<std::int64_t>(record.trigger.kind)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(8, record.trigger.local_minute_of_day); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(9, record.trigger.weekday_mask); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(10, record.trigger.timezone_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64_nullable(11, record.next_run_utc_ms); !bound) {
        return bound;
    }
    if (auto bound =
            stmt.bind_int64(12, record.exclude_page_and_hibernation_files ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(13, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(14, static_cast<std::int64_t>(record.updated_utc_ms)); !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
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
    return base::Result<std::optional<ports::ScheduleRecord>>::success(std::move(record.value()));
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
        "SELECT schedule_id, display_name, enabled, source_ids, repository_connection_id, "
        "backup_type, trigger_kind, local_minute_of_day, weekday_mask, timezone_id, "
        "next_run_utc_ms, exclude_page_and_hibernation_files, created_utc_ms, updated_utc_ms "
        "FROM schedules WHERE 1=1";
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
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token =
                encode_page_token(kPageScopeSchedules, filter, last_full->created_utc_ms,
                                  last_full->schedule_id);
            break;
        }
        last_full = record.value();
        page.items.push_back(to_schedule_summary(record.value()));
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

