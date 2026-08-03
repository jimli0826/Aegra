#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {
AuditEventStore::AuditEventStore(SqliteControlPlaneState& state,
                                 const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> AuditEventStore::append(const ports::AuditEventRecord& record,
                                           const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto valid = validate_audit_event_record(record);
    if (!valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO audit_events(event_id, created_utc_ms, severity, message_code, "
        "message_arguments, correlation_id) VALUES(?,?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_text(1, record.event_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(2, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(3, static_cast<std::int64_t>(record.severity)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(4, record.message_code); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(5, encode_message_arguments(record.message_arguments));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(6, record.correlation_id); !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

base::Result<contracts::AuditEventPage>
AuditEventStore::list(const contracts::AuditEventListRequest& request,
                      const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<contracts::AuditEventPage>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<contracts::AuditEventPage>::failure(cancelled.error());
    }
    auto valid_page = contracts::validate_audit_event_list_request(request);
    if (!valid_page) {
        return base::Result<contracts::AuditEventPage>::failure(valid_page.error());
    }
    const auto filter =
        audit_event_page_filter(request.minimum_severity, request.from_utc_ms, request.to_utc_ms,
                                request.correlation_id);
    auto token = decode_page_token(request.page.continuation_token, kPageScopeAuditEvents, filter);
    if (!token) {
        return base::Result<contracts::AuditEventPage>::failure(token.error());
    }
    std::string sql =
        "SELECT event_id, created_utc_ms, severity, message_code, message_arguments, "
        "correlation_id FROM audit_events WHERE 1=1";
    if (request.minimum_severity) {
        sql += " AND severity >= ?";
    }
    if (request.from_utc_ms) {
        sql += " AND created_utc_ms >= ?";
    }
    if (request.to_utc_ms) {
        sql += " AND created_utc_ms <= ?";
    }
    if (request.correlation_id) {
        sql += " AND correlation_id = ?";
    }
    if (token.value()) {
        sql += " AND (created_utc_ms < ? OR (created_utc_ms = ? AND event_id > ?))";
    }
    sql += " ORDER BY created_utc_ms DESC, event_id ASC LIMIT ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return base::Result<contracts::AuditEventPage>::failure(statement.error());
    }
    int bind_index = 1;
    if (request.minimum_severity) {
        if (auto bound = statement.value().bind_int64(
                bind_index++, static_cast<std::int64_t>(*request.minimum_severity));
            !bound) {
            return base::Result<contracts::AuditEventPage>::failure(bound.error());
        }
    }
    if (request.from_utc_ms) {
        if (auto bound = statement.value().bind_int64(
                bind_index++, static_cast<std::int64_t>(*request.from_utc_ms));
            !bound) {
            return base::Result<contracts::AuditEventPage>::failure(bound.error());
        }
    }
    if (request.to_utc_ms) {
        if (auto bound = statement.value().bind_int64(
                bind_index++, static_cast<std::int64_t>(*request.to_utc_ms));
            !bound) {
            return base::Result<contracts::AuditEventPage>::failure(bound.error());
        }
    }
    if (request.correlation_id) {
        if (auto bound = statement.value().bind_text(bind_index++, *request.correlation_id); !bound) {
            return base::Result<contracts::AuditEventPage>::failure(bound.error());
        }
    }
    if (auto bound = bind_page_cursor(statement.value(), bind_index, token.value()); !bound) {
        return base::Result<contracts::AuditEventPage>::failure(bound.error());
    }
    if (auto bound = statement.value().bind_int64(
            bind_index, static_cast<std::int64_t>(request.page.maximum_results + 1U));
        !bound) {
        return base::Result<contracts::AuditEventPage>::failure(bound.error());
    }

    contracts::AuditEventPage page;
    std::optional<ports::AuditEventRecord> last_full;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<contracts::AuditEventPage>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_audit_event(statement.value().get());
        if (!record) {
            return base::Result<contracts::AuditEventPage>::failure(record.error());
        }
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token = encode_page_token(
                kPageScopeAuditEvents, filter, last_full->created_utc_ms, last_full->event_id);
            break;
        }
        last_full = record.value();
        page.items.push_back(to_audit_summary(record.value()));
    }
    return base::Result<contracts::AuditEventPage>::success(std::move(page));
}


} // namespace aegra::adapters::sqlite::detail

