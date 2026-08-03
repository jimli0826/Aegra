#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {

CommandStore::CommandStore(SqliteControlPlaneState& state,
                           const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<std::optional<ports::CommandRecord>>
CommandStore::get(const std::string_view idempotency_key,
                  const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::optional<ports::CommandRecord>>::failure(active.error());
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return base::Result<std::optional<ports::CommandRecord>>::failure(cancelled.error());
    }
    auto statement = SqliteStatement::prepare(state_.db, kSelectCommandSql);
    if (!statement) {
        return base::Result<std::optional<ports::CommandRecord>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, idempotency_key); !bound) {
        return base::Result<std::optional<ports::CommandRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::CommandRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::CommandRecord>>::success(std::nullopt);
    }
    auto record = read_command(statement.value().get());
    return record ? base::Result<std::optional<ports::CommandRecord>>::success(
                        std::move(record).value())
                  : base::Result<std::optional<ports::CommandRecord>>::failure(record.error());
}

base::Result<void> CommandStore::insert(const ports::CommandRecord& record,
                                        const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return cancelled;
    }
    if (auto valid = validate_command_record(record); !valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO commands(idempotency_key, request_fingerprint, command_id, resource_id, "
        "created_utc_ms) VALUES(?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_text(1, record.idempotency_key); !bound)
        return bound;
    if (auto bound = stmt.bind_text(2, record.request_fingerprint); !bound)
        return bound;
    if (auto bound = stmt.bind_text(3, record.command_id); !bound)
        return bound;
    if (auto bound = stmt.bind_text_nullable(4, record.resource_id); !bound)
        return bound;
    if (auto bound = stmt.bind_int64(5, static_cast<std::int64_t>(record.created_utc_ms)); !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    return stepped ? base::Result<void>::success() : base::Result<void>::failure(stepped.error());
}

} // namespace aegra::adapters::sqlite::detail
