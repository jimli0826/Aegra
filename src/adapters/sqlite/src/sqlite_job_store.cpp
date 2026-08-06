#include "sqlite_internal.h"

#include <limits>
#include <utility>

namespace aegra::adapters::sqlite::detail {
JobStore::JobStore(SqliteControlPlaneState& state, const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> JobStore::insert(const ports::JobRecord& record,
                                    const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto valid = validate_job_record(record);
    if (!valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO jobs(job_id, trace_id, operation, state, created_utc_ms, started_utc_ms, "
        "completed_utc_ms, source_ids, repository_connection_id, target_source_id, backup_type, "
        "parent_recovery_point_id, preflight_token, message_code, idempotency_key, "
        "result_error_code, result_outcome, result_message_code, "
        "exclude_page_and_hibernation_files, request_fingerprint) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_text(1, record.job_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(2, record.trace_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(3, static_cast<std::int64_t>(record.operation)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(4, static_cast<std::int64_t>(record.state)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(5, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64_nullable(6, record.started_utc_ms); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64_nullable(7, record.completed_utc_ms); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(8, encode_string_list(record.source_ids)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(9, record.repository_connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(10, record.target_source_id); !bound) {
        return bound;
    }
    if (record.backup_type) {
        if (auto bound = stmt.bind_int64(11, static_cast<std::int64_t>(*record.backup_type));
            !bound) {
            return bound;
        }
    } else if (auto bound = stmt.bind_null(11); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(12, record.parent_recovery_point_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(13, record.preflight_token); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(14, record.message_code); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(15, record.idempotency_key); !bound) {
        return bound;
    }
    if (record.result_error_code) {
        if (auto bound = stmt.bind_int64(16, static_cast<std::int64_t>(*record.result_error_code));
            !bound) {
            return bound;
        }
    } else if (auto bound = stmt.bind_null(16); !bound) {
        return bound;
    }
    if (record.result_outcome) {
        if (auto bound = stmt.bind_int64(17, static_cast<std::int64_t>(*record.result_outcome));
            !bound) {
            return bound;
        }
    } else if (auto bound = stmt.bind_null(17); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(18, record.result_message_code); !bound) {
        return bound;
    }
    if (record.exclude_page_and_hibernation_files) {
        if (auto bound =
                stmt.bind_int64(19, *record.exclude_page_and_hibernation_files ? 1 : 0);
            !bound) {
            return bound;
        }
    } else if (auto bound = stmt.bind_null(19); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(20, record.request_fingerprint); !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

base::Result<std::optional<ports::JobRecord>>
JobStore::get(const std::string_view job_id, const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::optional<ports::JobRecord>>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<std::optional<ports::JobRecord>>::failure(cancelled.error());
    }
    auto statement = SqliteStatement::prepare(state_.db, kSelectJobSql);
    if (!statement) {
        return base::Result<std::optional<ports::JobRecord>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, job_id); !bound) {
        return base::Result<std::optional<ports::JobRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::JobRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::JobRecord>>::success(std::nullopt);
    }
    auto record = read_job(statement.value().get());
    if (!record) {
        return base::Result<std::optional<ports::JobRecord>>::failure(record.error());
    }
    return base::Result<std::optional<ports::JobRecord>>::success(std::move(record.value()));
}

base::Result<contracts::JobPage> JobStore::list(const contracts::JobListRequest& request,
                                                const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<contracts::JobPage>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<contracts::JobPage>::failure(cancelled.error());
    }
    auto valid_page = contracts::validate_job_list_request(request);
    if (!valid_page) {
        return base::Result<contracts::JobPage>::failure(valid_page.error());
    }
    const auto filter = job_page_filter(request.operation, request.state);
    auto token = decode_page_token(request.page.continuation_token, kPageScopeJobs, filter);
    if (!token) {
        return base::Result<contracts::JobPage>::failure(token.error());
    }
    std::string sql =
        "SELECT job_id, trace_id, operation, state, created_utc_ms, started_utc_ms, "
        "completed_utc_ms, "
        "source_ids, repository_connection_id, target_source_id, backup_type, "
        "parent_recovery_point_id, "
        "preflight_token, message_code, idempotency_key, result_error_code, result_outcome, "
        "result_message_code, exclude_page_and_hibernation_files, request_fingerprint "
        "FROM jobs WHERE 1=1";
    if (request.operation) {
        sql += " AND operation = ?";
    }
    if (request.state) {
        sql += " AND state = ?";
    }
    if (token.value()) {
        sql += " AND (created_utc_ms < ? OR (created_utc_ms = ? AND job_id > ?))";
    }
    sql += " ORDER BY created_utc_ms DESC, job_id ASC LIMIT ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return base::Result<contracts::JobPage>::failure(statement.error());
    }
    int bind_index = 1;
    if (request.operation) {
        if (auto bound = statement.value().bind_int64(
                bind_index++, static_cast<std::int64_t>(*request.operation));
            !bound) {
            return base::Result<contracts::JobPage>::failure(bound.error());
        }
    }
    if (request.state) {
        if (auto bound = statement.value().bind_int64(bind_index++,
                                                      static_cast<std::int64_t>(*request.state));
            !bound) {
            return base::Result<contracts::JobPage>::failure(bound.error());
        }
    }
    if (auto bound = bind_page_cursor(statement.value(), bind_index, token.value()); !bound) {
        return base::Result<contracts::JobPage>::failure(bound.error());
    }
    if (auto bound = statement.value().bind_int64(
            bind_index, static_cast<std::int64_t>(request.page.maximum_results + 1U));
        !bound) {
        return base::Result<contracts::JobPage>::failure(bound.error());
    }

    contracts::JobPage page;
    std::optional<ports::JobRecord> last_full;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<contracts::JobPage>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_job(statement.value().get());
        if (!record) {
            return base::Result<contracts::JobPage>::failure(record.error());
        }
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token = encode_page_token(
                kPageScopeJobs, filter, last_full->created_utc_ms, last_full->job_id);
            break;
        }
        last_full = record.value();
        page.items.push_back(to_job_summary(record.value()));
    }
    return base::Result<contracts::JobPage>::success(std::move(page));
}

base::Result<ports::JobRecord> JobStore::transition(const ports::JobStateTransition& transition,
                                                    const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<ports::JobRecord>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<ports::JobRecord>::failure(cancelled.error());
    }
    auto valid = validate_job_transition(transition);
    if (!valid) {
        return base::Result<ports::JobRecord>::failure(valid.error());
    }
    auto current = get(transition.job_id, cancellation);
    if (!current) {
        return base::Result<ports::JobRecord>::failure(current.error());
    }
    if (!current.value()) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kNotFound, "job not found"));
    }
    if (current.value()->state != transition.expected_state) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kConflict, "job state does not match expected state"));
    }
    if (transition.transition_utc_ms < current.value()->created_utc_ms) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "transition timestamp precedes create"));
    }
    if (current.value()->started_utc_ms &&
        transition.transition_utc_ms < *current.value()->started_utc_ms) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "transition timestamp precedes start"));
    }

    std::optional<std::uint64_t> started = current.value()->started_utc_ms;
    std::optional<std::uint64_t> completed = current.value()->completed_utc_ms;
    if (transition.next_state == contracts::ServiceJobState::kRunning ||
        (transition.next_state == contracts::ServiceJobState::kCancelling && !started) ||
        (transition.expected_state == contracts::ServiceJobState::kQueued &&
         transition.next_state == contracts::ServiceJobState::kInterrupted)) {
        if (transition.next_state == contracts::ServiceJobState::kRunning) {
            started = transition.transition_utc_ms;
        }
    }
    if (transition.expected_state == contracts::ServiceJobState::kQueued &&
        transition.next_state == contracts::ServiceJobState::kCancelled) {
        started = transition.transition_utc_ms;
        completed = transition.transition_utc_ms;
    } else if (transition.expected_state == contracts::ServiceJobState::kQueued &&
               transition.next_state == contracts::ServiceJobState::kInterrupted) {
        started = transition.transition_utc_ms;
        completed = transition.transition_utc_ms;
    } else if (ports::is_terminal_job_state(transition.next_state)) {
        if (!started) {
            started = transition.transition_utc_ms;
        }
        completed = transition.transition_utc_ms;
    }

    auto statement = SqliteStatement::prepare(
        state_.db,
        "UPDATE jobs SET state = ?, started_utc_ms = ?, completed_utc_ms = ?, message_code = ?, "
        "result_error_code = ?, result_outcome = ?, result_message_code = ? "
        "WHERE job_id = ? AND state = ?");
    if (!statement) {
        return base::Result<ports::JobRecord>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_int64(1, static_cast<std::int64_t>(transition.next_state)); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64_nullable(2, started); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64_nullable(3, completed); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_text(4, transition.message_code); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (transition.result_error_code) {
        if (auto bound =
                stmt.bind_int64(5, static_cast<std::int64_t>(*transition.result_error_code));
            !bound) {
            return base::Result<ports::JobRecord>::failure(bound.error());
        }
    } else if (auto bound = stmt.bind_null(5); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (transition.result_outcome) {
        if (auto bound = stmt.bind_int64(6, static_cast<std::int64_t>(*transition.result_outcome));
            !bound) {
            return base::Result<ports::JobRecord>::failure(bound.error());
        }
    } else if (auto bound = stmt.bind_null(6); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_text_nullable(7, transition.result_message_code); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_text(8, transition.job_id); !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64(9, static_cast<std::int64_t>(transition.expected_state));
        !bound) {
        return base::Result<ports::JobRecord>::failure(bound.error());
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<ports::JobRecord>::failure(stepped.error());
    }
    if (sqlite3_changes(state_.db) != 1) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kConflict, "job state does not match expected state"));
    }
    auto updated = get(transition.job_id, cancellation);
    if (!updated || !updated.value()) {
        return base::Result<ports::JobRecord>::failure(
            make_error(base::ErrorCode::kInternal, "job missing after transition"));
    }
    return base::Result<ports::JobRecord>::success(std::move(*updated.value()));
}

base::Result<std::uint64_t>
JobStore::mark_active_as_interrupted(const std::uint64_t interrupted_utc_ms,
                                     const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::uint64_t>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<std::uint64_t>::failure(cancelled.error());
    }
    if (interrupted_utc_ms >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return base::Result<std::uint64_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "interrupt timestamp is invalid"));
    }
    auto statement =
        SqliteStatement::prepare(state_.db, "UPDATE jobs SET state = ?, "
                                            "started_utc_ms = COALESCE(started_utc_ms, ?), "
                                            "completed_utc_ms = ?, "
                                            "message_code = 'job.interrupted', "
                                            "result_message_code = 'job.interrupted' "
                                            "WHERE state IN (?, ?, ?)");
    if (!statement) {
        return base::Result<std::uint64_t>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound =
            stmt.bind_int64(1, static_cast<std::int64_t>(contracts::ServiceJobState::kInterrupted));
        !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64(2, static_cast<std::int64_t>(interrupted_utc_ms)); !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64(3, static_cast<std::int64_t>(interrupted_utc_ms)); !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    if (auto bound =
            stmt.bind_int64(4, static_cast<std::int64_t>(contracts::ServiceJobState::kQueued));
        !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    if (auto bound =
            stmt.bind_int64(5, static_cast<std::int64_t>(contracts::ServiceJobState::kRunning));
        !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    if (auto bound =
            stmt.bind_int64(6, static_cast<std::int64_t>(contracts::ServiceJobState::kCancelling));
        !bound) {
        return base::Result<std::uint64_t>::failure(bound.error());
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<std::uint64_t>::failure(stepped.error());
    }
    return base::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(sqlite3_changes(state_.db)));
}

} // namespace aegra::adapters::sqlite::detail
