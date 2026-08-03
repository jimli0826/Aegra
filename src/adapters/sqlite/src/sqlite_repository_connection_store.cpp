#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr const char* kDefaultUpsertSavepoint = "sp_default_connection";

[[nodiscard]] base::Result<void>
bind_connection_values(SqliteStatement& stmt, const ports::RepositoryConnectionRecord& record) {
    if (auto bound = stmt.bind_text(1, record.connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(2, record.display_name); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(3, record.locator); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(
            4, record.credential_ref ? std::optional<std::string>{record.credential_ref->value}
                                     : std::nullopt);
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(5, static_cast<std::int64_t>(record.state)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(6, record.is_default ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(7, encode_string_list(record.capabilities)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(8, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    return stmt.bind_int64(9, static_cast<std::int64_t>(record.updated_utc_ms));
}

} // namespace

RepositoryConnectionStore::RepositoryConnectionStore(
    SqliteControlPlaneState& state, const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void>
RepositoryConnectionStore::upsert(const ports::RepositoryConnectionRecord& record,
                                  const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto valid = validate_repository_connection_record(record);
    if (!valid) {
        return valid;
    }
    if (record.is_default) {
        auto savepoint = begin_savepoint(state_.db, kDefaultUpsertSavepoint);
        if (!savepoint) {
            return savepoint;
        }
        auto cleared = clear_default_repository_flags(state_.db);
        if (!cleared) {
            rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
            return cleared;
        }
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO repository_connections("
        "connection_id, display_name, locator, credential_ref, state, is_default, capabilities, "
        "created_utc_ms, updated_utc_ms) VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(connection_id) DO UPDATE SET "
        "display_name=excluded.display_name, locator=excluded.locator, "
        "credential_ref=excluded.credential_ref, state=excluded.state, "
        "is_default=excluded.is_default, capabilities=excluded.capabilities, "
        "updated_utc_ms=excluded.updated_utc_ms");
    if (!statement) {
        if (record.is_default) {
            rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        }
        return base::Result<void>::failure(statement.error());
    }
    auto bound = bind_connection_values(statement.value(), record);
    if (!bound) {
        if (record.is_default) {
            rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        }
        return bound;
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        if (record.is_default) {
            rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        }
        return base::Result<void>::failure(stepped.error());
    }
    if (record.is_default) {
        return release_savepoint(state_.db, kDefaultUpsertSavepoint);
    }
    return base::Result<void>::success();
}

base::Result<std::optional<ports::RepositoryConnectionRecord>>
RepositoryConnectionStore::get(const std::string_view connection_id,
                               const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            cancelled.error());
    }
    auto statement = SqliteStatement::prepare(state_.db, kSelectConnectionSql);
    if (!statement) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            statement.error());
    }
    if (auto bound = statement.value().bind_text(1, connection_id); !bound) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(
            std::nullopt);
    }
    auto record = read_repository_connection(statement.value().get());
    if (!record) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            record.error());
    }
    return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(
        std::move(record.value()));
}

base::Result<contracts::RepositoryConnectionPage>
RepositoryConnectionStore::list(const contracts::RepositoryConnectionListRequest& request,
                                const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(active.error());
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(cancelled.error());
    }
    auto valid_page = contracts::validate_repository_connection_list_request(request);
    if (!valid_page) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(valid_page.error());
    }
    const auto filter = repository_connection_page_filter(request.state);
    auto token =
        decode_page_token(request.page.continuation_token, kPageScopeRepositoryConnections, filter);
    if (!token) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(token.error());
    }
    std::string sql =
        "SELECT connection_id, display_name, locator, credential_ref, state, is_default, "
        "capabilities, created_utc_ms, updated_utc_ms FROM repository_connections WHERE 1=1";
    if (request.state) {
        sql += " AND state = ?";
    }
    if (token.value()) {
        sql += " AND (created_utc_ms < ? OR (created_utc_ms = ? AND connection_id > ?))";
    }
    sql += " ORDER BY created_utc_ms DESC, connection_id ASC LIMIT ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(statement.error());
    }
    int bind_index = 1;
    if (request.state) {
        if (auto bound = statement.value().bind_int64(bind_index++,
                                                      static_cast<std::int64_t>(*request.state));
            !bound) {
            return base::Result<contracts::RepositoryConnectionPage>::failure(bound.error());
        }
    }
    if (auto bound = bind_page_cursor(statement.value(), bind_index, token.value()); !bound) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(bound.error());
    }
    if (auto bound = statement.value().bind_int64(
            bind_index, static_cast<std::int64_t>(request.page.maximum_results + 1U));
        !bound) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(bound.error());
    }

    contracts::RepositoryConnectionPage page;
    std::optional<ports::RepositoryConnectionRecord> last_full;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<contracts::RepositoryConnectionPage>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_repository_connection(statement.value().get());
        if (!record) {
            return base::Result<contracts::RepositoryConnectionPage>::failure(record.error());
        }
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token =
                encode_page_token(kPageScopeRepositoryConnections, filter,
                                  last_full->created_utc_ms, last_full->connection_id);
            break;
        }
        last_full = record.value();
        page.items.push_back(to_connection_summary(record.value()));
    }
    return base::Result<contracts::RepositoryConnectionPage>::success(std::move(page));
}

base::Result<void> RepositoryConnectionStore::set_default(const std::string_view connection_id,
                                                          const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto existing = get(connection_id, cancellation);
    if (!existing) {
        return base::Result<void>::failure(existing.error());
    }
    if (!existing.value()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kNotFound, "repository connection not found"));
    }
    auto savepoint = begin_savepoint(state_.db, kDefaultUpsertSavepoint);
    if (!savepoint) {
        return savepoint;
    }
    auto cleared = clear_default_repository_flags(state_.db);
    if (!cleared) {
        rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        return cleared;
    }
    auto statement = SqliteStatement::prepare(
        state_.db, "UPDATE repository_connections SET is_default = 1 WHERE connection_id = ?");
    if (!statement) {
        rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        return base::Result<void>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, connection_id); !bound) {
        rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        return bound;
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        return base::Result<void>::failure(stepped.error());
    }
    if (sqlite3_changes(state_.db) != 1) {
        rollback_savepoint(state_.db, kDefaultUpsertSavepoint);
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kNotFound, "repository connection not found"));
    }
    return release_savepoint(state_.db, kDefaultUpsertSavepoint);
}

base::Result<void> RepositoryConnectionStore::remove(const std::string_view connection_id,
                                                     const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    auto statement =
        SqliteStatement::prepare(state_.db, "DELETE FROM repository_connections WHERE connection_id = ?");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, connection_id); !bound) {
        return bound;
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    if (sqlite3_changes(state_.db) != 1) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kNotFound, "repository connection not found"));
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::sqlite::detail
